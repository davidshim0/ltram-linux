#!/usr/bin/env python3
"""Plot what the self-test measured.

    ./plot_selftest.py <dir with the csv files> [-o outdir]

Reads whatever is present and skips the rest, so a partial run still plots:

  read-vs-erase.csv   read latency against how hard the engine is erasing.
                      The design question behind erase_batch=1 and the
                      erase_poll_ms spacing, as a curve rather than one point.
  promote-rate.csv    the measured promotion rate, used to derive how long a
                      full fill takes at each interval.
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
# Same language as the sweep figures: colour is the medium, shape is the
# cache state, reference lines are grey so nothing but data wears a data
# colour.
C_NOR, C_DRAM, C_MODEL, C_GRID = "#9E2F33", "#1F5F7A", "#6B7280", "#868E8A"
M_COLD, M_WARM, M_COMP = "o", "^", "s"

def rows(name, delim=","):
    p = os.path.join(a.dir, name)
    if not os.path.exists(p): return None
    with open(p) as f: return list(csv.DictReader(f, delimiter=delim))

def legend_by_last(ax, **kw):
    """Legend entries in the order the lines finish, top to bottom.

    A reader's eye goes from the right-hand end of a curve to the legend, so a
    legend in call order rather than visual order makes them do the matching
    by hand. Sorting by each line's last finite y removes that, and keeps
    doing so when the data changes.
    """
    h, l = ax.get_legend_handles_labels()
    def lasty(handle):
        try:
            ys = [v for v in handle.get_ydata() if v == v]
            return ys[-1] if ys else float("-inf")
        except Exception:
            return float("-inf")
    order = sorted(range(len(h)), key=lambda i: -lasty(h[i]))
    ax.legend([h[i] for i in order], [l[i] for i in order], **kw)

def frame(ax, title, xl, yl, note=None):
    ax.set_title(title, fontsize=13, weight="semibold", pad=10)
    ax.set_xlabel(xl); ax.set_ylabel(yl); ax.grid(alpha=.25, lw=.5)
    # note is accepted and ignored: the figures carry no prose. What each one
    # shows belongs wherever it is being presented, not baked into the image.

made = []
r = rows("read-vs-erase.csv")
if r:
    def col(x, *names):
        for n in names:
            if n in x and x[n] not in ("", None): return float(x[n])
        return None
    def poll_val(x):
        return 1e9 if x["poll_ms"] == "off" else float(x["poll_ms"])
    US = 1000.0                     # ns -> us
    base = next((col(x, "mean_ns", "ns_per_line") for x in r if x["poll_ms"] == "off"), None)
    has_dist = "max_ns" in r[0]

    TARGET = 20.0        # the operating point worth quoting
    def at_poll(rows_, target, key):
        """Value at a poll setting: measured if present, linear between if not."""
        pts = sorted(((poll_val(x), col(x, key, "ns_per_line")) for x in rows_
                      if x["poll_ms"] != "off" and col(x, key, "ns_per_line") is not None),
                     key=lambda t: t[0])
        for px, py in pts:
            if abs(px - target) < 1e-9:
                return py
        lo_ = max((t for t in pts if t[0] < target), default=None)
        hi_ = min((t for t in pts if t[0] > target), default=None)
        if not lo_ or not hi_:
            return None
        return lo_[1] + (target - lo_[0]) / (hi_[0] - lo_[0]) * (hi_[1] - lo_[1])

    def draw(xs, ys, lo, hi, xlab, fname, title, xlim_left=None, mark=None):
        fig, ax = plt.subplots(figsize=(7.2, 4.6))
        if base:
            ax.axhline(base / US, color=C_GRID, ls="--", lw=1)
            ax.annotate(f"read-only baseline  {base/US:.2f} us",
                        (max(xs), base / US), fontsize=8.5, color="#3d474e",
                        ha="right", va="top", xytext=(0, -5),
                        textcoords="offset points")
        if lo and hi:
            ax.fill_between(xs, lo, hi, color=C_NOR, alpha=.15, lw=0,
                            label="min to max across passes")
        ax.plot(xs, ys, marker=M_COLD, ls="-", color=C_NOR, lw=1.8, ms=5.5, zorder=3,
                label="mean" if lo else None)
        if mark and base:
            mx_, my_ = mark
            ax.plot([mx_], [my_ / US], marker=M_COLD, ls="none", color=C_NOR, ms=8,
                    mfc="none", mew=2, zorder=5)
            ax.annotate(f"{TARGET:.0f} ms:  {my_/US:.2f} us  (+{(my_/base-1)*100:.0f}%)",
                        (mx_, my_ / US), fontsize=9, color="#3d474e",
                        ha="left", va="bottom", xytext=(8, 6),
                        textcoords="offset points")
        ax.set_ylim(bottom=0)
        if xlim_left is not None: ax.set_xlim(left=xlim_left)
        if lo: legend_by_last(ax, fontsize=9, frameon=False, loc="upper left")
        frame(ax, title, xlab, "NOR Read Latency (us)")
        fig.tight_layout(); fig.savefig(f"{OUT}/{fname}", dpi=200)

    TITLE = "Read latency with background erases"
    ra = sorted(r, key=lambda x: float(x["erase_rate_per_s"]))
    draw([float(x["erase_rate_per_s"]) for x in ra],
         [col(x, "mean_ns", "ns_per_line") / US for x in ra],
         [col(x, "min_ns") / US for x in ra] if has_dist else None,
         [col(x, "max_ns") / US for x in ra] if has_dist else None,
         "Erases/Second", "fig4-read-vs-erase.png", TITLE, xlim_left=-1.5,
         mark=(at_poll(r, TARGET, "erase_rate_per_s"), at_poll(r, TARGET, "mean_ns"))
              if at_poll(r, TARGET, "mean_ns") and at_poll(r, TARGET, "erase_rate_per_s") else None)
    made.append("fig4-read-vs-erase")

    rb = sorted([x for x in r if x["poll_ms"] != "off"], key=poll_val)
    if rb:
        draw([poll_val(x) for x in rb],
             [col(x, "mean_ns", "ns_per_line") / US for x in rb],
             [col(x, "min_ns") / US for x in rb] if has_dist else None,
             [col(x, "max_ns") / US for x in rb] if has_dist else None,
             "Erase Interval (ms)", "fig4b-read-vs-interval.png", TITLE, xlim_left=-4,
             mark=(TARGET, at_poll(rb, TARGET, "mean_ns")) if at_poll(rb, TARGET, "mean_ns") else None)
        made.append("fig4b-read-vs-interval")

r = rows("promote-rate.csv")
if r:
    r = sorted(r, key=lambda x: float(x["interval_ms"]))
    iv = [float(x["interval_ms"]) for x in r]
    me = [float(x["measured_per_s"]) for x in r]
    pr = [float(x["predicted_per_s"]) for x in r]
    # pool / rate: no new measurement, and it assumes a working set large
    # enough that the scanner never runs short of candidates.
    POOL = 65536
    byiv = {}
    for i_, m_ in zip(iv, me):
        byiv.setdefault(i_, []).append(m_)
    iv = sorted(byiv)
    me = [sum(byiv[k]) / len(byiv[k]) for k in iv]
    mins = [POOL / m / 60 for m in me]
    # I2 measures the idle erase period at ~22 ms; 20 is the round number the
    # rest of the project quotes for it, and the claim -- promotion cannot
    # outrun recycling -- does not turn on the difference.
    ERASE_MS = 20
    DEFAULT_MS = 24     # the five-year service budget
    fig, ax = plt.subplots(figsize=(7.6, 4.8))

    # Promotion cannot outrun recycling for long: every promoted page needs an
    # erase eventually, so a write interval shorter than the erase period
    # drains the pool faster than the engine can refill it.
    ax.axvline(ERASE_MS, color=C_GRID, ls="-.", lw=1.2)
    ax.axvline(DEFAULT_MS, color=C_GRID, ls="--", lw=1.2)

    ax.plot(iv, mins, marker=M_COLD, ls="-", color=C_NOR, lw=1.8, ms=5.5, zorder=3)
    for x_, y_ in zip(iv, mins):
        ax.vlines(x_, 0, y_, color=C_GRID, ls=":", lw=.9, zorder=1)
        ax.annotate(f"{y_:.0f}" if y_ >= 10 else f"{y_:.1f}", (x_, y_),
                    textcoords="offset points", xytext=(0, 8), ha="center",
                    fontsize=9, color="#3d474e")
    ax.set_ylim(bottom=0)
    ax.set_xlim(left=0, right=max(max(iv), DEFAULT_MS) * 1.12)
    # An explicit tick per measured point, so every x reads off the axis.
    ax.set_xticks(sorted(set(iv)))
    ax.set_xticklabels([f"{v:g}" for v in sorted(set(iv))])
    top = ax.get_ylim()[1]
    ax.annotate(f"erase limit\n{ERASE_MS:g} ms", (ERASE_MS, top), color=C_GRID,
                fontsize=8.5, ha="right", va="top", xytext=(-4, -4),
                textcoords="offset points")
    ax.annotate(f"5-year default\n{DEFAULT_MS:g} ms", (DEFAULT_MS, top), color=C_GRID,
                fontsize=8.5, ha="left", va="top", xytext=(4, -4),
                textcoords="offset points")
    frame(ax, "Time to fill NOR against write interval",
          "Write Interval (ms)", "Time to fill NOR (min)")
    fig.tight_layout(); fig.savefig(f"{OUT}/fig5-time-to-fill.png", dpi=200)
    made.append("fig5-time-to-fill")

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
    fig.tight_layout(); fig.savefig(f"{OUT}/fig7-transition-timeline.png", dpi=160)
    made.append("fig7-transition-timeline")

r = rows("wear-history.tsv", "\t")
if r and len(r) > 2:
    ok_ = [x for x in r if x.get("min", "?") not in ("?", "", None)
           and x.get("max", "?") not in ("?", "", None)]
    if len(ok_) > 2:
        tot = [float(x["total"]) / 1e6 for x in ok_]
        mn = [float(x["min"]) for x in ok_]
        mx = [float(x["max"]) for x in ok_]
        mean = [float(x["mean"]) for x in ok_]
        med = [float(x["median"]) for x in ok_] if ok_[0].get("median", "?") not in ("?", "", None) else None
        fig, ax = plt.subplots(figsize=(7.2, 4.6))
        ax.fill_between(tot, mn, mx, color=C_NOR, alpha=.13, lw=0)
        ax.plot(tot, mx, "-", color=C_NOR, lw=1.6, label="max")
        ax.plot(tot, mean, color=C_MODEL, lw=1.3, ls="--", label="mean")
        if med:
            ax.plot(tot, med, color=C_COMP if "C_COMP" in dir() else C_MODEL, lw=1.3, ls=":", label="median")
        ax.plot(tot, mn, "-", color=C_NOR, lw=1.6, label="min")
        ax.set_ylim(bottom=0)
        frame(ax, "Wear Spread",
              "Total erases on NOR (millions)", "Erase Count of a Single Page")
        legend_by_last(ax, fontsize=9, frameon=False, loc="upper left")
        fig.tight_layout(); fig.savefig(f"{OUT}/fig6-wear-spread.png", dpi=200)
        made.append("fig6-wear-spread")

print("plotted:", ", ".join(made) if made else "nothing found")
for m in made: print(f"  {OUT}/{m}.png")
