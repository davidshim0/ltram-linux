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
    ts = sorted(s[2] for s in slow)
    gaps = [(ts[i+1]-ts[i])*1e6 for i in range(len(ts)-1)]   # microseconds
    gaps = [g for g in gaps if g > 0]
    if gaps:
        gaps_s = sorted(gaps)
        med = gaps_s[len(gaps_s)//2]
        # how tightly are gaps packed around the median?
        near = sum(1 for g in gaps if 0.8*med <= g <= 1.2*med)
        print(f"\n  inter-arrival: median {med:8.1f} us  -> {1e6/med:7.0f}/s")
        print(f"                 {100*near/len(gaps):5.1f}% of gaps within +/-20% of the median")
        print(f"                 p10 {gaps_s[len(gaps_s)//10]:8.1f}   p90 {gaps_s[9*len(gaps_s)//10]:8.1f} us")
        if near/len(gaps) > 0.6:
            print(f"  => PERIODIC. Something fires every ~{med:.0f} us regardless of the loop.")
        else:
            print("  => not periodic; gaps are broadly spread.")

    # 2. positional in the loop?
    if L:
        NB = 10
        b = collections.Counter(min(NB-1, s[1]*NB//L) for s in slow)
        exp = len(slow)/NB
        print(f"\n  loop position, {NB} equal bins (uniform would be {exp:,.0f} each):")
        print("   " + " ".join(f"{b.get(i,0):>7,}" for i in range(NB)))
        chi = sum((b.get(i,0)-exp)**2/exp for i in range(NB))
        print(f"  chi-square vs uniform = {chi:.1f} (9 dof; >21.7 is p<0.01)")
        print("  => POSITIONAL, tied to where in the loop." if chi > 21.7
              else "  => positionally uniform: not the code or data at any one spot.")

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
        print(f"\n=== verdict ===")
        print(f"  real {a:.0f}/s vs null {b:.0f}/s  ratio {a/b:.2f}")
        print("  => the tail is NOT the memory access: it survives with the load removed."
              if 0.7 < a/b < 1.4 else
              "  => the load matters: removing it changes the stall rate materially.")
