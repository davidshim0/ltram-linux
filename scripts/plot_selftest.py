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
    # SORT BY THE X AXIS. The CSV is in measurement order -- off, 0, 10, 30,
    # 60, 120 -- but x is the erase RATE, which runs the other way: a poll of
    # 0 ms erases fastest and 120 ms slowest. Plotting in file order sent the
    # line out to 36/s and then walked it back left across every other point.
    r = sorted(r, key=lambda x: float(x["erase_rate_per_s"]))
    er = [float(x["erase_rate_per_s"]) for x in r]
    ns = [float(x["ns_per_line"]) for x in r]
    base = next((float(x["ns_per_line"]) for x in r if x["poll_ms"] == "off"), ns[0])

    fig, ax = plt.subplots(figsize=(8.5, 5.2))
    ax.axhline(base, color=C_GRID, ls="--", lw=1.1)
    ax.annotate(f"engine off — {base:.0f} ns/line", (max(er), base), fontsize=9,
                color=C_GRID, ha="right", va="bottom",
                xytext=(0, 5), textcoords="offset points")
    ax.plot(er, ns, "o-", color=C_NOR, lw=2, ms=6, zorder=3)

    # The poll setting and the achieved rate are two views of one knob, so
    # labelling every point repeats the x axis. Put the mapping on a second
    # axis instead: same positions, no clutter over the data.
    ax.set_ylim(bottom=0)
    ax.set_xlim(left=-1.5, right=max(er) * 1.08)
    top = ax.twiny()
    top.set_xlim(ax.get_xlim())
    top.set_xticks(er)
    top.set_xticklabels(["off" if x["poll_ms"] == "off" else x["poll_ms"] for x in r], fontsize=9)
    top.set_xlabel("erase_poll_ms  (the setting; 30 is the default)", fontsize=10, labelpad=8)
    top.tick_params(axis="x", length=3)
    # Mark where the system actually runs.
    d = next((x for x in r if x["poll_ms"] == "30"), None)
    if d:
        ax.plot([float(d["erase_rate_per_s"])], [float(d["ns_per_line"])],
                "o", mfc="none", mec=C_NOR, mew=2, ms=14, zorder=4)
    frame(ax, "Read latency against background erase rate",
          "erases per second while the workload reads",
          "ns per cache line, cold NOR reads")
    fig.tight_layout(); fig.savefig(f"{OUT}/selftest-read-vs-erase.png", dpi=160)
    made.append("read-vs-erase")

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
