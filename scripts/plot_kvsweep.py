#!/usr/bin/env python3
"""The redis parameter sweep: what actually determines write-coldness.

    ./plot_kvsweep.py baselines/260903_kvsweep -o docs/figures

Two knobs were swept independently, both against the same engine, generator,
key count, value size and offered rate, so each panel isolates one variable.

    fig11   write-cold against T, one curve per skew
    fig11b  write-cold against skew, one curve per T -- the same data rotated,
            because the interesting thing is a threshold in theta rather than
            a trend in T
    fig11c  write-cold against T, one curve per read ratio

fig11c is the result that matters. Going from half writes to no writes at all
moves write-coldness by about one point. If client writes were what dirtied a
page, pure-GET traffic would leave ~100% of pages write-cold. It does not,
because redis writes robj->lru on every lookupKey: a read IS a write.

Input is rw_census.py's CSV, one file per sweep point:
    workload,T_sec,pages,write_cold_pct,cold_pct,windows_averaged,
    cold_method,write_cold_sd,cold_sd,freed_pct
"""
import csv, os, sys, argparse
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

ap = argparse.ArgumentParser()
ap.add_argument("src", help="directory of <point>.csv files, or the files")
ap.add_argument("-o", "--out", default="docs/figures")
ap.add_argument("--engine", default="redis")
a = ap.parse_args()

TAB_CB = ["#006BA4", "#FF800E", "#595959", "#C85200", "#5F9ED1",
          "#ABABAB", "#A2C8EC", "#FFBC79"]
GREY = "#3d474e"
FAINT = "#7A838A"

# Point name -> (label, order). Kept explicit rather than derived from the
# filename so the legend reads as a physical quantity and not as a slug.
SKEW = [("skew-uniform", "uniform"),
        ("skew-t050",    r"$\theta$ = 0.5"),
        ("skew-t080",    r"$\theta$ = 0.8"),
        ("skew-t099",    r"$\theta$ = 0.99  (YCSB default)"),
        ("skew-t120",    r"$\theta$ = 1.2")]
SKEW_X = [None, 0.5, 0.8, 0.99, 1.2]        # None = uniform, plotted at x=0
RATIO = [("ratio-C-100r", "YCSB-C — 100% reads"),
         ("skew-t099",    "YCSB-B — 95% reads, 5% writes"),
         ("ratio-A-50r",  "YCSB-A — 50% reads, 50% writes")]


def load(src):
    files = []
    if os.path.isdir(src):
        files = [os.path.join(src, f) for f in os.listdir(src) if f.endswith(".csv")]
    else:
        files = [src]
    out = {}
    for p in files:
        key = os.path.basename(p)[:-4]
        with open(p) as f:
            rows = [r for r in csv.DictReader(f)]
        if not rows:
            continue
        out[key] = sorted(((float(r["T_sec"]), float(r["write_cold_pct"]),
                            float(r.get("write_cold_sd") or 0)) for r in rows),
                          key=lambda t: t[0])
    return out


def frame(ax, xlabel, ylabel, title, sub=None):
    ax.set_xlabel(xlabel, fontsize=10.5)
    ax.set_ylabel(ylabel, fontsize=10.5)
    ax.set_title(title, fontsize=13, weight="semibold",
                 pad=26 if sub else 10)
    if sub:
        ax.text(0.5, 1.015, sub, transform=ax.transAxes, ha="center",
                va="bottom", fontsize=9.2, color=FAINT)
    ax.grid(alpha=.25, lw=.5); ax.set_axisbelow(True)
    ax.set_ylim(0, 100)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)


def place_labels(ax, items, min_sep=4.2, xfac=1.075):
    """End-of-curve labels that do not sit on top of each other.

    These curves converge by construction -- that convergence IS the result --
    so the settled values must stay readable exactly where they collide. Push
    the labels apart to a minimum separation, recentre the block on its own
    mean so nothing drifts, and draw a leader back to the real point.
    """
    items = sorted(items, key=lambda t: t[1])
    ys = [y for _, y, _, _ in items]
    for i in range(1, len(ys)):
        if ys[i] - ys[i - 1] < min_sep:
            ys[i] = ys[i - 1] + min_sep
    shift = (sum(y for _, y, _, _ in items) - sum(ys)) / len(ys)
    ys = [y + shift for y in ys]
    for (x, y0, txt, col), y in zip(items, ys):
        moved = abs(y - y0) > 0.6
        ax.annotate(txt, xy=(x, y0), xytext=(x * xfac, y), textcoords="data",
                    va="center", ha="left", fontsize=8.6, color=col,
                    arrowprops=dict(arrowstyle="-", lw=.6, color=col,
                                    alpha=.5, shrinkA=1, shrinkB=2)
                    if moved else None)


def note(fig, txt):
    fig.text(0.012, 0.012, txt, fontsize=7.8, color=FAINT,
             va="bottom", linespacing=1.5)


def curves(data, spec, fname, title, sub, legend_title, tail):
    """write-cold against T, one curve per sweep point."""
    fig, ax = plt.subplots(figsize=(8.4, 5.2))
    ends = []
    for i, (key, label) in enumerate(spec):
        if key not in data:
            print(f"  ! missing {key}", file=sys.stderr); continue
        ts = [t for t, _, _ in data[key]]
        ys = [y for _, y, _ in data[key]]
        es = [e for _, _, e in data[key]]
        ax.errorbar(ts, ys, yerr=es, marker="o", ms=5, lw=2, capsize=3,
                    color=TAB_CB[i], label=label, zorder=3)
        # Label the settled end of each curve rather than relying on the eye
        # to match line colour to legend entry.
        ends.append((ts[-1], ys[-1], f"{ys[-1]:.1f}%", TAB_CB[i]))
    place_labels(ax, ends)
    ax.set_xscale("log")
    ax.set_xticks([60, 120, 300, 600])
    ax.set_xticklabels(["60", "120", "300", "600"])
    ax.set_xlim(50, 1000)
    frame(ax, "T — seconds since the bits were cleared",
          "Pages not written in T  (%)", title, sub)
    ax.legend(fontsize=9.5, frameon=False, title=legend_title,
              title_fontsize=9.5, loc="upper right")
    note(fig, tail)
    fig.tight_layout(rect=(0, 0.045, 1, 1))
    p = os.path.join(a.out, fname)
    fig.savefig(p, dpi=200); plt.close(fig)
    print("  ->", p)


def cliff(data, fname):
    """The same numbers rotated: write-cold against skew, one curve per T.

    Worth its own figure because the story is a threshold, not a trend. Every
    theta below 1.2 collapses to the same few percent by ten minutes; 1.2 does
    not. Skew buys time, not a floor.
    """
    fig, ax = plt.subplots(figsize=(8.4, 5.2))
    Ts = sorted({t for k, _ in SKEW if k in data for t, _, _ in data[k]})
    xs = list(range(len(SKEW)))
    for i, T in enumerate(Ts):
        ys, es = [], []
        for key, _ in SKEW:
            row = next((r for r in data.get(key, []) if r[0] == T), None)
            ys.append(row[1] if row else float("nan"))
            es.append(row[2] if row else 0)
        ax.errorbar(xs, ys, yerr=es, marker="o", ms=5, lw=2, capsize=3,
                    color=TAB_CB[i], label=f"T = {int(T)} s", zorder=3)
    ax.set_xticks(xs)
    ax.set_xticklabels([lbl.replace(r"$\theta$ = ", "").replace("  (YCSB default)", "\n(YCSB\ndefault)")
                        for _, lbl in SKEW], fontsize=9.5)
    ax.set_xlim(-0.35, len(SKEW) - 0.65)
    frame(ax, "Request distribution", "Pages not written in T  (%)",
          "Skew buys time, not a floor",
          "redis · 7 M keys × 140 B · 95% reads · 10,000 ops/s")
    ax.legend(fontsize=9.5, frameon=False, loc="upper left")
    note(fig,
         "Every skew below θ=1.2 decays to the same 4–7% by ten minutes; only θ=1.2 holds a\n"
         "large read-mostly set. The x-axis is categorical — uniform is not θ=0.")
    fig.tight_layout(rect=(0, 0.055, 1, 1))
    p = os.path.join(a.out, fname)
    fig.savefig(p, dpi=200); plt.close(fig)
    print("  ->", p)


data = load(a.src)
os.makedirs(a.out, exist_ok=True)

curves(data, SKEW, "fig11-redis-skew.png",
       "Write-coldness against request skew",
       "redis · 7 M keys × 140 B · 95% reads · 10,000 ops/s · 2 passes",
       "zipfian",
       "Error bars are the spread across 2 passes. Skew is the YCSB ZipfianGenerator\n"
       "(zeta precomputed, then inverted); uniform is a flat draw over all 7 M keys.")

cliff(data, "fig11b-redis-skew-cliff.png")

curves(data, RATIO, "fig11c-redis-read-ratio.png",
       "Write-coldness barely depends on the write ratio",
       r"redis · 7 M keys × 140 B · zipfian $\theta$ = 0.99 · 10,000 ops/s · 2 passes",
       "YCSB workload",
       "Removing every client write moves the settled value by ~1 point. redis writes\n"
       "robj->lru on every lookupKey, so a GET dirties the page it reads.")
