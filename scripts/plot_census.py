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
    # Targets stay in LOG space, because that is the space the comparison
    # below works in. Exponentiating them here and then comparing against
    # log(ts[j]) mixes the two: "closest to 5 s" resolves to "log closest to
    # 5", which is 148 s, and a 14-point ladder thins to two points.
    lo, hi = math.log(ts[0]), math.log(ts[-1])
    want = [lo + (hi - lo) * i / (keep - 1) for i in range(keep)]
    return sorted({min(range(len(ts)), key=lambda j: abs(math.log(ts[j]) - w))
                   for w in want})

# ---------------------------------------------------------------- load
work, raw = {}, {}
for p in a.csv:
    with open(p) as f:
        for r in csv.DictReader(f):
            if not r.get("T_sec"): continue
            raw.setdefault((r["workload"], float(r["T_sec"])), []).append(
                (float(r["write_cold_pct"]), float(r["cold_pct"]),
                 int(r["pages"]), r.get("cold_method", "page_idle"),
                 int(r.get("windows_averaged", 1) or 1),
                 float(r.get("write_cold_sd", 0) or 0)))
# Several rounds of the same workload are independent samples of the same
# statistic. Pool them by pass count rather than treating a repeated (workload,
# T) as two points, which would draw two bars for one measurement.
for (name, T), rows in raw.items():
    n = sum(r[4] for r in rows)
    wc = sum(r[0] * r[4] for r in rows) / n
    cds = [r for r in rows if r[1] >= 0]
    cd = (sum(r[1] * r[4] for r in cds) / sum(r[4] for r in cds)) if cds else -1.0
    var = sum(r[4] * (r[5] ** 2 + r[0] ** 2) for r in rows) / n - wc ** 2
    work.setdefault(name, []).append(
        (T, wc, cd, max(r[2] for r in rows), rows[0][3], n, max(var, 0) ** 0.5))
if not work:
    sys.exit("no rows")
for k in work: work[k].sort()

# Write-cold is monotonically non-increasing in T by definition: a page not
# written in 8 minutes was not written in 6 either. A rise means the
# estimator is comparing unlike samples -- the windowed census averages
# T = k*S over (windows - k + 1) boundaries, so every T is a mean over a
# different set, and the mean of monotone curves over different index sets
# need not be monotone. Small, but it should never pass silently.
for _n, _pts in work.items():
    _p = sorted(_pts)
    for _a, _b in zip(_p, _p[1:]):
        _sd = max(_a[6] if len(_a) > 6 else 0, _b[6] if len(_b) > 6 else 0)
        if _b[1] > _a[1] + 1e-9:
            _tag = "within the measured spread" if _b[1] - _a[1] <= _sd \
                   else "LARGER than the spread -- investigate"
            print(f"!! {_n}: write-cold RISES {_a[1]:.2f}% -> {_b[1]:.2f}% "
                  f"from T={_a[0]:g}s to T={_b[0]:g}s (sd {_sd:.2f}, {_tag}). "
                  f"Monotone by definition.", file=sys.stderr)

names = sorted(work)

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

def grouped(keep, fname, unit="s", coldf=-0.45):
    """keep: the T values to draw. unit: 's' or 'min' for the tick labels."""
    keep = [T for T in keep if T in set(allT)]
    if not keep:
        return
    # Descending write-cold at the LEFTMOST group of this figure, so the tallest
    # bar is always on the left of the first group.
    lead = keep[0]
    ordered = sorted(names, key=lambda n: -next((p[1] for p in work[n]
                                                 if p[0] == lead), -1))
    nb = len(ordered); bw = 0.70 / nb
    fig, ax = plt.subplots(figsize=(min(15.0, max(9.5, 1.9 * len(keep) + 3)), 5.2))
    for k, name in enumerate(ordered):
        col = TAB_CB[k % len(TAB_CB)]
        d = {p[0]: p[1] for p in work[name]}
        xs = [i + (k - (nb - 1) / 2) * bw for i, T in enumerate(keep) if T in d]
        ys = [d[T] for T in keep if T in d]
        ax.bar(xs, ys, width=bw, color=col,
               label=name.split(" (")[0], zorder=3)
        dc = {p[0]: p[2] for p in work[name] if p[2] >= 0}
        xc = [i + (k - (nb - 1) / 2) * bw for i, T in enumerate(keep) if T in dc]
        yc = [dc[T] for T in keep if T in dc]
        if xc:
            ax.bar(xc, yc, width=bw, color=shade(col, coldf), zorder=4)

        # One label per bar when two would collide. Below about five points of
        # separation the write-cold and cold labels sit on top of each other,
        # which is where memcached ends up past T=300 s -- 1.4% and 0.01%, two
        # unreadable numbers stacked in the same place. Then: if the cold
        # segment is invisible, the bar IS its write-cold value, so keep that
        # one; otherwise the two are effectively the same number and the cold
        # reading is the more informative of them.
        GAP = 5.0
        for i, T in enumerate(keep):
            if T not in d: continue
            x = i + (k - (nb - 1) / 2) * bw
            wc = d[T]; cd = dc.get(T)
            if cd is None or wc - cd >= GAP:
                ax.text(x, wc + 1.2, num(wc), ha="center", va="bottom",
                        fontsize=8, color="#3d474e")
                if cd is not None:
                    ax.text(x, cd + 1.0, num(cd), ha="center", va="bottom",
                            fontsize=7.5, color="white", zorder=5)
            elif cd < 0.5:
                ax.text(x, wc + 1.2, num(wc), ha="center", va="bottom",
                        fontsize=8, color="#3d474e")
            else:
                ax.text(x, cd + 1.0, num(cd), ha="center", va="bottom",
                        fontsize=7.5, color="white", zorder=5)

    hasc = {T: any(p[0] == T and p[2] >= 0 for n in ordered for p in work[n])
            for T in keep}
    # One unit for the whole axis. A ladder that starts at 5 s renders as
    # 0.0833333 minutes, which is not a time anyone reads.
    secs = (unit == "s")
    ax.set_xticks(range(len(keep)))
    ax.set_xticklabels([(f"{T:g}" if secs else f"{T/60:g}")
                        + ("" if hasc[T] else "\u2020") for T in keep])
    ax.set_xlabel("Time (s)" if secs else "Time (min)")
    ax.set_ylim(0, 108); ax.set_yticks([0, 25, 50, 75, 100])
    ax.set_yticklabels(["0", "25%", "50%", "75%", "100%"])
    ax.set_ylabel("Percentage of Memory")
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

# fig10: the whole ladder, log-spaced down to 8 groups, ticks in seconds.
# A 14-point ladder x 5 workloads is 70 bars and renders 30 inches wide.
grouped([allT[i] for i in thin(allT, keep=8)], "fig10-write-cold-by-T.png", "s")

# fig10b: the same data on a minute scale. These T are measured, not
# interpolated -- 60, 120, 180, 300 and 600 s are all points on the ladder.
grouped([60, 120, 180, 300, 600], "fig10b-write-cold-by-T.png", "min")

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
