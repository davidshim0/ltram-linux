#!/usr/bin/env python3
"""Write-cold against cold, as a function of the threshold T.

The motivation measurement. Cold-page tiering (Google's software-defined far
memory, Meta's TMO) moves pages NOT ACCESSED for T seconds. LtRAM can move
pages NOT WRITTEN for T seconds, which is a superset by construction: every
cold page is write-cold, but a page read a million times a second and never
written is write-cold and not cold.

    sudo ./rw_census.py --pid 1234 --interval 120 --run 600
    sudo ./rw_census.py --cmd './pagerank -g 20 -n 100000' --interval 120 --run 600

HOW T IS DERIVED
    Sampling every S seconds gives, per window, the set of pages written and
    the set accessed. Recording each page's LAST written window and LAST
    accessed window is then enough to answer every T that is a multiple of S,
    from the same trace -- so 20 points on the x-axis cost one run, not twenty.

    S is therefore the smallest reportable T. Sampling at 120 s cannot say
    anything about T=1; that needs S=1, and pays for it in perturbation,
    because clear_refs walks the page tables and write-protects every page.

WHAT IS REPORTED
    Not the decay from a single starting point, which falls to zero for any
    workload that eventually touches everything. The steady-state statistic:
    at each window boundary, what fraction of pages have not been written in
    the preceding T seconds -- averaged over all boundaries. That is the "X% of
    memory is write-cold at threshold T" the figure claims.

    written    /proc/PID/clear_refs = 4 clears soft-dirty; any store sets it.
    accessed   /sys/kernel/mm/page_idle/bitmap. Marking idle means any access
               clears the bit. Root only: PFNs are privileged, and there is no
               unprivileged substitute -- mincore gives residency not access,
               clear_refs=1 cannot be read back, userfaultfd catches writes.
               Without root the accessed column is skipped and the write-cold
               curve, which is the half the argument rests on, still works.
"""
import argparse, os, shlex, struct, sys, time, subprocess, signal
import numpy as np

PAGE = os.sysconf("SC_PAGE_SIZE")
PM_SOFT_DIRTY, PM_PRESENT = 1 << 55, 1 << 63
PFN_MASK = (1 << 55) - 1

ap = argparse.ArgumentParser()
ap.add_argument("--pid", type=int)
ap.add_argument("--cmd")
ap.add_argument("--label")
ap.add_argument("--interval", type=int, default=120, help="S: sampling window, and the smallest T")
ap.add_argument("--run", type=int, default=600, help="total seconds to observe")
ap.add_argument("--warmup", type=int, default=30)
ap.add_argument("--anon-only", action="store_true")
ap.add_argument("--out", default="-")
a = ap.parse_args()

proc = None
if a.cmd:
    # NOT shell=True. It returns the SHELL's pid -- dash forks rather than
    # execs here -- so the census silently measured a 972 kB shell instead of a
    # 263 MB workload and reported 461 pages. Exec the argv directly and the
    # pid is the workload's. Anything needing shell syntax should be started
    # separately and passed with --pid.
    proc = subprocess.Popen(shlex.split(a.cmd), preexec_fn=os.setsid)
    pid = proc.pid
    print(f"# launched pid {pid}: {a.cmd}", file=sys.stderr)
    time.sleep(a.warmup)
elif a.pid: pid = a.pid
else: sys.exit("need --pid or --cmd")
label = a.label or (a.cmd.split()[0].split("/")[-1] if a.cmd else f"pid{pid}")

HAVE_IDLE = os.geteuid() == 0 and os.path.exists("/sys/kernel/mm/page_idle/bitmap")

def vmas():
    out = []
    with open(f"/proc/{pid}/maps") as f:
        for line in f:
            p = line.split(); name = p[5] if len(p) > 5 else ""
            if name in ("[vsyscall]", "[vvar]", "[vdso]"): continue
            if a.anon_only and name and not name.startswith("["): continue
            lo, hi = (int(x, 16) for x in p[0].split("-"))
            out.append((lo, hi))
    return out

def pagemap(lo, hi):
    with open(f"/proc/{pid}/pagemap", "rb") as f:
        f.seek((lo // PAGE) * 8)
        buf = f.read(((hi - lo) // PAGE) * 8)
    return struct.unpack(f"<{len(buf)//8}Q", buf)

def idle_write(pfns):
    w = {}
    for p in pfns: w[p >> 6] = w.get(p >> 6, 0) | (1 << (p & 63))
    with open("/sys/kernel/mm/page_idle/bitmap", "r+b") as f:
        for k, v in w.items():
            f.seek(k * 8); f.write(struct.pack("<Q", v))

def idle_read(pfns):
    need = sorted({p >> 6 for p in pfns}); words = {}
    with open("/sys/kernel/mm/page_idle/bitmap", "rb") as f:
        for k in need:
            f.seek(k * 8); d = f.read(8)
            if len(d) == 8: words[k] = struct.unpack("<Q", d)[0]
    return {p for p in pfns if words.get(p >> 6, 0) & (1 << (p & 63))}

# Fix the page set at t=0 and follow those pages. A workload whose footprint
# grows mid-run would otherwise change the denominator underneath the curve.
regions = vmas()
va_pfn = {}
for lo, hi in regions:
    try: ents = pagemap(lo, hi)
    except Exception: continue
    for i, e in enumerate(ents):
        if e & PM_PRESENT: va_pfn[lo + i * PAGE] = e & PFN_MASK
N = len(va_pfn)
if not N: sys.exit("no resident pages")
# Cross-check the snapshot against the kernel's own RSS. Measuring the wrong
# process, or snapshotting before a workload has faulted its memory in, both
# show up here as a large shortfall -- and both are silent otherwise.
try:
    rss_pages = int(open(f"/proc/{pid}/statm").read().split()[1])
    if rss_pages and N < 0.5 * rss_pages:
        print(f"!! snapshot has {N:,} pages but RSS is {rss_pages:,} -- measuring the "
              f"wrong process, or it has not faulted in yet. Refusing.", file=sys.stderr)
        if proc:
            try: os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
            except Exception: pass
        sys.exit(2)
except FileNotFoundError:
    pass
order = sorted(va_pfn)
idx = {va: i for i, va in enumerate(order)}
pfns = [va_pfn[va] for va in order]

NW = a.run // a.interval
if NW < 2: sys.exit(f"--run {a.run} gives only {NW} window(s) at S={a.interval}; need >= 2")
NEVER = -10**9
last_w = np.full(N, NEVER, dtype=np.int32)   # last window the page was written
last_a = np.full(N, NEVER, dtype=np.int32)   # ... accessed
pfn_arr = np.array(pfns, dtype=np.int64)
acc_wc = {}; acc_cold = {}; nobs = {}

print(f"# {label}: {N:,} pages = {N*PAGE/2**20:.0f} MiB, S={a.interval}s, "
      f"{NW} windows, idle tracking {'on' if HAVE_IDLE else 'OFF (needs root)'}", file=sys.stderr)

for w in range(NW):
    try:
        with open(f"/proc/{pid}/clear_refs", "w") as f: f.write("4")
        if HAVE_IDLE: idle_write([p for p in pfns if p])
    except (FileNotFoundError, ProcessLookupError): break
    time.sleep(a.interval)
    if proc and proc.poll() is not None:
        print(f"# workload exited during window {w}", file=sys.stderr); break
    try:
        for lo, hi in regions:
            try:
                with open(f"/proc/{pid}/pagemap", "rb") as f:
                    f.seek((lo // PAGE) * 8)
                    raw = f.read(((hi - lo) // PAGE) * 8)
            except Exception: continue
            e = np.frombuffer(raw, dtype=np.uint64)
            hit = np.nonzero(((e & np.uint64(PM_PRESENT)) != 0) &
                             ((e & np.uint64(PM_SOFT_DIRTY)) != 0))[0]
            for i in hit:
                j = idx.get(lo + int(i) * PAGE)
                if j is not None: last_w[j] = w
        if HAVE_IDLE:
            still = idle_read([p for p in pfns if p])
            for i, p in enumerate(pfns):
                if p and p not in still: last_a[i] = w
    except (FileNotFoundError, ProcessLookupError): break
    # Every T that is a multiple of S, evaluated at this boundary.
    # Counting by prefix sum over a histogram of last-written windows, so a
    # boundary costs O(windows) rather than O(pages) per T.
    hw = np.bincount(np.clip(last_w - (w - NW), 0, None).astype(np.int64), minlength=NW + 2)
    ha = np.bincount(np.clip(last_a - (w - NW), 0, None).astype(np.int64), minlength=NW + 2)
    cw, ca = np.cumsum(hw), np.cumsum(ha)
    for k in range(1, w + 2):
        T = k * a.interval
        b = (w - k) - (w - NW)
        wc = int(cw[b]) if b >= 0 else 0
        cd = (int(ca[b]) if b >= 0 else 0) if HAVE_IDLE else 0
        acc_wc[T] = acc_wc.get(T, 0) + wc
        acc_cold[T] = acc_cold.get(T, 0) + cd
        nobs[T] = nobs.get(T, 0) + 1
    print(f"# window {w+1}/{NW} done", file=sys.stderr)

out = sys.stdout if a.out == "-" else open(a.out, "w")
# csv.writer, not an f-string: a label like "redis (ycsb-c, 95/5)" carries a
# comma, and hand-formatted rows shift every column after it by one.
import csv as _csv
w_ = _csv.writer(out, lineterminator="\n")
w_.writerow(["workload", "T_sec", "pages", "write_cold_pct", "cold_pct",
             "windows_averaged"])
for T in sorted(acc_wc):
    n = nobs[T]
    wc = 100.0 * acc_wc[T] / n / N
    # -1, not nan: it round-trips through CSV and the plotter tests for it.
    cd = (100.0 * acc_cold[T] / n / N) if HAVE_IDLE else -1.0
    w_.writerow([label, T, N, f"{wc:.2f}", f"{cd:.2f}", n])
if proc:
    try: os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except Exception: pass
