#!/usr/bin/env python3
"""How much memory is write-cold, against how much is cold, as a function of T.

The motivation argument in one measurement. Cold-page tiering (Google's
software-defined far memory, Meta's TMO) moves pages that were NOT ACCESSED for
T seconds. LtRAM can move pages that were NOT WRITTEN for T seconds -- a
strictly larger set, because every cold page is also write-cold but a page that
is read a million times a second and never written is write-cold and not cold.

    sudo ./rw_census.py --pid 1234 --until 28800
    sudo ./rw_census.py --cmd './my_workload args' --until 3600

Both quantities come from the SAME pages in the SAME pass, which is what makes
the comparison mean anything:

  written    /proc/PID/clear_refs = 4 clears the soft-dirty bit; any store then
             sets it, and it stays set until cleared again. So one clear at t=0
             plus reads at increasing T gives the cumulative "written by T"
             curve directly -- no repeated clearing, no sampling gaps.
  accessed   /sys/kernel/mm/page_idle/bitmap. Marking a page idle at t=0 means
             any access clears the bit, and it stays clear. Same cumulative
             property, so the two curves are measured the same way.

Needs root for page_idle (it is indexed by PFN, and PFNs are privileged).
Without root the accessed column is skipped and the write-cold curve still
works, which is the half this project's argument rests on.
"""
import argparse, ctypes, os, struct, sys, time, subprocess, signal

PAGE = os.sysconf("SC_PAGE_SIZE")
PM_SOFT_DIRTY = 1 << 55
PM_PRESENT    = 1 << 63
PM_FILE       = 1 << 61
PM_SWAPPED    = 1 << 62
PFN_MASK      = (1 << 55) - 1

ap = argparse.ArgumentParser()
ap.add_argument("--pid", type=int)
ap.add_argument("--cmd", help="launch this and measure it")
ap.add_argument("--label", default=None, help="workload name for the output")
ap.add_argument("--until", type=int, default=3600, help="last T in seconds")
ap.add_argument("--warmup", type=int, default=30, help="let it reach steady state first")
ap.add_argument("--anon-only", action="store_true", help="exclude file-backed pages")
ap.add_argument("--out", default="-")
a = ap.parse_args()

SCHEDULE = [1, 2, 4, 8, 15, 30, 60, 120, 240, 360, 480, 600,
            900, 1200, 1800, 2700, 3600, 5400, 7200, 10800, 14400, 21600, 28800]
SCHEDULE = [t for t in SCHEDULE if t <= a.until]

proc = None
if a.cmd:
    proc = subprocess.Popen(a.cmd, shell=True, preexec_fn=os.setsid)
    pid = proc.pid
    print(f"# launched pid {pid}: {a.cmd}", file=sys.stderr)
    time.sleep(a.warmup)
elif a.pid:
    pid = a.pid
else:
    sys.exit("need --pid or --cmd")

label = a.label or (a.cmd.split()[0] if a.cmd else f"pid{pid}")

def vmas():
    """Mappings worth counting. Excludes the special ones the kernel pins in
    place -- vsyscall, vvar, vdso -- which can never move regardless of policy
    and would otherwise pad the denominator."""
    out = []
    with open(f"/proc/{pid}/maps") as f:
        for line in f:
            p = line.split()
            rng, perms = p[0], p[1]
            name = p[5] if len(p) > 5 else ""
            if name in ("[vsyscall]", "[vvar]", "[vdso]"): continue
            if a.anon_only and name and not name.startswith("["): continue
            lo, hi = (int(x, 16) for x in rng.split("-"))
            out.append((lo, hi, name))
    return out

def read_pagemap(lo, hi):
    n = (hi - lo) // PAGE
    with open(f"/proc/{pid}/pagemap", "rb") as f:
        f.seek((lo // PAGE) * 8)
        buf = f.read(n * 8)
    return struct.unpack(f"<{len(buf)//8}Q", buf)

HAVE_IDLE = os.geteuid() == 0 and os.path.exists("/sys/kernel/mm/page_idle/bitmap")

def idle_set(pfns):
    """Mark these PFNs idle. Writes are per-u64, 64 PFNs each."""
    words = {}
    for p in pfns: words.setdefault(p >> 6, 0)
    for p in pfns: words[p >> 6] |= 1 << (p & 63)
    with open("/sys/kernel/mm/page_idle/bitmap", "r+b") as f:
        for w, v in words.items():
            f.seek(w * 8); f.write(struct.pack("<Q", v))

def idle_get(pfns):
    """Which of these are STILL idle, i.e. untouched since we marked them."""
    need = sorted({p >> 6 for p in pfns})
    words = {}
    with open("/sys/kernel/mm/page_idle/bitmap", "rb") as f:
        for w in need:
            f.seek(w * 8); d = f.read(8)
            if len(d) == 8: words[w] = struct.unpack("<Q", d)[0]
    return {p for p in pfns if words.get(p >> 6, 0) & (1 << (p & 63))}

# ---- t = 0 -----------------------------------------------------------------
base = {}          # vaddr -> pfn, for the pages we are tracking
regions = vmas()
for lo, hi, name in regions:
    try: ents = read_pagemap(lo, hi)
    except Exception: continue
    for i, e in enumerate(ents):
        if e & PM_PRESENT:
            base[lo + i * PAGE] = e & PFN_MASK
resident = len(base)
if resident == 0: sys.exit("no resident pages found -- wrong pid, or nothing mapped yet")

with open(f"/proc/{pid}/clear_refs", "w") as f: f.write("4")   # clear soft-dirty
if HAVE_IDLE:
    pfns = {p for p in base.values() if p}
    if pfns: idle_set(pfns)
t0 = time.time()

out = sys.stdout if a.out == "-" else open(a.out, "w")
print("workload,T_sec,resident_pages,write_cold_pct,cold_pct,rss_mb", file=out, flush=True)
print(f"# {label}: {resident:,} resident pages = {resident*PAGE/2**20:.0f} MiB, "
      f"idle tracking {'on' if HAVE_IDLE else 'OFF (needs root)'}", file=sys.stderr)

for T in SCHEDULE:
    now = time.time() - t0
    if now < T: time.sleep(T - now)
    if proc and proc.poll() is not None:
        print(f"# workload exited before T={T}", file=sys.stderr); break
    written = 0; still = set()
    try:
        for lo, hi, name in regions:
            try: ents = read_pagemap(lo, hi)
            except Exception: continue
            for i, e in enumerate(ents):
                va = lo + i * PAGE
                if va in base and (e & PM_PRESENT) and (e & PM_SOFT_DIRTY):
                    written += 1
        if HAVE_IDLE:
            still = idle_get({p for p in base.values() if p})
    except (FileNotFoundError, ProcessLookupError):
        print(f"# workload gone at T={T}", file=sys.stderr); break
    wc = 100.0 * (resident - written) / resident
    cold = 100.0 * len(still) / resident if HAVE_IDLE else float("nan")
    rss = 0
    try:
        with open(f"/proc/{pid}/statm") as f: rss = int(f.read().split()[1]) * PAGE / 2**20
    except Exception: pass
    print(f"{label},{T},{resident},{wc:.2f},{cold:.2f},{rss:.0f}", file=out, flush=True)

if proc:
    try: os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except Exception: pass
