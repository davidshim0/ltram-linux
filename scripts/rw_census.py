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
    # The workload's own output is not the measurement. GAPBS prints a line
    # per trial and llama.cpp prints every token, either of which buries the
    # census's progress and the "workload exited" warning that matters.
    clog = (a.out + ".workload.log") if a.out != "-" else os.devnull
    proc = subprocess.Popen(shlex.split(a.cmd), preexec_fn=os.setsid,
                            stdout=open(clog, "w"), stderr=subprocess.STDOUT)
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

# Re-enumerate every window rather than fixing the page set at t=0.
#
# Fixing it was wrong in a way that flattered the result. GAPBS pagerank frees
# its 32 MiB score array between trials, so a snapshot that lands in that gap
# permanently excludes 8,203 pages which are written 100% of the time -- and
# the workload came out 99.92% write-cold instead of ~94%. Any page not
# resident at one arbitrary instant was dropped from the denominator forever.
#
# So: the denominator is the pages resident AT EACH BOUNDARY, and a page that
# appears for the first time counts as written in that window. First touch of
# an anon page is a write, and for a read-faulted file page it merely denies
# the page a history it never had. Both directions of that choice are
# conservative -- they can only lower the write-cold fraction we claim.
NEVER = -10 ** 9

def regions_now():
    return tuple(vmas())

layout, off, total = (), {}, 0
last_w = np.zeros(0, dtype=np.int32)
last_a = np.zeros(0, dtype=np.int32)
# "Never written" and "never seen before" are different facts, and using one
# sentinel for both made window 0 mark every resident page as freshly written
# -- including a 522 MiB read-only graph -- so nothing was write-cold at the
# longest T. The baseline population is seen but unwritten.
seen = np.zeros(0, dtype=bool)

def relayout(regs, w):
    """Rebuild the slot mapping, carrying each surviving page's history by VA."""
    global layout, off, total, last_w, last_a, seen
    old_va = {}
    if layout:
        for (lo, hi) in layout:
            b = off[(lo, hi)]
            for k in range((hi - lo) // PAGE):
                if seen[b + k]:
                    old_va[lo + k * PAGE] = (last_w[b + k], last_a[b + k])
    off = {}; total = 0
    for (lo, hi) in regs:
        off[(lo, hi)] = total; total += (hi - lo) // PAGE
    last_w = np.full(total, NEVER, dtype=np.int32)
    last_a = np.full(total, NEVER, dtype=np.int32)
    seen = np.zeros(total, dtype=bool)
    if old_va:
        for (lo, hi) in regs:
            b = off[(lo, hi)]
            for va, (vw, va_) in old_va.items():
                if lo <= va < hi:
                    last_w[b + (va - lo) // PAGE] = vw
                    last_a[b + (va - lo) // PAGE] = va_
                    seen[b + (va - lo) // PAGE] = True
    layout = regs

def scan(regs):
    """present, soft-dirty and pfn slots for the current layout, vectorised."""
    pres, dirty, pfn_of = [], [], {}
    with open(f"/proc/{pid}/pagemap", "rb") as f:
        for (lo, hi) in regs:
            try:
                f.seek((lo // PAGE) * 8)
                raw = f.read(((hi - lo) // PAGE) * 8)
            except Exception:
                continue
            if len(raw) < ((hi - lo) // PAGE) * 8:
                raw = raw[:len(raw) // 8 * 8]
            e = np.frombuffer(raw, dtype=np.uint64)
            b = off[(lo, hi)]
            pm = (e & np.uint64(PM_PRESENT)) != 0
            sd = (e & np.uint64(PM_SOFT_DIRTY)) != 0
            ip = np.nonzero(pm)[0]
            pres.append(b + ip)
            dirty.append(b + np.nonzero(pm & sd)[0])
            if HAVE_IDLE and len(ip):
                pfn_of[(lo, hi)] = (b + ip,
                                    (e[ip] & np.uint64(PFN_MASK)).astype(np.int64))
    cat = lambda xs: np.concatenate(xs) if xs else np.zeros(0, dtype=np.int64)
    return cat(pres), cat(dirty), pfn_of

# The first pass only establishes a layout and a residency baseline.
relayout(regions_now(), -1)
p0, _, _ = scan(layout)
seen[p0] = True          # the baseline population, not a run of fresh faults
N0 = len(p0)
if not N0: sys.exit("no resident pages")
try:
    rss_pages = int(open(f"/proc/{pid}/statm").read().split()[1])
    if rss_pages and N0 < 0.5 * rss_pages:
        print(f"!! snapshot has {N0:,} pages but RSS is {rss_pages:,} -- measuring the "
              f"wrong process, or it has not faulted in yet. Refusing.", file=sys.stderr)
        if proc:
            try: os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
            except Exception: pass
        sys.exit(2)
except FileNotFoundError:
    pass

NW = a.run // a.interval
if NW < 2: sys.exit(f"--run {a.run} gives only {NW} window(s) at S={a.interval}; need >= 2")
sum_wc = {}; sum_cold = {}; nobs = {}; resident = []

print(f"# {label}: {N0:,} pages = {N0*PAGE/2**20:.0f} MiB resident at t=0, "
      f"S={a.interval}s, {NW} windows, "
      f"idle tracking {'on' if HAVE_IDLE else 'OFF (needs root)'}", file=sys.stderr)

for w in range(NW):
    try:
        regs = regions_now()
        if regs != layout: relayout(regs, w)
        with open(f"/proc/{pid}/clear_refs", "w") as f: f.write("4")
        if HAVE_IDLE:
            _, _, pf = scan(layout)
            for slots, pfns_ in pf.values():
                idle_write([int(x) for x in pfns_ if x])
    except (FileNotFoundError, ProcessLookupError): break
    time.sleep(a.interval)
    if proc and proc.poll() is not None:
        print(f"# workload exited during window {w}", file=sys.stderr); break
    try:
        regs = regions_now()
        if regs != layout: relayout(regs, w)
        present, dirty, pf = scan(regs)
    except (FileNotFoundError, ProcessLookupError): break
    if not len(present): break

    # A page seen for the first time has no history; call it written now.
    fresh = present[~seen[present]]
    seen[present] = True
    last_w[fresh] = w
    last_a[fresh] = w
    last_w[dirty] = w
    if HAVE_IDLE:
        for slots, pfns_ in pf.values():
            still = idle_read([int(x) for x in pfns_ if x])
            touched = slots[np.array([int(x) not in still for x in pfns_], dtype=bool)]
            last_a[touched] = w

    # Denominator is what is resident NOW, not what was resident at t=0.
    Nw = len(present)
    resident.append(Nw)
    lw = last_w[present]; la = last_a[present]
    for k in range(1, w + 2):
        T = k * a.interval
        wc = int(np.count_nonzero(lw <= w - k))
        cd = int(np.count_nonzero(la <= w - k)) if HAVE_IDLE else 0
        sum_wc[T] = sum_wc.get(T, 0.0) + 100.0 * wc / Nw
        sum_cold[T] = sum_cold.get(T, 0.0) + 100.0 * cd / Nw
        nobs[T] = nobs.get(T, 0) + 1
    print(f"# window {w+1}/{NW} done, {Nw:,} resident", file=sys.stderr)

N = int(sum(resident) / len(resident)) if resident else N0

out = sys.stdout if a.out == "-" else open(a.out, "w")
# csv.writer, not an f-string: a label like "redis (ycsb-c, 95/5)" carries a
# comma, and hand-formatted rows shift every column after it by one.
import csv as _csv
w_ = _csv.writer(out, lineterminator="\n")
w_.writerow(["workload", "T_sec", "pages", "write_cold_pct", "cold_pct",
             "windows_averaged"])
for T in sorted(sum_wc):
    n = nobs[T]
    wc = sum_wc[T] / n
    # -1, not nan: it round-trips through CSV and the plotter tests for it.
    cd = (sum_cold[T] / n) if HAVE_IDLE else -1.0
    w_.writerow([label, T, N, f"{wc:.2f}", f"{cd:.2f}", n])
if proc:
    try: os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except Exception: pass
