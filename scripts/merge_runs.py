#!/usr/bin/env python3
"""Merge repeat_large.sh output into sweep.csv.

The two files have different shapes and different provenance. sweep.csv is the
original 16-size sweep and is the base; big.csv holds points re-measured with
residency-gated timing, and those win wherever they overlap.

Every (n, mode) in the overlay replaces the base row, or is inserted if the
size is new. The LAST occurrence wins, so re-running a point supersedes the
earlier attempt rather than colliding with it -- 8192 nor_cold appears twice,
once at 87% residency with the erase engine pinned off and once at full
residency with --warm-engine, and only the second is a NOR latency.

    ./merge_runs.py sweep.csv big.csv -o sweep.csv
"""
import csv, argparse, sys

ap = argparse.ArgumentParser()
ap.add_argument("base"); ap.add_argument("overlay")
ap.add_argument("-o", "--out", required=True)
a = ap.parse_args()

rows = list(csv.reader(open(a.base)))
hdr, body = rows[0], rows[1:]
COL = {name: i for i, name in enumerate(hdr)}

over = {}
for r in csv.DictReader(open(a.overlay)):
    try:
        if not float(r["mean_s"]) > 0:
            continue
    except (ValueError, KeyError):
        continue
    over[(r["n"], r["mode"])] = r          # last wins

out, seen, replaced = [], set(), 0
for r in body:
    key = (r[COL["n"]], r[COL["mode"]])
    seen.add(key)
    if key in over:
        o = over[key]
        r = list(r)
        r[COL["mean_s"]] = o["mean_s"]
        r[COL["sd_s"]] = o["sd_s"]
        if "resident_pages" in COL:
            r[COL["resident_pages"]] = o.get("resident", r[COL["resident_pages"]])
        replaced += 1
    out.append(r)

added = 0
for key, o in over.items():
    if key in seen:
        continue
    r = [""] * len(hdr)
    r[COL["n"]] = o["n"]; r[COL["mode"]] = o["mode"]
    r[COL["bytes"]] = o["bytes"]; r[COL["pages"]] = o["pages"]
    r[COL["mean_s"]] = o["mean_s"]; r[COL["sd_s"]] = o["sd_s"]
    if "samples" in COL:
        r[COL["samples"]] = "36"
    if "resident_pages" in COL:
        r[COL["resident_pages"]] = o.get("resident", "0")
    out.append(r); added += 1

out.sort(key=lambda r: (int(r[COL["n"]]), r[COL["mode"]]))
w = csv.writer(open(a.out, "w", newline=""))
w.writerow(hdr); w.writerows(out)
print(f"replaced {replaced}, added {added} -> {a.out}")

# Say plainly what is still missing, per size, rather than letting a hole
# show up as a gap in a figure.
have = {}
for r in out:
    have.setdefault(int(r[COL["n"]]), set()).add(r[COL["mode"]])
NEED = {"comp", "dram_cold", "dram_warm", "nor_cold", "nor_warm"}
gaps = {n: NEED - m for n, m in sorted(have.items()) if NEED - m}
if gaps:
    print("\nINCOMPLETE -- these sizes cannot be plotted in every figure:")
    for n, miss in gaps.items():
        print(f"  N={n:<6} missing {' '.join(sorted(miss))}")
else:
    print("\nevery size has all five modes")
