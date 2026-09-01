#!/usr/bin/env python3
"""Cold and write-cold at every T on a ladder, from one pass.

    ./decay_census.py --cmd './pr -f g.sg' --passes 9
    ./decay_census.py --pid 1234 --ladder 5,10,20,30,45,60,90,120,180,300,600

WHY A LADDER AND NOT ONE RUN PER T
    Both bits are sticky: once set they stay set until something clears them.
    So clear once at t0 and read repeatedly WITHOUT clearing again, and the
    count at time t is "pages touched anywhere in [t0, t]":

        cold(T)       = 1 - Referenced(t0+T) / Rss          <- smaps
        write-cold(T) = 1 - soft_dirty(t0+T) / resident     <- pagemap

    A count is all either statistic needs, so neither needs root and one pass
    answers the whole ladder. Clearing every window is what would confine
    smaps -- which reports Referenced: per mapping, a count and not a set --
    to answering T = S and nothing else.

WHY PASSES
    A pass measures decay from one t0. If the workload has phases, a window
    that contains a phase answers differently from one that does not, and the
    steady-state answer is the average over t0. If it has no phases, one pass
    is the whole measurement. The sd across passes is how we tell which case
    we are in, so it is reported beside every mean rather than assumed.

Results are written after every pass, so an interrupted run still leaves
everything it completed.
"""
import argparse, atexit, csv, os, shlex, signal, subprocess, sys, time
import numpy as np

PAGE = os.sysconf("SC_PAGE_SIZE")
ap = argparse.ArgumentParser()
ap.add_argument("--pid", type=int); ap.add_argument("--cmd")
ap.add_argument("--label"); ap.add_argument("--out", default="-")
ap.add_argument("--ladder", default="5,10,20,30,45,60,90,120,180,240,300,360,480,600")
ap.add_argument("--passes", type=int, default=3)
ap.add_argument("--warmup", type=int, default=45)
a = ap.parse_args()

proc = None
if a.cmd:
    proc = subprocess.Popen(shlex.split(a.cmd), preexec_fn=os.setsid,
                            stdout=open(os.devnull, "w"), stderr=subprocess.STDOUT)
    pid = proc.pid
    def _reap(*_):
        try: os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except Exception: pass
    atexit.register(_reap)
    for _s in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(_s, lambda *_: sys.exit(143))
    time.sleep(a.warmup)
elif a.pid:
    pid = a.pid
else:
    sys.exit("need --pid or --cmd")

label = a.label or (a.cmd.split()[0].split("/")[-1] if a.cmd else f"pid{pid}")
LAD = [int(x) for x in a.ladder.split(",")]
HAVE_ROOT = os.geteuid() == 0
acc_wc = {T: [] for T in LAD}
acc_cd = {T: [] for T in LAD}
acc_gone = {T: [] for T in LAD}   # of the t0 population, how much has vanished
npages = []


def regions():
    out = []
    for line in open(f"/proc/{pid}/maps"):
        p = line.split()
        n = p[5] if len(p) > 5 else ""
        if n in ("[vsyscall]", "[vvar]", "[vdso]"):
            continue
        lo, hi = (int(x, 16) for x in p[0].split("-"))
        out.append((lo, hi))
    return out


def smaps_ref():
    """(Rss kB, Referenced kB). Referenced is cumulative since the last
    clear_refs, which is the whole point."""
    r = f = 0
    with open(f"/proc/{pid}/smaps") as fh:
        for line in fh:
            if line.startswith("Rss:"):
                r += int(line.split()[1])
            elif line.startswith("Referenced:"):
                f += int(line.split()[1])
    return r, f


def scan_pass(regs):
    """present and soft-dirty masks over a FIXED region list, concatenated."""
    pres, dirty = [], []
    with open(f"/proc/{pid}/pagemap", "rb") as fh:
        for lo, hi in regs:
            npg = (hi - lo) // PAGE
            try:
                fh.seek((lo // PAGE) * 8)
                raw = fh.read(npg * 8)
            except Exception:
                raw = b""
            e = np.frombuffer(raw[:len(raw) // 8 * 8], dtype=np.uint64)
            if len(e) < npg:                       # region shrank or vanished
                e = np.concatenate([e, np.zeros(npg - len(e), dtype=np.uint64)])
            pres.append((e & np.uint64(1 << 63)) != 0)
            dirty.append((e & np.uint64(1 << 55)) != 0)
    if not pres:
        return np.zeros(0, dtype=bool), np.zeros(0, dtype=bool)
    return np.concatenate(pres), np.concatenate(dirty)


def emit():
    """Rewrite the CSV from everything gathered so far."""
    out = sys.stdout if a.out == "-" else open(a.out, "w")
    w = csv.writer(out, lineterminator="\n")
    w.writerow(["workload", "T_sec", "pages", "write_cold_pct", "cold_pct",
                "windows_averaged", "cold_method", "write_cold_sd", "cold_sd",
                "freed_pct"])
    N = int(np.mean(npages)) if npages else 0
    for T in LAD:
        if not acc_wc[T]:
            continue
        w.writerow([label, T, N,
                    f"{np.mean(acc_wc[T]):.2f}", f"{np.mean(acc_cd[T]):.2f}",
                    len(acc_wc[T]),
                    "page_idle" if HAVE_ROOT else "smaps_referenced",
                    f"{np.std(acc_wc[T]):.2f}", f"{np.std(acc_cd[T]):.2f}",
                    f"{np.mean(acc_gone[T]):.2f}" if acc_gone[T] else ""])
    if out is not sys.stdout:
        out.close()


print(f"# {label}: pid {pid}, ladder {LAD[0]}..{LAD[-1]}s, "
      f"{a.passes} passes of {LAD[-1]}s", file=sys.stderr)

for p in range(a.passes):
    try:
        open(f"/proc/{pid}/clear_refs", "w").write("3")   # young
        open(f"/proc/{pid}/clear_refs", "w").write("4")   # soft-dirty
    except (FileNotFoundError, ProcessLookupError):
        break
    # Fix the population at t0 and mark pages written ONCE, permanently.
    #
    # The obvious formulation -- 1 - dirty_now / resident_now -- is not
    # monotone in T, and write-cold must be: a page not written in 8 minutes
    # was not written in 6. pagerank frees its 32 MiB score array between
    # trials, so a ladder point landing mid-free drops ~8,203 pages from BOTH
    # the numerator and the denominator and the ratio JUMPS UP. Measured:
    # 94.21% at T=6 s against 96.32% at T=12 s.
    #
    # So: the denominator is the pages resident at t0, and a page counts as
    # written once it is seen dirty OR once it disappears. A freed page is
    # not available to place on flash, and treating it as written is both
    # conservative and monotone by construction.
    regs0 = regions()
    pres0, _ = scan_pass(regs0)
    N0 = int(pres0.sum())
    if not N0:
        break
    written = np.zeros(len(pres0), dtype=bool)
    rss0, _ = smaps_ref()
    ref_hi = 0
    t0 = time.time()
    dead = False
    for T in LAD:
        d = t0 + T - time.time()
        if d > 0:
            time.sleep(d)
        if proc and proc.poll() is not None:
            print(f"# workload exited in pass {p}", file=sys.stderr)
            dead = True
            break
        try:
            _, ref = smaps_ref()
            pres, dty = scan_pass(regs0)
        except (FileNotFoundError, ProcessLookupError):
            dead = True
            break
        if not rss0:
            dead = True
            break
        # sticky: dirtied now, or gone from the t0 population
        gone = pres0 & ~pres
        written |= (pres0 & (dty | ~pres))
        acc_wc[T].append(100.0 * (1.0 - int(written.sum()) / N0))
        # smaps gives a COUNT, not a set, so the sticky trick above is not
        # available for the accessed bit -- Referenced: counts only pages
        # resident right now, and a freed page takes its referenced state with
        # it. Running maximum instead: the true accessed-since-t0 set only
        # grows, so the measured count can only ever undercount it, and the
        # max-so-far is both closer to the truth and monotone.
        ref_hi = max(ref_hi, ref)
        acc_cd[T].append(min(100.0, max(0.0, 100.0 * (1.0 - ref_hi / rss0))))
        emit()          # per ladder point: a long pass that dies keeps what it had
        acc_gone[T].append(100.0 * int(gone.sum()) / N0)
        npages.append(N0)
    emit()
    print(f"# pass {p+1}/{a.passes} done", file=sys.stderr, flush=True)
    if dead:
        break
