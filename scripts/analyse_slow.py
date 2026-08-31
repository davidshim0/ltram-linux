#!/usr/bin/env python3
"""Localise a stall without assuming what causes it.

    ./analyse_slow.py real.log [null.log]

Two questions, both answerable from SLOW records alone:

  PERIODIC IN TIME?  If inter-arrival times cluster hard at one period, the
                     loop is being interrupted by something external at a
                     fixed rate, and it does not matter what the loop is doing.
  POSITIONAL IN LOOP? If the stalls cluster at particular loop indices, it is
                     the code or the data at that spot, not an external event.

A second log taken with --null-load (same loop, same clock reads, no memory
access) settles whether the tail involves memory at all.
"""
import sys, collections, math

def read(path):
    slow, sect = [], []
    for l in open(path):
        f = l.split()
        if f[:1] == ["SLOW"] and len(f) >= 5:
            slow.append((int(f[1]), int(f[2]), float(f[3]), int(f[4])))
        elif f[:1] == ["SECT"] and len(f) >= 5:
            sect.append(tuple(float(x) for x in f[2:5]))
    return slow, sect

def chase_ns(path):
    """Median nanoseconds per access, from the SECT chase column."""
    L = nlines(path)
    v = sorted(float(l.split()[4]) for l in open(path) if l.startswith("SECT"))
    return (v[len(v)//2] / L) if (v and L) else None


def nlines(path):
    for l in open(path):
        if l.startswith("CHASE"):
            return int(l.split()[1])
    return None

def report(path, tag):
    slow, sect = read(path)
    L = nlines(path)
    print(f"\n=== {tag}: {path} ===")
    if not slow:
        print("  no SLOW records (nothing crossed the threshold)"); return None
    span = max(s[2] for s in slow) - min(s[2] for s in slow)
    print(f"  {len(slow):,} slow events over {span:.1f} s = {len(slow)/span:.0f}/s"
          f"   loop length {L:,} lines" if L else "")

    # 1. periodic in time?
    #
    # NOT a tightness test. An event is only recorded when it lands inside a
    # timed bracket, so most firings are missed and the gaps come out as
    # integer MULTIPLES of the true period. Clustering around the median then
    # fails badly -- it called a perfectly quantised 1 kHz signal "not
    # periodic" because only 59% of gaps were 1x. Test quantisation instead:
    # take the smallest common gap as the candidate period and ask what
    # fraction of all gaps are integer multiples of it.
    ts = sorted(s[2] for s in slow)
    gaps = sorted(g for g in ((ts[i+1]-ts[i])*1e6 for i in range(len(ts)-1)) if g > 0)
    if gaps:
        base = gaps[len(gaps)//20]            # 5th percentile ~ one period
        mult = collections.Counter(round(g/base) for g in gaps)
        on = sum(1 for g in gaps
                 if round(g/base) >= 1 and abs(g/base - round(g/base)) < 0.02)
        print(f"\n  inter-arrival: base period {base:8.1f} us  -> {1e6/base:7.0f}/s")
        print(f"                 {100*on/len(gaps):5.1f}% of gaps are integer multiples of it")
        print("                 " + "  ".join(f"{k}x:{100*mult[k]/len(gaps):.0f}%"
                                              for k in sorted(mult)[:6]))
        if on/len(gaps) > 0.9:
            print(f"  => PERIODIC at {1e6/base:.0f} Hz. Multiples mean firings were missed,")
            print(f"     not that the source is irregular; the miss rate gives the duty cycle.")
        else:
            print("  => not quantised to any single period.")

    # 2. positional in the loop?
    if L:
        NB = 10
        b = collections.Counter(min(NB-1, s[1]*NB//L) for s in slow)
        exp = len(slow)/NB
        print(f"\n  loop position, {NB} equal bins (uniform would be {exp:,.0f} each):")
        print("   " + " ".join(f"{b.get(i,0):>7,}" for i in range(NB)))
        chi = sum((b.get(i,0)-exp)**2/exp for i in range(NB))
        print(f"  chi-square vs uniform = {chi:.1f} (9 dof; >21.7 is p<0.01)")
        if chi > 21.7:
            # Significant is not the same as meaningful. At tens of thousands
            # of samples a 10% skew clears p<0.01 easily, so print how big the
            # skew actually is next to the verdict, and say where to look.
            worst = max(range(NB), key=lambda i: abs(b.get(i, 0) - exp))
            print(f"  => POSITIONAL (bin {worst} is {b.get(worst,0)/exp:.2f}x uniform,"
                  f" {100*abs(b.get(worst,0)-exp)/len(slow):.1f}% of all events)")
            print("     Check whether it sits in the very first iterations (cold cache)")
            print("     or is spread flat across the bin (something else).")
        else:
            print("  => positionally uniform: not the code or data at any one spot.")

    # 3. size classes
    print("\n  size classes:")
    cls = collections.Counter()
    for _,_,_,d in slow:
        cls[1 << (d.bit_length()-1)] += 1
    for k in sorted(cls):
        print(f"    {k/1000:9.1f} - {2*k/1000:<9.1f} us  {cls[k]:>8,}")

    if sect:
        print("\n  where the pass time goes (median / p99, ms):")
        for i, nm in enumerate(("gap", "scrub", "chase")):
            v = sorted(x[i]/1e6 for x in sect)
            print(f"    {nm:<6} {v[len(v)//2]:9.3f} {v[int(0.99*len(v))]:9.3f}   max {v[-1]:9.3f}")
    return len(slow)/span

a = report(sys.argv[1], "REAL LOAD")
if len(sys.argv) > 2:
    b = report(sys.argv[2], "NULL LOAD (no memory access)")
    if a and b:
        # Comparing raw rates is wrong: the two runs observe through DIFFERENT
        # window sizes, because removing the load shortens the timed bracket.
        # Normalise. If the same external event is being sampled by a shorter
        # window, then solving for the clock-read overhead X from each run
        # separately must give the same answer.
        ra, rb = chase_ns(sys.argv[1]), chase_ns(sys.argv[2])
        print(f"\n=== verdict ===")
        print(f"  raw rate: real {a:.0f}/s vs null {b:.0f}/s  (ratio {a/b:.2f})")
        if ra and rb:
            load = ra - rb
            per = 1e6 / (a / (a / 1000.0) if False else 1000.0)   # assumed 1 kHz source
            Xr = (a / 1000.0) * ra - load
            Xn = (b / 1000.0) * rb
            print(f"  per access: real {ra:.1f} ns, null {rb:.1f} ns -> the load costs {load:.1f} ns")
            print(f"  implied clock-read overhead X: {Xr:.1f} ns (real) vs {Xn:.1f} ns (null)")
            agree = 100 * abs(Xr - Xn) / ((Xr + Xn) / 2)
            print(f"  agreement {agree:.1f}%")
            if agree < 15:
                print("  => SAME EVENT, two window sizes. The tail is NOT the memory access:")
                print("     the whole rate difference is explained by the shorter bracket.")
            else:
                print("  => the load changes the tail beyond what window size explains.")
