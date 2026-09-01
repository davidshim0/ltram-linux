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
                 float(r["cold_pct"]), int(r["pages"]),
                 r.get("cold_method", "page_idle")))
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
    x = range(len(ts))
    # Unprivileged runs can only measure cold at T = S, so a panel may have
    # the cold segment on some bars and not others. Draw the full write-cold
    # bar first and overdraw cold where it exists, rather than skipping the
    # comparison entirely because one bar is missing it.
    ax.bar(x, wc, width=.72, color=C_EXTRA, zorder=3,
           label="write-cold: read, but not written in T  (what LtRAM moves)")
    xc = [i for i in x if cold[i] >= 0]
    if xc:
        ax.bar(xc, [cold[i] for i in xc], width=.72, color=C_COLD, zorder=4,
               label="cold: not accessed in T  (what cold-page tiering moves)")
    have_cold = len(xc) == len(ts)

    # One callout, on the T the argument is actually made at. A number the
    # reader can quote beats a bar they have to measure against the axis.
    tcall = a.at if a.at is not None else (ts[xc[-1]] if xc else ts[-1])
    i = min(range(len(ts)), key=lambda j: abs(ts[j] - tcall))
    if have_cold:
        # Nudge off the frame when the callout sits on the last bar.
        ha = "right" if i >= len(ts) - 1 else ("left" if i == 0 else "center")
        dx = {"right": 10, "left": -10, "center": 0}[ha]
        ax.annotate(f"{cold[i]:.0f}% \u2192 {wc[i]:.0f}%",
                    (i, wc[i]), xytext=(dx, 7), textcoords="offset points",
                    ha=ha, fontsize=10, weight="semibold", color="#3d474e")

    if any(p[4] == "smaps_referenced" for p in pts):
        ax.text(0.99, 0.03, "cold via smaps Referenced (proxy)", ha="right",
                transform=ax.transAxes, fontsize=8, color="#7A838A")
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
# ---------------------------------------------------------------- grouped
# Same numbers, transposed: T on the x-axis, one bar per workload inside each
# T group. The panel-per-workload view answers "how much can we move in THIS
# workload"; this one answers "at this T, how do the workloads compare".
#
# Tableau's colour-blind-safe ten. Cold is the SAME hue as its workload,
# shifted in lightness, so the pair reads as one measurement in two states
# rather than as two unrelated series.
# Ordered so no series sits next in rank to one that could pass for its own
# cold shade. Tableau's colour-blind ten is really three hues -- blue, orange,
# grey -- so a light blue used as a SERIES reads as a lightened dark blue.
# redis, the largest of the two caches, takes the dark orange for that reason.
TAB_CB = ["#006BA4", "#FF800E", "#595959", "#C85200", "#5F9ED1",
          "#ABABAB", "#A2C8EC", "#FFBC79"]

def shade(hexc, f):
    """f > 0 lightens toward white, f < 0 darkens toward black."""
    r, g, b = (int(hexc[k:k+2], 16) for k in (1, 3, 5))
    if f >= 0: r, g, b = (int(c + (255 - c) * f) for c in (r, g, b))
    else:      r, g, b = (int(c * (1 + f)) for c in (r, g, b))
    return "#%02X%02X%02X" % (r, g, b)

def num(v):
    """No decimal unless it would read as zero."""
    return f"{v:.1f}" if v < 1 else f"{v:.0f}"

allT = sorted({p[0] for name in names for p in work[name]})
# Descending by write-cold at the smallest T, so the reader meets the
# workloads in rank order and the ordering survives the data changing.
first = allT[0]
ordered = sorted(names, key=lambda n: -next((p[1] for p in work[n]
                                             if p[0] == first), -1))

def grouped(coldf, fname, note):
    nb = len(ordered); bw = 0.70 / nb
    fig, ax = plt.subplots(figsize=(max(9.5, 1.9 * len(allT) + 3), 5.2))
    for k, name in enumerate(ordered):
        col = TAB_CB[k % len(TAB_CB)]
        d = {p[0]: p[1] for p in work[name]}
        xs = [i + (k - (nb - 1) / 2) * bw for i, T in enumerate(allT) if T in d]
        ys = [d[T] for T in allT if T in d]
        ax.bar(xs, ys, width=bw, color=col,
               label=name.split(" (")[0], zorder=3)
        for x, y in zip(xs, ys):
            ax.text(x, y + 1.2, num(y), ha="center", va="bottom",
                    fontsize=8, color="#3d474e")
        dc = {p[0]: p[2] for p in work[name] if p[2] >= 0}
        xc = [i + (k - (nb - 1) / 2) * bw for i, T in enumerate(allT) if T in dc]
        yc = [dc[T] for T in allT if T in dc]
        if xc:
            # No edge. matplotlib strokes it centred on the boundary, so an
            # outlined bar is visibly wider than the one it sits inside.
            ax.bar(xc, yc, width=bw, color=shade(col, coldf), zorder=4)
            for x, y in zip(xc, yc):
                # Sits inside the write-cold bar, so white rather than dark.
                ax.text(x, y + 1.0, num(y), ha="center", va="bottom",
                        fontsize=7.5, color="white", zorder=5)
    # An unmeasured cold value must not render as a measured zero. The
    # smaps fallback aggregates, so exactly one T carries a cold reading and
    # the rest have none -- which without a mark looks like "cold is 0 here".
    hasc = {T: any(p[0] == T and p[2] >= 0 for n in ordered for p in work[n])
            for T in allT}
    ax.set_xticks(range(len(allT)))
    ax.set_xticklabels([f"{T/60:g}" + ("" if hasc[T] else "\u2020")
                        for T in allT])
    ax.set_ylim(0, 108); ax.set_yticks([0, 25, 50, 75, 100])
    ax.set_yticklabels(["0", "25%", "50%", "75%", "100%"])
    ax.set_xlabel("Time (min)")
    ax.set_ylabel("Percentage of Memory Usage")
    ax.set_title("Share of Read-Mostly Data and Cold Data per Workload",
                 fontsize=13, weight="semibold", pad=34)
    ax.grid(axis="y", alpha=.25, lw=.5); ax.set_axisbelow(True)
    for sp in ("top", "right"): ax.spines[sp].set_visible(False)
    # One row, in bar order, short names: the legend should be read the same
    # way the bars are, and the parameters belong in the caption not the key.
    lg = ax.legend(fontsize=9.5, frameon=False, ncol=nb, loc="lower left",
                   bbox_to_anchor=(0, 1.005), columnspacing=1.6,
                   handlelength=1.5, handletextpad=0.5)
    ax.add_artist(lg)
    ax.legend(handles=[plt.Rectangle((0, 0), 1, 1, color=TAB_CB[0]),
                       plt.Rectangle((0, 0), 1, 1,
                                     color=shade(TAB_CB[0], coldf))],
              labels=["Top: Read-mostly data (Dirty = 0)",
                      "Bottom: Cold data (Accessed = 0)"],
              fontsize=8.5, frameon=False, loc="upper right", ncol=1,
              bbox_to_anchor=(1.0, 1.10), handlelength=1.3,
              handletextpad=0.5, labelspacing=0.35)
    if not all(hasc.values()):
        fig.tight_layout(rect=[0, 0.075, 1, 1])
        fig.text(0.008, 0.012,
                 "\u2020 cold not measured at this T \u2014 absent, not zero. "
                 "Reading the accessed bit per page needs /sys/kernel/mm/page_idle "
                 "(root); the unprivileged\n   fallback, clear_refs=3 with smaps "
                 "Referenced:, aggregates per mapping and so answers exactly one "
                 "T per run, here T = S = 2 min.",
                 fontsize=7.8, color="#7A838A", va="bottom", linespacing=1.5)
    else:
        fig.tight_layout()
    pth = os.path.join(a.out, fname)
    fig.savefig(pth, dpi=200)
    print("wrote", pth)

grouped(+0.58, "fig10b-write-cold-by-T.png",  "lighter shade: cold, not accessed in T")
grouped(-0.45, "fig10c-write-cold-by-T.png",  "darker shade: cold, not accessed in T")

for name in names:
    # Report at the largest T that actually HAS a cold measurement -- an
    # unprivileged run has it only at T = S, and printing -1.0% there reads
    # as a result rather than as an absence.
    pts = work[name]
    c = [p for p in pts if p[2] >= 0]
    if c:
        p_ = c[-1]
        print(f"  {name}: T={tlabel(p_[0])}  cold {p_[2]:.2f}%  "
              f"write-cold {p_[1]:.2f}%  ({p_[4]})")
    else:
        print(f"  {name}: T={tlabel(pts[-1][0])}  cold not measured  "
              f"write-cold {pts[-1][1]:.2f}%")
