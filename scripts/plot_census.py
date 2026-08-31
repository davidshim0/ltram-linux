#!/usr/bin/env python3
"""The motivation figure: how much more memory is movable if you ask a
different question.

    ./plot_census.py census/*.csv -o docs/figures

Cold-page tiering -- Google's software-defined far memory, Meta's TMO -- moves
pages NOT ACCESSED for T seconds. LtRAM moves pages NOT WRITTEN for T seconds.
The second set contains the first by construction: a page read a million times
a second and never written is write-cold and is not cold.

So each bar is stacked rather than paired. The lower segment is what prior
work can move; the upper segment is what only the write-cold test reaches. The
segment boundary IS the claim, and stacking puts it where the eye lands
instead of asking the reader to subtract two bar heights.

Input is rw_census.py's CSV:
    workload,T_sec,pages,write_cold_pct,cold_pct,windows_averaged
"""
import csv, os, sys, argparse, math
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

ap = argparse.ArgumentParser()
ap.add_argument("csv", nargs="+")
ap.add_argument("-o", "--out", default=".")
ap.add_argument("--at", type=float, default=None,
                help="T to call out on each panel (default: the largest measured)")
a = ap.parse_args()

C_COLD  = "#1F5F7A"   # reachable today
C_EXTRA = "#C9852F"   # reachable only by asking about writes
C_GRID  = "#868E8A"

def tlabel(t):
    t = float(t)
    if t < 60:                       return f"{t:g}s"
    if t < 5400 or t % 3600:         return f"{t/60:g}m"   # 3840s reads as 64m
    return f"{t/3600:g}h"                                  # not 1.06667h

def thin(ts, keep=9):
    """Log-spaced subset of the measured thresholds.

    The census emits every multiple of S, which for an 8 h run at S=120 is 240
    of them. All are real; a bar chart can only carry about nine. Log spacing
    keeps both ends and the knee, where linear spacing would spend every bar
    on the flat tail.
    """
    if len(ts) <= keep: return list(range(len(ts)))
    lo, hi = math.log(ts[0]), math.log(ts[-1])
    want = [math.exp(lo + (hi - lo) * i / (keep - 1)) for i in range(keep)]
    idx = sorted({min(range(len(ts)), key=lambda j: abs(math.log(ts[j]) - w))
                  for w in want})
    return idx

# ---------------------------------------------------------------- load
work = {}
for p in a.csv:
    with open(p) as f:
        for r in csv.DictReader(f):
            if not r.get("T_sec"): continue
            work.setdefault(r["workload"], []).append(
                (float(r["T_sec"]), float(r["write_cold_pct"]),
                 float(r["cold_pct"]), int(r["pages"])))
if not work:
    sys.exit("no rows")
for k in work: work[k].sort()

names = sorted(work)
n = len(names)
ncol = 1 if n == 1 else (2 if n <= 4 else 3)
nrow = math.ceil(n / ncol)
fig, axes = plt.subplots(nrow, ncol, figsize=(6.4 * ncol, 4.0 * nrow),
                         squeeze=False)

for k, name in enumerate(names):
    ax = axes[k // ncol][k % ncol]
    pts = work[name]
    pts = [pts[j] for j in thin([p[0] for p in pts])]
    ts   = [p[0] for p in pts]
    wc   = [p[1] for p in pts]
    cold = [p[2] for p in pts]
    # cold_pct is -1 when the run had no root and could not read page_idle.
    have_cold = all(c >= 0 for c in cold)
    x = range(len(ts))

    if have_cold:
        ax.bar(x, cold, width=.72, color=C_COLD, zorder=3,
               label="cold: not accessed in T  (what cold-page tiering moves)")
        ax.bar(x, [w - c for w, c in zip(wc, cold)], width=.72, bottom=cold,
               color=C_EXTRA, zorder=3,
               label="write-cold only: read, but not written in T  (what LtRAM adds)")
    else:
        ax.bar(x, wc, width=.72, color=C_EXTRA, zorder=3,
               label="write-cold: not written in T")

    # One callout, on the T the argument is actually made at. A number the
    # reader can quote beats a bar they have to measure against the axis.
    tcall = a.at if a.at is not None else ts[-1]
    i = min(range(len(ts)), key=lambda j: abs(ts[j] - tcall))
    if have_cold:
        # Nudge off the frame when the callout sits on the last bar.
        ha = "right" if i >= len(ts) - 1 else ("left" if i == 0 else "center")
        dx = {"right": 10, "left": -10, "center": 0}[ha]
        ax.annotate(f"{cold[i]:.0f}% \u2192 {wc[i]:.0f}%",
                    (i, wc[i]), xytext=(dx, 7), textcoords="offset points",
                    ha=ha, fontsize=10, weight="semibold", color="#3d474e")

    mib = pts[0][3] * os.sysconf("SC_PAGE_SIZE") / 2**20
    ax.set_title(f"{name}   ({mib:,.0f} MiB resident)", fontsize=12,
                 weight="semibold", pad=10)
    ax.set_xticks(list(x)); ax.set_xticklabels([tlabel(t) for t in ts], fontsize=9)
    ax.set_ylim(0, 105); ax.set_yticks([0, 25, 50, 75, 100])
    ax.set_yticklabels(["0", "25%", "50%", "75%", "100%"])
    ax.set_xlabel("T, age threshold"); ax.set_ylabel("share of resident pages")
    ax.grid(axis="y", alpha=.25, lw=.5); ax.set_axisbelow(True)
    for s in ("top", "right"): ax.spines[s].set_visible(False)

for k in range(n, nrow * ncol):
    axes[k // ncol][k % ncol].axis("off")

h, l = axes[0][0].get_legend_handles_labels()
fig.legend(h[::-1], l[::-1], loc="lower center", ncol=1, frameon=False,
           fontsize=10, bbox_to_anchor=(0.5, 0.0))
fig.suptitle("Write-Cold Against Cold", fontsize=14, weight="semibold", y=0.995)
fig.tight_layout(rect=[0, 0.055 + 0.02 * (2 - min(2, nrow)), 1, 0.975])
os.makedirs(a.out, exist_ok=True)
p = os.path.join(a.out, "fig10-write-cold.png")
fig.savefig(p, dpi=200)
print("wrote", p)
for name in names:
    pts = work[name]
    print(f"  {name}: T={tlabel(pts[-1][0])}  cold {pts[-1][2]:.1f}%  "
          f"write-cold {pts[-1][1]:.1f}%")
