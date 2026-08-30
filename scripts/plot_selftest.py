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
  timeline.csv        one workload across DRAM, migrating and flash.
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

    TARGET = 20.0        # the interval worth quoting: cheap, and well inside the flat region
    def at_poll(rows_, target, key):
        """Value at a poll setting, measured if we have it, linear if not."""
        pts = sorted(((poll_val(x), col(x, key, "ns_per_line")) for x in rows_
                      if x["poll_ms"] != "off" and col(x, key, "ns_per_line")), key=lambda t: t[0])
        for px, py in pts:
            if abs(px - target) < 1e-9:
                return py, True
        lo_ = max((t for t in pts if t[0] < target), default=None)
        hi_ = min((t for t in pts if t[0] > target), default=None)
        if not lo_ or not hi_:
            return None, False
        f = (target - lo_[0]) / (hi_[0] - lo_[0])
        return lo_[1] + f * (hi_[1] - lo_[1]), False

    def draw(xs, ys, lo, hi, xlab, fname, title, xlim_left=None, marker=None,
             callout=None):
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
        if callout and base:
            cx, cy, exact = callout
            ax.plot([cx], [cy], "D", color="#0f6b70", ms=7, zorder=5)
            ax.annotate(f"{TARGET:.0f} ms erase interval\n{cy/base:.2f}x the read-only baseline"
                        + ("" if exact else "  (interpolated)"),
                        (cx, cy), fontsize=9, color="#0f6b70", fontweight="medium",
                        xytext=(12, -30), textcoords="offset points",
                        arrowprops=dict(arrowstyle="-", color="#0f6b70", lw=1))
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
         marker=(float(d["erase_rate_per_s"]), col(d, "mean_ns", "ns_per_line")) if d else None,
         callout=(lambda v: (v[0], v[1], v[2]))(
             (at_poll(r, TARGET, "erase_rate_per_s")[0],
              at_poll(r, TARGET, "mean_ns")[0],
              at_poll(r, TARGET, "mean_ns")[1]))
             if at_poll(r, TARGET, "mean_ns")[0] and at_poll(r, TARGET, "erase_rate_per_s")[0] else None)
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
             marker=(30.0, col(d, "mean_ns", "ns_per_line")) if d else None,
             callout=(TARGET,) + at_poll(rb, TARGET, "mean_ns")
                     if at_poll(rb, TARGET, "mean_ns")[0] else None)
        made.append("read-vs-interval")

r = rows("promote-rate.csv")
if r:
    r = sorted(r, key=lambda x: float(x["interval_ms"]))
    iv = [float(x["interval_ms"]) for x in r]
    me = [float(x["measured_per_s"]) for x in r]
    pr = [float(x["predicted_per_s"]) for x in r]
    fig, ax = plt.subplots(figsize=(8.5, 5.2))
    # The two series coincide to 3 s.f., so drawing both as markers hides one
    # under the other. Model as a bare line, measurement as points on it.
    ax.plot(iv, pr, "-", color=C_MODEL, lw=3, alpha=.45, label="model: 1000/(interval + 3 ms)")
    ax.plot(iv, me, "o", color=C_DRAM, ms=7, zorder=3, label="measured")
    err = max(abs(a - b) / b * 100 for a, b in zip(me, pr))
    ax.annotate(f"measured within {err:.1f}% of the model at every point",
                (0.97, 0.92), xycoords="axes fraction", ha="right", fontsize=9.5,
                color="#3d474e")
    ax.set_ylim(bottom=0)
    frame(ax, "Promotion rate against the interval it was told to use",
          "promotion interval (ms between promotions)", "promotions per second")
    ax.legend(fontsize=9.5, frameon=False, loc="upper right", bbox_to_anchor=(1, .85))
    fig.tight_layout(); fig.savefig(f"{OUT}/selftest-promote-rate.png", dpi=160)
    made.append("promote-rate")

    # Same data, the question actually asked: how long to fill the flash.
    # pool / rate, so it assumes a working set large enough that the scanner
    # never runs short of candidates -- which is the case that matters, since
    # a fill that stalls for want of candidates is not measuring the pacing.
    POOL = 65536
    # The kernel clamps the interval at 1 ms, so two different budgets can
    # land on the same x and stack one marker on another. Collapse them so
    # the count of visible points matches the count of distinct settings.
    byiv = {}
    for i_, m_ in zip(iv, me):
        byiv.setdefault(i_, []).append(m_)
    iv = sorted(byiv)
    me = [sum(byiv[k]) / len(byiv[k]) for k in iv]
    fig, ax = plt.subplots(figsize=(8.5, 5.2))
    mins = [POOL / m / 60 for m in me]
    ax.plot(iv, mins, "o-", color=C_DRAM, lw=2, ms=7)
    for x, y in zip(iv, mins):
        ax.annotate(f"{y:.0f} min", (x, y), textcoords="offset points",
                    xytext=(0, 9), ha="center", fontsize=8.5, color="#3d474e")
    d24 = next((POOL / m / 60 for i_, m in zip(iv, me) if abs(i_ - 24) < 6), None)
    ax.set_ylim(bottom=0)
    frame(ax, f"Time to fill all {POOL:,} sectors",
          "promotion interval (ms between promotions)", "minutes")
    fig.tight_layout(); fig.savefig(f"{OUT}/selftest-time-to-fill.png", dpi=160)
    made.append("time-to-fill")

r = rows("timeline.csv")
if r:
    t = [float(x["elapsed_s"]) for x in r]
    ns = [float(x["ns_per_line"]) for x in r]
    ph = [int(x["phase"]) for x in r]
    fig, ax = plt.subplots(figsize=(9.5, 5.4))

    # Shade the three regimes rather than drawing three separate series: it is
    # one continuous workload and the point is that nothing about it changed
    # except where its pages live.
    for p_, col_, lab in ((1, "#1F5F7A", "DRAM"), (2, "#8a5320", "migrating"), (3, "#9E2F33", "flash")):
        xs = [x for x, q in zip(t, ph) if q == p_]
        if not xs: continue
        ax.axvspan(min(xs), max(xs), color=col_, alpha=.07, lw=0)
        mid = (min(xs) + max(xs)) / 2
        mean = sum(y for y, q in zip(ns, ph) if q == p_) / len(xs)
        ax.hlines(mean, min(xs), max(xs), color=col_, lw=2, zorder=4)
        ax.annotate(f"{lab}\n{mean:.0f} ns/line", (mid, mean), ha="center", va="bottom",
                    fontsize=9.5, color=col_, fontweight="medium",
                    xytext=(0, 8), textcoords="offset points")
    ax.plot(t, ns, "-", color="#3d474e", lw=.8, alpha=.55, zorder=3)
    ax.set_ylim(bottom=0)

    rr = [(float(x["elapsed_s"]), float(x["resid_pct"])) for x in r if x["resid_pct"]]
    if rr:
        ax2 = ax.twinx()
        ax2.plot([a for a, _ in rr], [b for _, b in rr], "-", color="#0f6b70", lw=1.8)
        ax2.set_ylabel("% of the weights in flash", color="#0f6b70")
        ax2.tick_params(axis="y", colors="#0f6b70")
        ax2.set_ylim(0, 105)
    frame(ax, "One workload, three regimes: DRAM, migrating, flash",
          "seconds", "ns per cache line")
    fig.tight_layout(); fig.savefig(f"{OUT}/selftest-timeline.png", dpi=160)
    made.append("timeline")

r = rows("wear-history.tsv", "\t")
if r and len(r) > 2:
    ok_ = [x for x in r if x.get("min", "?") not in ("?", "", None)
           and x.get("max", "?") not in ("?", "", None)]
    if len(ok_) > 2:
        tot = [float(x["total"]) for x in ok_]
        mn = [float(x["min"]) for x in ok_]
        mx = [float(x["max"]) for x in ok_]
        mean = [float(x["mean"]) for x in ok_]
        fig, ax = plt.subplots(figsize=(8.5, 5.2))
        # Absolute erase counts, not a ratio of the mean. "The worst sector is
        # N erases ahead of the best" is a number you can hold against the
        # 100,000-cycle budget; "spread is 42% of mean" is not.
        ax.fill_between(tot, mn, mx, color=C_DRAM, alpha=.16, lw=0)
        ax.plot(tot, mx, "-", color=C_NOR, lw=1.8, label="max")
        ax.plot(tot, mean, color=C_MODEL, lw=1.2, ls="--", label="mean")
        ax.plot(tot, mn, "-", color=C_DRAM, lw=1.8, label="min")
        gap = mx[-1] - mn[-1]
        ax.annotate(f"spread {gap:.0f} erases\n{gap/100000*100:.3f}% of the 100,000-cycle budget",
                    (tot[-1], mx[-1]), ha="right", va="bottom", fontsize=9.5,
                    color="#3d474e", xytext=(-6, 10), textcoords="offset points")
        ax.set_ylim(bottom=0)
        frame(ax, "How far apart the least and most worn sectors are",
              "total erases recorded across the array", "erase count of a single sector")
        ax.legend(fontsize=9, frameon=False, loc="upper left")
        fig.tight_layout(); fig.savefig(f"{OUT}/selftest-wear-spread.png", dpi=160)
        made.append("wear-spread")

print("plotted:", ", ".join(made) if made else "nothing found")
for m in made: print(f"  {OUT}/selftest-{m}.png")
