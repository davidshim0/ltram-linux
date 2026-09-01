#!/usr/bin/env python3
"""Which mappings are the cold ones.

    ./where_cold.py --pid 1234 --interval 120
    ./where_cold.py --cmd './foo --bar' --interval 120 --warmup 60

The census reports one number per workload. When that number is surprising --
llama.cpp came out 41% cold over a 2 minute window, on a model that is read
end to end for every token -- the next question is WHICH pages, and a single
percentage cannot answer it.

smaps gives Rss and Referenced per VMA, so cold can be attributed to a
mapping by name and size without root. Soft-dirty per VMA comes from pagemap
the same way, so the same pass also says which mappings are written.
"""
import argparse, os, shlex, signal, subprocess, sys, time, atexit
import numpy as np

PAGE = os.sysconf("SC_PAGE_SIZE")
ap = argparse.ArgumentParser()
ap.add_argument("--pid", type=int); ap.add_argument("--cmd")
ap.add_argument("--interval", type=int, default=120)
ap.add_argument("--warmup", type=int, default=60)
ap.add_argument("--top", type=int, default=14)
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
    print(f"# pid {pid}, warming up {a.warmup}s", file=sys.stderr)
    time.sleep(a.warmup)
elif a.pid: pid = a.pid
else: sys.exit("need --pid or --cmd")

def vmas():
    """(lo, hi, name) per mapping, in /proc/pid/maps order."""
    out = []
    for line in open(f"/proc/{pid}/maps"):
        p = line.split()
        lo, hi = (int(x, 16) for x in p[0].split("-"))
        out.append((lo, hi, p[5] if len(p) > 5 else "anon", p[1]))
    return out

def smaps():
    """Rss and Referenced kB per mapping, keyed by start address."""
    d = {}; cur = None
    for line in open(f"/proc/{pid}/smaps"):
        if "-" in line.split()[0] and ":" not in line.split()[0]:
            cur = int(line.split()[0].split("-")[0], 16); d[cur] = [0, 0]
        elif cur is not None:
            if line.startswith("Rss:"):          d[cur][0] = int(line.split()[1])
            elif line.startswith("Referenced:"): d[cur][1] = int(line.split()[1])
    return d

open(f"/proc/{pid}/clear_refs", "w").write("3")   # clear young
open(f"/proc/{pid}/clear_refs", "w").write("4")   # clear soft-dirty
time.sleep(a.interval)

sm = smaps()
regs = vmas()
rows = []
with open(f"/proc/{pid}/pagemap", "rb") as f:
    for lo, hi, name, perm in regs:
        rss, ref = sm.get(lo, (0, 0))
        if rss == 0: continue
        try:
            f.seek((lo // PAGE) * 8)
            raw = f.read(((hi - lo) // PAGE) * 8)
        except Exception:
            raw = b""
        dirty = 0
        if len(raw) >= 8:
            e = np.frombuffer(raw[:len(raw) // 8 * 8], dtype=np.uint64)
            pm = (e & np.uint64(1 << 63)) != 0
            sd = (e & np.uint64(1 << 55)) != 0
            dirty = int((pm & sd).sum()) * PAGE // 1024
        rows.append((rss, ref, dirty, name, perm, hi - lo))

rows.sort(reverse=True)
tot_r = sum(r[0] for r in rows); tot_f = sum(r[1] for r in rows)
tot_d = sum(r[2] for r in rows)
print(f"\nover {a.interval}s, pid {pid}:  resident {tot_r/1024:,.0f} MiB   "
      f"cold {100*(1-tot_f/tot_r):.1f}%   written {100*tot_d/tot_r:.1f}%\n")
print(f"  {'resident':>10} {'cold':>7} {'written':>8}  {'perm':<5} mapping")
for rss, ref, dirty, name, perm, sz in rows[:a.top]:
    short = name if len(name) < 46 else "..." + name[-43:]
    print(f"  {rss/1024:>7,.0f} MiB {100*(1-ref/rss):>6.1f}% "
          f"{100*dirty/rss:>7.1f}%  {perm:<5} {short}")
rest = rows[a.top:]
if rest:
    rr = sum(r[0] for r in rest); ff = sum(r[1] for r in rest)
    print(f"  {rr/1024:>7,.0f} MiB {100*(1-ff/max(rr,1)):>6.1f}% "
          f"{'':>8}  {'':<5} ... {len(rest)} smaller mappings")
