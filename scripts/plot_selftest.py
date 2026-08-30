#!/usr/bin/env python3
"""Plot what the self-test measured.

    ./plot_selftest.py <dir with the csv files> [-o outdir]

Reads whatever is present and skips the rest, so a partial run still plots:

  read-vs-erase.csv   read latency against how hard the engine is erasing.
                      The design question behind erase_batch=1 and the
                      erase_poll_ms spacing, as a curve rather than one point.
  promote-rate.csv    achieved promotion rate against the configured budget
                      interval, with the model 1000/(interval + 3 ms) drawn
                      through it -- the 3 ms is scan plus migration per tick.
  fill-curve.csv      residency against time for one fill.
  wear-history.tsv    spread against mean over the campaign. Falling means
                      wear levelling is tightening.
"""
import csv, os, sys, argparse
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

ap = argparse.ArgumentParser()
ap.add_argument("dir"); ap.add_argument("-o", "--out", default=None)
a = ap.parse_args(); OUT = a.out or a.dir
C_NOR, C_DRAM, C_MODEL, C_GRID = "#9E2F33", "#1F5F7A", "#6B7280", "#868E8A"

def rows(name, delim=","):
    p = os.path.join(a.dir, name)
    if not os.path.exists(p): return None
    with open(p) as f: return list(csv.DictReader(f, delimiter=delim))

def frame(ax, title, xl, yl, note=None):
    ax.set_title(title, fontsize=13, weight="semibold", pad=10)
    ax.set_xlabel(xl); ax.set_ylabel(yl); ax.grid(alpha=.25, lw=.5)
    # note is accepted and ignored: the figures carry no prose. What each one
    # shows belongs wherever it is being presented, not baked into the image.

made = []
r = rows("read-vs-erase.csv")
if r:
    # Tolerate both shapes: the first runs recorded only a mean.
    def col(x, *names):
        for n in names:
            if n in x and x[n] not in ("", None): return float(x[n])
        return None
    def poll_val(x):        # "off" sorts to the far right on an interval axis
        return 1e9 if x["poll_ms"] == "off" else float(x["poll_ms"])

    base = next((col(x, "mean_ns", "ns_per_line") for x in r if x["poll_ms"] == "off"), None)
    has_dist = "max_ns" in r[0]

    def draw(xs, ys, lo, hi, xlab, fname, title, xlim_left=None, marker=None):
        fig, ax = plt.subplots(figsize=(8.5, 5.2))
        if base:
            ax.axhline(base, color=C_GRID, ls="--", lw=1.1)
            ax.annotate(f"read-only baseline — {base:.0f} ns/line",
                        (max(xs), base), fontsize=9, color=C_GRID, ha="right",
                        va="bottom", xytext=(0, 5), textcoords="offset points")
        if lo and hi:
            ax.fill_between(xs, lo, hi, color=C_NOR, alpha=.15, lw=0,
                            label="min to max across passes")
        ax.plot(xs, ys, "o-", color=C_NOR, lw=2, ms=6, zorder=3,
                label="mean" if lo else None)
        if marker is not None:
            ax.plot([marker[0]], [marker[1]], "o", mfc="none", mec=C_NOR, mew=2, ms=14, zorder=4)
        ax.set_ylim(bottom=0)
        if xlim_left is not None: ax.set_xlim(left=xlim_left)
        if lo: ax.legend(fontsize=9, frameon=False, loc="upper left")
        frame(ax, title, xlab, "ns per cache line, cold NOR reads")
        fig.tight_layout(); fig.savefig(f"{OUT}/{fname}", dpi=160)

    # (a) against what the engine actually achieved
    ra = sorted(r, key=lambda x: float(x["erase_rate_per_s"]))
    xs = [float(x["erase_rate_per_s"]) for x in ra]
    ys = [col(x, "mean_ns", "ns_per_line") for x in ra]
    lo = [col(x, "min_ns") for x in ra] if has_dist else None
    hi = [col(x, "max_ns") for x in ra] if has_dist else None
    d = next((x for x in ra if x["poll_ms"] == "30"), None)
    draw(xs, ys, lo, hi, "erases per second while the workload reads",
         "selftest-read-vs-erase.png", "Read latency against background erase rate",
         xlim_left=-1.5,
         marker=(float(d["erase_rate_per_s"]), col(d, "mean_ns", "ns_per_line")) if d else None)
    made.append("read-vs-erase")

    # (b) against the knob: how often an erase is issued
    rb = sorted([x for x in r if x["poll_ms"] != "off"], key=poll_val)
    if rb:
        xs = [poll_val(x) for x in rb]
        ys = [col(x, "mean_ns", "ns_per_line") for x in rb]
        lo = [col(x, "min_ns") for x in rb] if has_dist else None
        hi = [col(x, "max_ns") for x in rb] if has_dist else None
        d = next((x for x in rb if x["poll_ms"] == "30"), None)
        draw(xs, ys, lo, hi, "erase_poll_ms  (delay between erases)",
             "selftest-read-vs-interval.png", "Read latency against erase interval",
             xlim_left=-4,
             marker=(30.0, col(d, "mean_ns", "ns_per_line")) if d else None)
        made.append("read-vs-interval")

r = rows("promote-rate.csv")
if r:
    iv = [float(x["interval_ms"]) for x in r]
    me = [float(x["measured_per_s"]) for x in r]
    pr = [float(x["predicted_per_s"]) for x in r]
    fig, ax = plt.subplots(figsize=(8, 5))
    ax.plot(iv, pr, "s--", color=C_MODEL, lw=1.6, label="model: 1000/(interval + 3 ms)")
    ax.plot(iv, me, "o-", color=C_DRAM, lw=2, label="measured")
    ax.set_ylim(bottom=0)
    frame(ax, "Promotion rate against the budget interval", "interval_ms from cycles_left / seconds_left",
          "promotions per second",
          "The 3 ms is the scan and migration each tick does on top of the sleep, so the interval is a gap, not a period.")
    ax.legend(fontsize=9, frameon=False)
    fig.tight_layout(); fig.savefig(f"{OUT}/selftest-promote-rate.png", dpi=160); made.append("promote-rate")

r = rows("fill-curve.csv")
if r:
    fig, ax = plt.subplots(figsize=(8, 5))
    ax.plot([float(x["seconds"]) for x in r], [float(x["residency_pct"]) for x in r],
            "-", color=C_NOR, lw=2)
    ax.set_ylim(0, 100)
    frame(ax, "Filling flash: residency against time", "seconds since the fill started",
          "% of the weights in flash",
          "One promotion per tick at the configured budget. A fall would mean pages demoted while still filling.")
    fig.tight_layout(); fig.savefig(f"{OUT}/selftest-fill-curve.png", dpi=160); made.append("fill-curve")

r = rows("wear-history.tsv", "\t")
if r and len(r) > 2:
    ok = [x for x in r if x.get("mean", "?") not in ("?", "", None)]
    if len(ok) > 2:
        tot = [float(x["total"]) for x in ok]
        rel = [float(x["spread"]) / float(x["mean"]) * 100 for x in ok]
        fig, ax = plt.subplots(figsize=(8, 5))
        ax.plot(tot, rel, "o-", color=C_DRAM, lw=1.6, ms=3)
        ax.set_ylim(bottom=0)
        frame(ax, "Wear spread as the array is used", "total erases recorded",
              "spread as % of mean erase count",
              "Falling means levelling is tightening. Buckets are 1000 wide, so this is FIFO round-robin, not bucket order.")
        fig.tight_layout(); fig.savefig(f"{OUT}/selftest-wear-spread.png", dpi=160); made.append("wear-spread")

print("plotted:", ", ".join(made) if made else "nothing found")
for m in made: print(f"  {OUT}/selftest-{m}.png")
