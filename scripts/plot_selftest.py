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
  worstcase.csv       the same, but the second migration has to wait for the
                      erase engine to free the sectors it wants.
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
    has_p99 = "p99_ns" in r[0]

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

    def draw(xs, ys, lo, hi, xlab, fname, title, xlim_left=None, mark=None,
             worst=None, band_label="min to p90 across passes", note=None):
        fig, ax = plt.subplots(figsize=(7.2, 4.6))
        if base:
            ax.axhline(base / US, color=C_GRID, ls="--", lw=1)
            ax.annotate(f"read-only baseline  {base/US:.2f} us",
                        (max(xs), base / US), fontsize=8.5, color="#3d474e",
                        ha="right", va="top", xytext=(0, -5),
                        textcoords="offset points")
        if lo and hi:
            # min to p90, not min to max. The max is the single worst pass out
            # of ~150, so it is one sample and need not move monotonically --
            # at poll 60 it reads 2089 against a p90 of 1379, one outlier
            # sitting 700 ns above the rest of the distribution. The envelope
            # is the thing that behaves; the outlier is worth showing, but not
            # worth letting define the band.
            ax.fill_between(xs, lo, hi, color=C_NOR, alpha=.15, lw=0,
                            label=band_label)
        if worst:
            ax.plot(xs, worst, marker="x", ls="none", color=C_NOR, ms=5,
                    mew=1.2, alpha=.6, label="p99" if has_p99 else "worst single pass")
        ax.plot(xs, ys, marker=M_COLD, ls="-", color=C_NOR, lw=1.8, ms=5.5, zorder=3,
                label="mean" if lo else None)
        if mark and base:
            mx_, my_ = mark
            ax.plot([mx_], [my_ / US], marker=M_COLD, ls="none", color=C_NOR,
                    ms=8, zorder=5)
            ax.annotate(f"{TARGET:.0f} ms:  {my_/US:.2f} us  (+{(my_/base-1)*100:.0f}%)",
                        (mx_, my_ / US), fontsize=9, color="#3d474e",
                        ha="left", va="bottom", xytext=(8, 6),
                        textcoords="offset points")
        ax.set_ylim(bottom=0)
        if xlim_left is not None: ax.set_xlim(left=xlim_left)
        if note:
            ax.annotate(note, (0.985, 0.97), xycoords="axes fraction",
                        ha="right", va="top", fontsize=8, color="#535B58",
                        linespacing=1.4)
        if lo: legend_by_last(ax, fontsize=9, frameon=False, loc="upper left")
        frame(ax, title, xlab, "NOR Read Latency (us)")
        fig.tight_layout(); fig.savefig(f"{OUT}/{fname}", dpi=200)

    TITLE = "Read latency with background erases"
    LINES = 1448 * 1448 * 4 // 128          # the pass J measures
    NOTE = ("each point is one full pass:\n"
            f"pass time / {LINES:,} cache lines,\n"
            "so a single stall is averaged away")

    # Against the INTERVAL only. The erase-rate view said the same thing with
    # a less useful x: the rate is an outcome of the setting and of contention
    # with the reader, so two settings could land on the same rate.
    rb = sorted([x for x in r if x["poll_ms"] != "off"], key=poll_val)
    if rb:
        xs = [poll_val(x) for x in rb]
        ys = [col(x, "mean_ns", "ns_per_line") / US for x in rb]
        mn = [col(x, "min_ns") / US for x in rb] if has_dist else None
        p90 = [col(x, "p90_ns") / US for x in rb] if has_dist else None
        mx = [col(x, "max_ns") / US for x in rb] if has_dist else None
        p99 = [col(x, "p99_ns", "max_ns") / US for x in rb] if has_dist else None
        m20 = at_poll(rb, TARGET, "mean_ns")

        # Two versions of the same measurement: the envelope that behaves, and
        # the envelope that includes the single worst pass.
        draw(xs, ys, mn, p90, "Erase Interval (ms)", "fig4-read-vs-interval.png",
             TITLE, xlim_left=-4,
             mark=(TARGET, m20) if m20 else None,
             worst=p99, band_label="min to p90 across passes", note=NOTE)
        made.append("fig4-read-vs-interval")

        draw(xs, ys, mn, mx, "Erase Interval (ms)", "fig4b-read-vs-interval-max.png",
             TITLE, xlim_left=-4,
             mark=(TARGET, m20) if m20 else None,
             band_label="min to max across passes", note=NOTE)
        made.append("fig4b-read-vs-interval-max")

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
    # Three references, all grey, all labelled at the bottom.
    WRITE_MS  = 1      # the governor's floor: it will not pace faster than this
    ERASE_MS  = 20     # I2's idle erase period, rounded: recycling cannot keep up below it
    DEFAULT_MS = 24    # the five-year service budget
    fig, ax = plt.subplots(figsize=(7.6, 4.8))
    ax.plot(iv, mins, marker=M_COLD, ls="-", color=C_NOR, lw=1.8, ms=5.5, zorder=3)
    for x_, y_ in zip(iv, mins):
        ax.vlines(x_, 0, y_, color=C_GRID, ls=":", lw=.9, zorder=1)
        # Above the marker, except for the highest point, which would sit on
        # the frame -- that one goes to its left, where the axis is empty.
        top_pt = (y_ == max(mins))
        ax.annotate(f"{y_:.0f}" if y_ >= 10 else f"{y_:.1f}", (x_, y_),
                    textcoords="offset points",
                    xytext=((-10, -4) if top_pt else (0, 9)),
                    ha=("right" if top_pt else "center"),
                    va=("center" if top_pt else "bottom"),
                    fontsize=9, color="#3d474e")
    ax.set_ylim(bottom=0, top=max(mins) * 1.12)
    ax.set_xlim(left=0, right=max(max(iv), DEFAULT_MS) * 1.12)
    ax.set_xticks(sorted(set(iv)))
    ax.set_xticklabels([f"{v:g}" for v in sorted(set(iv))])
    bot = ax.get_ylim()[0]
    def yat(xv):
        """Curve height at xv: measured if we have it, linear between if not."""
        pts = sorted(zip(iv, mins))
        for px, py in pts:
            if abs(px - xv) < 1e-9:
                return py
        lo_ = max((t for t in pts if t[0] < xv), default=None)
        hi_ = min((t for t in pts if t[0] > xv), default=None)
        if not lo_ or not hi_:
            return max(mins)
        return lo_[1] + (xv - lo_[0]) / (hi_[0] - lo_[0]) * (hi_[1] - lo_[1])

    for xv, lab in ((WRITE_MS, "write limit"), (ERASE_MS, "erase limit"),
                    (DEFAULT_MS, "5-year default")):
        # Stop at the curve. A full-height line implies the reference means
        # something above the data, and it does not -- it marks an interval,
        # which is an x, and the curve is where that interval lands.
        ax.vlines(xv, 0, yat(xv), color=C_GRID, ls="--", lw=1.1, zorder=0)
        ax.annotate(lab, (xv, bot), color=C_GRID, fontsize=8,
                    ha="right", va="bottom", rotation=90,
                    xytext=(-3, 6), textcoords="offset points")
    frame(ax, "Time to fill NOR against write interval",
          "Write Interval (ms)", "Time to fill NOR (min)")
    fig.tight_layout(); fig.savefig(f"{OUT}/fig5-time-to-fill.png", dpi=200)
    made.append("fig5-time-to-fill")

r = rows("timeline.csv")
if r:
    t = [float(x["elapsed_s"]) for x in r]
    ns = [float(x["ns_per_line"]) for x in r]
    ph = [int(x["phase"]) for x in r]

    # Trust the recorded boundary when the run could actually see residency,
    # and only fall back to deriving one when it could not.
    #
    # The derivation -- first pass within 2% of the flash mean -- was written
    # when RESID was being emitted from the chase branch, so the harness never
    # saw 99% and marked the boundary 1,500 s late. With residency working it
    # is worse than useless: the migration OVERSHOOTS the settled value, so
    # "first within 2% of it" fires during the climb. On this run that put the
    # boundary at 322 s instead of 406 s and folded the overshoot and the drop
    # into a "flash" phase averaging 1036 ns, which is neither.
    if not any(x["resid_pct"] for x in r):
        p1 = [v for v, q in zip(ns, ph) if q == 1]
        p3 = [v for v, q in zip(ns, ph) if q == 3]
        if p1 and p3:
            lo, hi = sum(p1) / len(p1), sum(p3) / len(p3)
            thresh = lo + 0.98 * (hi - lo)
            cross = next((i for i, (v, q) in enumerate(zip(ns, ph))
                          if q >= 2 and v >= thresh), None)
            if cross is not None:
                ph = [q if q == 1 else (2 if i <= cross else 3)
                      for i, q in enumerate(ph)]

    # Keep 60 s of the settled regime: long enough to show the overshoot come
    # back down, short enough that it does not dominate the axis.
    p3i = [i for i, q in enumerate(ph) if q == 3]
    if p3i:
        cut = t[p3i[0]] + 60
        keep = [i for i, x in enumerate(t) if x <= cut]
        t = [t[i] for i in keep]; ns = [ns[i] for i in keep]; ph = [ph[i] for i in keep]

    fig, ax = plt.subplots(figsize=(9.5, 5.2))
    top = max(ns) * 1.16
    for p_, lab in ((1, "DRAM"), (2, "Migrating"), (3, "NOR")):
        xs = [x for x, q in zip(t, ph) if q == p_]
        if not xs: continue
        if p_ > 1:
            ax.axvline(min(xs), color=C_GRID, ls=":", lw=1, zorder=1)
        mean = sum(y for y, q in zip(ns, ph) if q == p_) / len(xs)
        # One colour, one case, one size: these name regions of the axis, they
        # are not data and should not compete with the curve.
        ax.annotate(f"{lab}\n{mean:.0f} ns", ((min(xs) + max(xs)) / 2, top * 0.985),
                    ha="center", va="top", fontsize=10.5, color="#3d474e")
    ax.plot(t, ns, "-", color=C_NOR, lw=1.7, solid_joinstyle="round", zorder=3)

    # The overshoot is the finding: at the end of migration the medium is
    # already all flash, so the excess over the settled value is what active
    # promotion costs a concurrent reader.
    pk = max(range(len(ns)), key=lambda k: ns[k])
    settled = sum(y for y, q in zip(ns, ph) if q == 3) / max(1, sum(1 for q in ph if q == 3))
    ax.plot([t[pk]], [ns[pk]], "o", color=C_NOR, ms=6, zorder=4)
    ax.annotate(f"{ns[pk]:.0f} ns while still promoting\n"
                f"{ns[pk]-settled:+.0f} ns over settled",
                (t[pk], ns[pk]), textcoords="offset points", xytext=(-14, 10),
                ha="right", fontsize=9, color=C_NOR,
                arrowprops=dict(arrowstyle="-", color=C_NOR, lw=.9,
                                shrinkA=0, shrinkB=3))
    ax.set_ylim(0, top)
    ax.set_xlim(left=0)
    frame(ax, "Latency change over migration phases",
          "Time (sec)", "Average Latency per Cache line (ns)")
    fig.tight_layout(); fig.savefig(f"{OUT}/fig7-transition-timeline.png", dpi=160)
    made.append("fig7-transition-timeline")

# gated.csv is the worst case done properly: migration into an already-dirty
# pool, run to completion. worstcase.csv reached the same state the long way
# and its refill was truncated, so prefer gated when both exist.
r = rows("gated.csv") or rows("worstcase.csv")
if r:
    t = [float(x["elapsed_s"]) for x in r]
    ns = [float(x["ns_per_line"]) for x in r]
    ph = [int(x["phase"]) for x in r]
    # Trim the settled tail the same way fig7 does.
    p5i = [i for i, q in enumerate(ph) if q == 5]
    if p5i:
        cut = t[p5i[0]] + 60
        keep = [i for i, x in enumerate(t) if x <= cut]
        t = [t[i] for i in keep]; ns = [ns[i] for i in keep]; ph = [ph[i] for i in keep]

    fig, ax = plt.subplots(figsize=(10, 5.4))
    # gated.csv has three phases, worstcase.csv five. Name them by number so
    # the same block draws either.
    PH = (((1, "DRAM"), (2, "Migrating\n(erase-gated)"), (3, "NOR"))
          if max(ph) == 3 else
          ((1, "DRAM"), (2, "Migrating"), (3, "Evicted"),
           (4, "Migrating\n(erase-gated)"), (5, "NOR")))
    # A BROKEN AXIS. The gated fill runs 4,512 s against 60 s at either end,
    # so on one linear axis the two settled phases are 1.3% of the width each
    # and invisible -- the figure looks like it only measured the migration.
    # Three panels sharing the y scale, each with its own time range, gives
    # every phase room without distorting any of them.
    import matplotlib.gridspec as gridspec
    present = [(p_, lab) for p_, lab in PH if any(q == p_ for q in ph)]
    widths = []
    for p_, _ in present:
        xs = [x for x, q in zip(t, ph) if q == p_]
        widths.append(max(1.0, (max(xs) - min(xs)) ** 0.42))   # compress, do not flatten
    fig = plt.figure(figsize=(10.5, 5.2))
    gs = gridspec.GridSpec(1, len(present), width_ratios=widths, wspace=0.06)
    top = max(ns) * 1.16
    axes = []
    for k, (p_, lab) in enumerate(present):
        axp = fig.add_subplot(gs[k], sharey=axes[0] if axes else None)
        axes.append(axp)
        xs = [x for x, q in zip(t, ph) if q == p_]
        ys = [y for y, q in zip(ns, ph) if q == p_]
        axp.plot(xs, ys, "-", color=C_NOR, lw=1.5, solid_joinstyle="round", zorder=3)
        axp.set_xlim(min(xs), max(xs))
        axp.grid(alpha=.25, lw=.5)
        axp.annotate(f"{lab}\n{sum(ys)/len(ys):.0f} ns",
                   (0.5, 0.985), xycoords="axes fraction", ha="center", va="top",
                   fontsize=10, color="#3d474e")
        axp.set_xlabel(f"{min(xs):.0f}-{max(xs):.0f} s", fontsize=9)
        if k:
            axp.tick_params(labelleft=False)
            axp.spines["left"].set_visible(False)
        if k < len(present) - 1:
            axp.spines["right"].set_visible(False)
    axes[0].set_ylim(0, top)
    axes[0].set_ylabel("Average Latency per Cache line (ns)")
    fig.suptitle("Latency when migration must wait for erases",
                 fontsize=13, weight="semibold", y=0.97)
    fig.supxlabel("Time (sec), axis broken between phases", fontsize=10, y=0.02)
    fig.tight_layout(rect=[0, 0.04, 1, 0.95]); fig.savefig(f"{OUT}/fig8-worst-case.png", dpi=200)
    made.append("fig8-worst-case")

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
