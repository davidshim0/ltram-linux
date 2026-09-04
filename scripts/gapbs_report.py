#!/usr/bin/env python3
"""Turn a gapbs_ltram run into the two numbers that matter:

  1. How much slower is a trial once its pages are on flash?
  2. Does that slowdown agree with the read latency we measured directly?

(2) is the calibration check. We know from fig7 that a settled LtRAM read costs
~987 ns against ~90 ns for DRAM, so ~900 ns of extra latency per access. If we
regress trial time against the number of the process's pages that are actually
resident on LtRAM, the slope is seconds of extra trial time per resident page.
Divide by the accesses each page takes per trial and we get an implied
per-access penalty. If that lands near 900 ns, the two independent measurements
agree and the harness is sound. If it is orders out, one of them is wrong.
"""
import sys, csv, statistics as st

def rows(p):
    try:
        with open(p) as f: return list(csv.DictReader(f))
    except OSError: return []

def med(v): return st.median(v) if v else None

def fnum(r, k):
    """A row whose field is empty or malformed is dropped, not fatal."""
    try: return float(r[k])
    except (KeyError, TypeError, ValueError): return None

tri, state = rows(sys.argv[1]), rows(sys.argv[2])
iters = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
out = []


dram, ltram, bad = [], [], 0
for r in tri:
    t, e = fnum(r, "trial_s"), fnum(r, "elapsed_s")
    if t is None or e is None: bad += 1; continue
    (dram if r.get("phase") == "dram" else ltram).append((e, t))
dram = [t for _, t in dram]
if bad: out.append(("malformed_trial_rows", bad))

if dram:
    out.append(("dram_trials", len(dram)))
    out.append(("dram_median_s", round(med(dram), 4)))
    out.append(("dram_spread_s", round(max(dram) - min(dram), 4)))

if ltram:
    # "Settled" is the last quarter of the attached window: residency has
    # plateaued and the migration engine has mostly stopped moving pages.
    tail = [t for _, t in ltram[max(1, len(ltram) * 3 // 4):]]
    out.append(("ltram_trials", len(ltram)))
    out.append(("ltram_settled_median_s", round(med(tail), 4)))
    if dram:
        d, l = med(dram), med(tail)
        out.append(("slowdown_pct", round(100.0 * (l - d) / d, 2)))
        out.append(("slowdown_x", round(l / d, 3)))

# Join each trial to the residency reading nearest in time, then fit.
if state and len(ltram) >= 8:
    pts = sorted((float(r["elapsed_s"]), float(r["ltram_pages"]),
                  float(r["total_pages"])) for r in state)
    def resident(t):
        return min(pts, key=lambda p: abs(p[0] - t))
    xs, ys = [], []
    for t, s in ltram:
        xs.append(resident(t)[1]); ys.append(s)
    n = len(xs); mx, my = sum(xs)/n, sum(ys)/n
    den = sum((x - mx) ** 2 for x in xs)
    if den > 0:
        b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den
        out.append(("fit_points", n))
        out.append(("fit_slope_s_per_page", f"{b:.6e}"))
        # Each CSR page is read roughly once per PageRank iteration, and the
        # iteration count is measured (PageRank converges at 5 on kron22, so
        # -i 20 never binds). It is still the weakest link: the policy promotes
        # the hottest read-mostly pages, which take more than the average
        # number of accesses, so this number is biased HIGH.
        out.append(("implied_ns_per_access", round(b * 1e9 / max(iters, 1), 1)))
        out.append(("expected_ns_per_access", 900))
    peak = max(p[1] for p in pts); tot = max(p[2] for p in pts)
    out.append(("peak_ltram_pages", int(peak)))
    if tot: out.append(("peak_ltram_pct", round(100.0 * peak / tot, 2)))

for k, v in out: print(f"{k},{v}")
