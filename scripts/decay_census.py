#!/usr/bin/env python3
"""Cold and write-cold at every T on a ladder, from one pass.

    ./decay_census.py --cmd './pr -f g.sg' --passes 3
    ./decay_census.py --pid 1234 --ladder 5,10,20,30,45,60,90,120,180,300,450,600

WHY THIS AND NOT ONE RUN PER T
    Soft-dirty is per page, so a windowed census answers every multiple of S
    from one run. The accessed bit is not, without root: smaps reports
    Referenced: per mapping, so a windowed census answers cold at T = S and
    nothing else, and a curve costs one run per point.

    But the clearing is what confines it. Clear once at t0 and read WITHOUT
    clearing again, and Referenced: at time t is the set accessed anywhere in
    [t0, t] -- so a single pass gives cold at every t we care to look at.
    Twelve points for the price of one, and pagemap gives write-cold on the
    same ladder in the same pass.

    The cost is that one pass is decay from a fixed t0, not the steady-state
    average over boundaries. Repeating from several t0 and averaging recovers
    it: for a stationary workload the boundary average IS the expectation of
    the from-t0 statistic.
"""
import argparse, atexit, os, shlex, signal, subprocess, sys, time
import numpy as np

PAGE = os.sysconf("SC_PAGE_SIZE")
ap = argparse.ArgumentParser()
ap.add_argument("--pid", type=int); ap.add_argument("--cmd")
ap.add_argument("--label"); ap.add_argument("--out", default="-")
ap.add_argument("--ladder", default="5,10,20,30,45,60,90,120,180,300,450,600")
ap.add_argument("--passes", type=int, default=3)
ap.add_argument("--warmup", type=int, default=45)
a = ap.parse_args()

proc = None
if a.cmd:
    proc = subprocess.Popen(shlex.split(a.cmd), preexec_fn=os.setsid,
                            stdout=open(os.devnull, "w"), stderr=subprocess.STDOUT)
    pid = proc.pid
    def reap(*_):
        try: os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except Exception: pass
    atexit.register(reap)
    for s in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(s, lambda *_: sys.exit(143))
    time.sleep(a.warmup)
elif a.pid: pid = a.pid
else: sys.exit("need --pid or --cmd")
label = a.label or (a.cmd.split()[0].split("/")[-1] if a.cmd else f"pid{pid}")
LAD = [int(x) for x in a.ladder.split(",")]
HAVE_ROOT = os.geteuid() == 0

def regions():
    out = []
    for line in open(f"/proc/{pid}/maps"):
        p = line.split(); n = p[5] if len(p) > 5 else ""
        if n in ("[vsyscall]", "[vvar]", "[vdso]"): continue
        lo, hi = (int(x, 16) for x in p[0].split("-"))
        out.append((lo, hi))
    return out

def smaps_ref():
    """(Rss kB, Referenced kB) summed. Referenced is cumulative since the
    last clear_refs, which is the whole point."""
    r = f = 0
    with open(f"/proc/{pid}/smaps") as fh:
        for line in fh:
            if line.startswith("Rss:"):          r += int(line.split()[1])
            elif line.startswith("Referenced:"): f += int(line.split()[1])
    return r, f

def dirty_frac():
    tot = d = 0
    with open(f"/proc/{pid}/pagemap", "rb") as fh:
        for lo, hi in regions():
            try:
                fh.seek((lo // PAGE) * 8)
                raw = fh.read(((hi - lo) // PAGE) * 8)
            except Exception:
                continue
            if len(raw) < 8: continue
            e = np.frombuffer(raw[:len(raw) // 8 * 8], dtype=np.uint64)
            pm = (e & np.uint64(1 << 63)) != 0
            sd = (e & np.uint64(1 << 55)) != 0
            tot += int(pm.sum()); d += int((pm & sd).sum())
    return tot, d

acc_wc = {T: [] for T in LAD}
acc_cd = {T: [] for T in LAD}
npages = []
print(f"# {label}: pid {pid}, ladder {LAD[0]}..{LAD[-1]}s, "
      f"{a.passes} passes of {LAD[-1]}s", file=sys.stderr)

for p in range(a.passes):
    try:
        open(f"/proc/{pid}/clear_refs", "w").write("3")   # young
        open(f"/proc/{pid}/clear_refs", "w").write("4")   # soft-dirty
    except (FileNotFoundError, ProcessLookupError): break
    t0 = time.time()
    for T in LAD:
        d = t0 + T - time.time()
        if d > 0: time.sleep(d)
        if proc and proc.poll() is not None:
            print(f"# workload exited in pass {p}", file=sys.stderr); break
        try:
            rss, ref = smaps_ref()
            tot, dty = dirty_frac()
        except (FileNotFoundError, ProcessLookupError): break
        if not rss or not tot: break
        acc_cd[T].append(100.0 * (1.0 - ref / rss))
        acc_wc[T].append(100.0 * (1.0 - dty / tot))
        npages.append(tot)
    print(f"# pass {p+1}/{a.passes}", file=sys.stderr)

import csv as _csv
out = sys.stdout if a.out == "-" else open(a.out, "w")
w = _csv.writer(out, lineterminator="\n")
w.writerow(["workload", "T_sec", "pages", "write_cold_pct", "cold_pct",
            "windows_averaged", "cold_method"])
N = int(np.mean(npages)) if npages else 0
for T in LAD:
    if not acc_wc[T]: continue
    w.writerow([label, T, N, f"{np.mean(acc_wc[T]):.2f}",
                f"{np.mean(acc_cd[T]):.2f}", len(acc_wc[T]),
                "page_idle" if HAVE_ROOT else "smaps_referenced"])
