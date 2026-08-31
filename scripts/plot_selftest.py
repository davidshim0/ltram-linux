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

def overshoot(t, ns, ph, mig, ref, window=60.0):
    """Peak of the migrating phase, and how far migration sits above settled.

    Peak-minus-settled is the wrong estimator for the second question. The
    peak is the max of thousands of passes, so it carries the extreme value
    of the phase's own scatter: on the erase-gated run that is sd 25 over
    2,875 passes, and +54 "over settled" is what noise alone produces. Take
    the mean of the last `window` seconds instead, and report the scatter
    with it so a difference smaller than its own error is visible as one.
    """
    mi = [i for i, q in enumerate(ph) if q == mig]
    ri = [i for i, q in enumerate(ph) if q == ref]
    if not mi or not ri: return None
    pk = max(mi, key=lambda k: ns[k])
    settled = sum(ns[i] for i in ri) / len(ri)
    wi = [i for i in mi if t[i] > t[mi[-1]] - window] or mi
    m = sum(ns[i] for i in wi) / len(wi)
    var = sum((ns[i] - m) ** 2 for i in wi) / len(wi)
    return pk, m - settled, var ** .5


def mark_peak(ax, t, ns, ph, mig, ref, colour, dx=-14, dy=8):
    o = overshoot(t, ns, ph, mig, ref)
    if not o: return
    pk, ov, sd = o
    ax.plot([t[pk]], [ns[pk]], "o", color=colour, ms=6, zorder=4)
    ax.annotate(f"{ns[pk]:.0f} ns peak while still promoting\n"
                f"{ov:+.0f} \u00b1 {sd:.0f} ns over settled",
                (t[pk], ns[pk]), textcoords="offset points", xytext=(dx, dy),
                ha="right", fontsize=9, color=colour,
                arrowprops=dict(arrowstyle="-", color=colour, lw=.9,
                                shrinkA=0, shrinkB=3))


def figure_note(fig, lines, size=7.4):
    """Prose in the bottom margin, not over the plot.

    The other figures carry no prose on purpose -- what a figure shows belongs
    where it is presented. fig8 is the exception by request: the reason its
    overshoot is zero took a wrong estimator and a rate argument to establish,
    and that is worth having attached to the picture rather than in a commit
    message. Reserved as margin via tight_layout's rect, so the axes keep
    their proportions and the note cannot land on the data.
    """
    # Grow the canvas for the note rather than reserving margin out of it --
    # reserving squashed a 5.2 inch plot into a 2 inch strip.
    w, h0 = fig.get_size_inches()
    hn = len(lines) * size * 1.55 / 72 + 0.35     # inches the note needs
    fig.set_size_inches(w, h0 + hn, forward=True)
    frac = hn / (h0 + hn)
    fig.tight_layout(rect=[0, frac, 1, 1])
    fig.text(0.010, frac * 0.93, "\n".join(lines), fontsize=size,
             color="#4a5560", va="top", ha="left", linespacing=1.55)


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
    mark_peak(ax, t, ns, ph, 2, 3, C_NOR)
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
          ((1, "DRAM"), (2, "Migrating\n(erase-gated)"),
           (3, "NOR, engine on"), (4, "NOR, engine off"))
          if max(ph) == 4 else
          ((1, "DRAM"), (2, "Migrating"), (3, "Evicted"),
           (4, "Migrating\n(erase-gated)"), (5, "NOR")))
    fig, ax = plt.subplots(figsize=(10, 5.2))
    span = max(t) - min(t)
    seg = {p_: [x for x, q in zip(t, ph) if q == p_] for p_, _ in PH}
    seg = {k: v for k, v in seg.items() if v}
    wide = {k: (max(v) - min(v)) > 0.12 * span for k, v in seg.items()}
    # A 60 s phase at the end of a 4800 s run is a sliver: its label cannot
    # sit in it, and two adjacent slivers cannot both sit at their divider.
    # Hang each sliver off the nearer edge and stack it, then reserve enough
    # headroom above the trace that the stack never lands on the data.
    right = [k for k, v in seg.items() if not wide[k]
             and (min(v) + max(v)) / 2 > min(t) + span / 2]
    band = 0.105 * max(ns)
    top = max(ns) + band * (len(right) + 0.6) if right else max(ns) * 1.16
    nleft = 0
    for p_, lab in PH:
        xs = seg.get(p_)
        if not xs: continue
        if p_ > 1:
            ax.axvline(min(xs), color=C_GRID, ls=":", lw=1, zorder=1)
        mean = sum(y for y, q in zip(ns, ph) if q == p_) / len(xs)
        if wide[p_]:
            ax.annotate(f"{lab}\n{mean:.0f} ns", ((min(xs) + max(xs)) / 2, top * 0.985),
                        ha="center", va="top", fontsize=10, color="#3d474e")
        elif p_ in right:
            ax.annotate(f"{lab}  {mean:.0f} ns",
                        (0.995, top - band * (right.index(p_) + 0.15)),
                        xycoords=("axes fraction", "data"),
                        ha="right", va="top", fontsize=10, color="#3d474e")
        else:
            ax.annotate(f"{lab}  {mean:.0f} ns",
                        (0.005, top - band * (nleft + 0.15)),
                        xycoords=("axes fraction", "data"),
                        ha="left", va="top", fontsize=10, color="#3d474e")
            nleft += 1
    ax.plot(t, ns, "-", color=C_NOR, lw=1.5, solid_joinstyle="round", zorder=3)

    # The same mark fig7 carries, so the two read against each other.
    # Restricted to the migrating phase -- the global max here lands in the
    # settled-engine-on sliver, a different quantity -- and referenced to the
    # settled phase that still has the engine running, since referencing the
    # engine-off value would charge promotion for the erase interference too.
    mig = max((p_ for p_, lab in PH if "Migrating" in lab), default=None)
    if mig is not None and seg.get(mig) and seg.get(mig + 1):
        mark_peak(ax, t, ns, ph, mig, mig + 1, C_NOR)
    ax.set_ylim(0, top); ax.set_xlim(left=0)
    frame(ax, "Latency when migration must wait for erases",
          "Time (sec)", "Average Latency per Cache line (ns)")
    # Everything below is derived from the runs, so a re-measurement moves the
    # note with it. 49,152 pages is the 192 MiB working set both runs promote.
    WS = 49152
    def rate(rows_, mig_):
        tt = [float(x["elapsed_s"]) for x in rows_ if int(x["phase"]) == mig_]
        return WS / (max(tt) - min(tt)) if tt and max(tt) > min(tt) else 0.0

    o8 = overshoot(t, ns, ph, mig, mig + 1)
    r8 = rate(r, mig)
    t7 = rows("timeline.csv")
    note = ["Why the erase-gated overshoot is zero, and what is still open"]
    if t7 and o8:
        t7t = [float(x["elapsed_s"]) for x in t7]
        t7n = [float(x["ns_per_line"]) for x in t7]
        t7p = [int(x["phase"]) for x in t7]
        o7 = overshoot(t7t, t7n, t7p, 2, 3)
        r7 = rate(t7, 2)
        # fig7's settled value, for the duty-cycle comparison. Read it rather
        # than typing it, so a re-measurement cannot leave a stale constant.
        s7 = [y for y, q in zip(t7n, t7p) if q == 3]
        set7 = sum(s7) / len(s7)
        quiet = sum(y for y, q in zip(ns, ph) if q == mig + 2) / \
                max(1, sum(1 for q in ph if q == mig + 2)) if 4 in ph else None
        eng = sum(y for y, q in zip(ns, ph) if q == mig + 1) / \
              max(1, sum(1 for q in ph if q == mig + 1))
        note += [
          f"Promotion interference scales with promotion RATE, not with the cost of an erase.",
          f"  NOTE: both runs use wear_days=379 to finish in reasonable time. At the shipped",
          f"  5-year default the scanner ticks every 24 ms, not 5 ms, so the clean-pool rate is",
          f"  37/s, not {r7:.0f}/s. The gated rate is erase-supply-limited and barely moves, so at",
          f"  the real operating point the ratio is ~3.5x, not {r7/r8:.1f}x.",
          f"  clean pool (fig7)  {r7:5.1f} pages/s   {o7[1]:+.0f} +/- {o7[2]:.0f} ns over settled",
          f"  erase-gated        {r8:5.1f} pages/s   {o8[1]:+.0f} +/- {o8[2]:.0f} ns over settled",
          f"  {r7/r8:.1f}x fewer promotions, so linear scaling predicts {o7[1]/(r7/r8):+.0f} ns -- under the noise floor. It is not",
          f"  smaller than fig7's, it is below what this measurement can detect.",
          f"Mechanism: a program is 1.2 ms/page, so {r7:.1f}/s is {100*r7*0.0012:.1f}% duty against {100*o7[1]/set7:.1f}% observed",
          f"  slowdown. Kernel-side work (TLB shootdown, 4 KiB copy, L2 writeback) is ~0.7% at that",
          f"  rate -- an order of magnitude too small to be the term.",
        ]
        if quiet:
            note += [
          f"OPEN: an erase is 16.4 ms and the engine runs ~40/s = ~65% duty, yet erase interference is",
          f"  only {eng-quiet:+.0f} ns ({100*(eng-quiet)/quiet:.1f}%) -- ~10x too small to be blocking reads the way programs do.",
          f"  Either bank-level read-while-erase, or programs are correlated with the read stream (a",
          f"  promotion writes the page the reader is about to touch) while erases hit scattered free",
          f"  sectors. fig9, the per-read latency tail, separates these.",
        ]
        note += [
          f"ESTIMATOR: peak-minus-settled is NOT the overshoot. The peak is the max of {sum(1 for q in ph if q == mig):,} passes with",
          f"  sd {o8[2]:.0f}, so noise alone puts it ~3 sigma high -- that route gave a spurious +54. Use the",
          f"  last-60 s window mean with its scatter, as annotated.",
        ]
    figure_note(fig, note)
    fig.savefig(f"{OUT}/fig8-worst-case.png", dpi=200)
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


r = rows("qos.csv")
if r:
    # Per-READ latency, not per-pass. A pass average cannot answer "does a
    # read randomly stall": at 192 MiB a pass is 1.5M reads, so one 16.4 ms
    # erase moves the mean by 1% and the read that waited is gone.
    #
    # Drawn as percentiles on a LINEAR millisecond axis rather than a log-log
    # CCDF. The CCDF is the right object mathematically and unreadable in
    # practice -- five decades of y, and the number a reader actually wants
    # (what IS p99.99) has to be traced off an axis. Here every value is
    # printed. The bars below p99.9 are invisible slivers, and that is the
    # finding: nothing moves until the tail, then it explodes.
    def hist(cond):
        return sorted(((int(x["bucket_hi_ns"]), int(x["count"])) for x in r
                       if x["condition"] == cond and int(x["count"])), key=lambda t: t[0])

    def pct(cond, q):
        """Bucket upper edge below which q of reads fall -- "no slower than".
        Buckets are octaves, so this is coarse by construction, which is the
        right resolution for a tail spanning 1 us to 34 ms."""
        b = hist(cond)
        if not b: return 0
        n = sum(c for _, c in b); seen = 0
        for hi, c in b:
            seen += c
            if seen >= q * n: return hi
        return b[-1][0]

    def slower_than(cond, ns):
        # Count on the bucket's LOWER edge, so every read counted is
        # definitely at least ns. Counting on the upper edge would sweep in a
        # bucket spanning 4.2-8.4 ms and overstate by one.
        b = hist(cond); n = sum(c for _, c in b)
        return sum(c for hi, c in b if (hi + 1) // 2 >= ns), n

    def fmt(ns):
        if ns < 1000:            return f"{ns} ns"
        if ns < 1_000_000:       return f"{ns/1000:.1f} \u00b5s"
        return f"{ns/1e6:.1f} ms"

    # Colour is the MEDIUM, which is this project's rule everywhere else and
    # was broken here: the DRAM control was grey while "no background erasing"
    # -- which is NOR -- wore the DRAM blue. Both NOR conditions are now shades
    # of the NOR red, DRAM is the DRAM blue, and the difference between the two
    # NOR curves is what the engine is doing, not what the medium is.
    # All four flash conditions are the same medium, so intensity carries what
    # the device is doing rather than colour carrying which device it is:
    # lightest is quiet flash, darkest is writing and erasing together. DRAM
    # keeps the DRAM blue.
    LABEL = {"dram":            (C_DRAM,    "DRAM"),
             "dram_control":    (C_DRAM,    "DRAM control (no flash at all)"),
             "nor_read":        ("#D9A0A2", "NOR, reads"),
             "engine_off":      ("#D9A0A2", "NOR, no background erasing"),
             "nor_write":       ("#C9852F", "NOR, reads + writes"),
             "nor_erase":       ("#9E2F33", "NOR, reads + erases"),
             "erasing":         ("#9E2F33", "NOR, erasing every 28.8 ms"),
             "nor_write_erase": ("#5E1417", "NOR, reads + writes + erases"),
             "engine_spaced":   (C_NOR,     "NOR, erasing at 7.2/s (30 ms spacing)"),
             "engine_flat_out": ("#7A1F22", "NOR, erasing unspaced at 33.9/s")}
    order = ["dram", "dram_control", "nor_read", "engine_off",
             "nor_write", "nor_erase", "erasing", "nor_write_erase",
             "engine_spaced", "engine_normal", "engine_on", "engine_flat_out"]
    present = [c for c in order if any(x["condition"] == c for x in r)]
    present += sorted({x["condition"] for x in r} - set(order))
    # engine_flat_out is omitted from the figures on purpose. The watermarks
    # only drive the hysteresis latch; pacing comes from erase_poll_ms, and in
    # these runs clean never reached even the 8192 high-water, so the latch
    # never turned off in either condition -- flat out and module defaults were
    # operationally the same experiment and measured within 0.3% of each other.
    # The data stays in qos.csv; showing both curves implies a comparison that
    # was not actually being made.
    # engine_normal is EXCLUDED because it measured nothing: setting the
    # watermarks does not start the engine when the hysteresis latch is already
    # off and clean sits between the marks, so that phase recorded ~0 erases.
    # Plotting it would show a suspiciously clean "erasing" curve that was not
    # erasing. engine_flat_out is excluded as the unspaced worst case -- it is
    # real, but it is not an operating point the system runs in, because the
    # 30 ms reader spacing is only disabled by clearing target_pid, which is a
    # measurement artefact. Both stay in qos.csv.
    HIDE = {"engine_normal", "engine_flat_out", "engine_on", "nor_erase"}
    have = [(c,) + LABEL.get(c, (C_MODEL, c.replace("_", " "))) for c in present
            if hist(c) and c not in HIDE]
    if have:
        QS = [(0.50, "p50"), (0.99, "p99"), (0.999, "p99.9"), (0.9999, "p99.99"),
              (0.99999, "p99.999"), (0.999999, "p99.9999"), (1.0, "max")]
        # Same bars, log x. Linear was honest but spent 99% of its width on
        # the tail, so every percentile below p99.999 was a sliver against the
        # axis. Log gives 1 us and 34 ms room on the same picture; the table
        # carries the digits so nothing has to be traced off the axis.
        # Chart and table as SEPARATE images, so each can be used on its own.
        # A combined figure forces whoever is placing it to take both.
        fig, ax = plt.subplots(figsize=(11, 5.0))
        X0 = 1e-4                                        # ms, i.e. 100 ns
        top = max(pct(c, 1.0) for c, _, _ in have) / 1e6 * 3
        ax.set_xscale("log"); ax.set_xlim(X0, top)
        nb = len(have); h = 0.8 / nb
        for k, (cond, colour, label) in enumerate(have):
            vals = [pct(cond, q) / 1e6 for q, _ in QS]
            ys = [len(QS) - 1 - j2 + (nb - 1 - k) * h - (nb - 1) * h / 2
                  for j2 in range(len(QS))]
            ax.barh(ys, [v - X0 for v in vals], left=X0, height=h * 0.92,
                    color=colour, label=label, zorder=3)
        ax.axvline(20.0, color=C_GRID, lw=1, ls="--", zorder=2)
        ax.annotate("one erase, 20 ms", (20.0, len(QS) - 0.45),
                    xytext=(-5, 0), textcoords="offset points",
                    ha="right", va="top", fontsize=8.5, color="#5b6670", rotation=90)
        ax.set_yticks(range(len(QS)))
        ax.set_yticklabels([n for _, n in QS][::-1], fontsize=10)
        ax.set_xticks([1e-4, 1e-3, 1e-2, 1e-1, 1, 10])
        ax.set_xticklabels(["100 ns", "1 \u00b5s", "10 \u00b5s", "100 \u00b5s", "1 ms", "10 ms"])
        ax.set_ylim(-0.6, len(QS) - 0.4)
        frame(ax, "Read Latency Percentiles, per Access", "Read Latency", "")
        ax.grid(axis="y", visible=False)
        ax.legend(fontsize=9, frameon=False, loc="upper center",
                  labelspacing=0.5, handlelength=1.6, borderpad=0.7)
        fig.tight_layout()
        fig.savefig(f"{OUT}/fig9-latency-tail.png", dpi=200)
        made.append("fig9-latency-tail")

        # The same numbers as a standalone table, carrying its own legend so it
        # is self-describing away from the chart.
        cols = [n for _, n in QS]
        cells, rowlab, rowcol = [], [], []
        for cond, colour, label in have:
            cells.append([fmt(pct(cond, q)) for q, _ in QS])
            # Strip the "(1 in N reads)" frequency here. In the table the row
            # labels are the only place the incremental structure shows, and
            # left-aligned with the parenthetical gone they stack as
            # reads / reads + writes / reads + writes + erases -- so what each
            # row adds is legible down the column.
            rowlab.append(label.split("  (")[0].split(" (")[0])
            rowcol.append(colour)
        nrow = len(have)
        figt, axt = plt.subplots(figsize=(11.5, 0.42 * nrow + 0.8))
        axt.axis("off")
        tb = axt.table(cellText=cells, colLabels=cols, rowLabels=rowlab,
                       cellLoc="center", rowLoc="left", loc="center")
        tb.auto_set_font_size(False); tb.set_fontsize(9.5); tb.scale(1, 1.75)
        for (row, col), cell in tb.get_celld().items():
            cell.set_edgecolor("#D5D8DA"); cell.set_linewidth(.6)
            if row == 0:
                cell.set_text_props(weight="semibold", color="#3d474e")
                cell.set_facecolor("#F2F3F4")
            elif col == -1:
                cell.set_text_props(color=rowcol[row - 1], weight="semibold",
                                    ha="left")
                cell.set_facecolor("none"); cell.set_edgecolor("none")
                cell.PAD = 0.04
            else:
                cell.set_text_props(color=rowcol[row - 1])
        axt.set_title("Read Latency Distribution", fontsize=13,
                      weight="semibold", color="#22282c", pad=16)
        figt.savefig(f"{OUT}/fig9c-table.png", dpi=200, bbox_inches="tight")
        made.append("fig9c-table")

        # fig9b: the quantile function. x is percentile on a "number of nines"
        # scale (-log10(1-p)), y is latency, log. A CDF hides knees because it
        # spends all its width on the body; this spends width on the tail,
        # which is where the structure is. A knee here is a population --
        # a class of event with its own characteristic cost -- so the shape
        # says how many distinct things are going on, not just how bad it gets.
        fig2, ax2 = plt.subplots(figsize=(9.6, 5.6))
        NINES = [(0.5, "50%"), (0.99, "99%"), (0.999, "99.9%"), (0.9999, "99.99%"),
                 (0.99999, "99.999%"), (0.999999, "99.9999%"), (0.9999999, "99.99999%")]
        def nines(q): return -__import__("math").log10(1.0 - q)
        # The axis ends where the data does. With n reads the largest
        # observation IS the quantile at 1 - 1/n, so that is the real right
        # edge; truncating earlier and drawing a straight line out to the max
        # would invent a slope through percentiles that were never resolved.
        NS = {c: sum(k for _, k in hist(c)) for c, _, _ in have}
        XR = max(nines(1.0 - 1.0 / n) for n in NS.values())
        for cond, colour, label in have:
            n = NS[cond]
            qmax = 1.0 - 1.0 / n
            qs = [1.0 - 0.5 * 10 ** (-t / 40.0) for t in range(0, 401)]
            qs = [q for q in qs if q <= qmax]
            xs = [nines(q) for q in qs]
            ys = [pct(cond, q) for q in qs]
            # Past its own 1 - 1/n a condition has nothing left to say, and
            # the empirical quantile is flat at the maximum by definition.
            xs.append(XR); ys.append(pct(cond, 1.0))
            ax2.plot(xs, ys, "-", color=colour, lw=1.9, label=label)
        # The floor, drawn from the control rather than asserted. With
        # nohz_full=47 isolcpus=47 the scheduler tick is gone -- 60,012
        # arch_timer interrupts per 60 s became ~0 -- and the DRAM control now
        # records NOTHING above 100 us in 145,196,752 reads. So the shaded band
        # is not noise to be tolerated, it is the resolution limit: anything
        # above it is the device. The dashed line is one erase.
        ctl = [c for c, _, _ in have if c == "dram_control"]
        if ctl:
            fl = pct(ctl[0], 1.0)
            ax2.axhspan(0, fl, color=C_GRID, alpha=.10, lw=0, zorder=0)
            ax2.annotate(f"measurement floor: {fl/1000:.1f} \u00b5s\n"
                         f"(nothing above it in {sum(c for _, c in hist(ctl[0])):,} control reads)",
                         (0.015, fl * 2.6), xycoords=("axes fraction", "data"),
                         fontsize=8.5, color="#5b6670", va="bottom")
        ax2.axhline(20_000_000, color=C_GRID, lw=1, ls="--", zorder=1)
        ax2.axhline(80_000, color=C_GRID, lw=1, ls="--", zorder=1)
        ax2.annotate("one 256 B page program, 80 \u00b5s",
                     (0.012, 80_000), xycoords=("axes fraction", "data"),
                     xytext=(0, 5), textcoords="offset points", ha="left",
                     fontsize=8.5, color="#5b6670")
        # Right-aligned: the legend owns the top-left, and the curves reach
        # this line only at the far right, so the label sits over empty axis.
        # Mid-axis: the legend owns the top-left and the erase curve climbs
        # through the top-right, so the only clear span is the middle.
        ax2.annotate("one erase, 20 ms", (0.012, 20_000_000),
                     xycoords=("axes fraction", "data"), xytext=(0, 5),
                     textcoords="offset points", ha="left",
                     fontsize=8.5, color="#5b6670")
        ax2.set_yscale("log")
        # nines(1 - 10**-k) == k exactly for k >= 1, but nines(0.5) is 0.301,
        # not 0 -- placing the 50% tick at the origin puts it a third of a
        # decade left of the data.
        tk = [(nines(0.5), "50%")] + \
             [(float(k), "%s%%" % ("99.99999999"[:k + 2] if k > 1 else "99"))
              # ...but not one that collides with the 100% tick at the end.
              for k in range(1, int(XR) + 1) if XR - k >= 0.9]
        ax2.set_xticks([k for k, _ in tk] + [XR])
        ax2.set_xticklabels([lab for _, lab in tk] + ["100%\n(max)"])
        ax2.set_yticks([1e2, 1e3, 1e4, 1e5, 1e6, 1e7])
        ax2.set_yticklabels(["100 ns", "1 \u00b5s", "10 \u00b5s", "100 \u00b5s", "1 ms", "10 ms"])
        ax2.set_xlim(nines(0.5), XR)
        frame(ax2, "Read Latency Distribution", "Percentile", "Read Latency")
        legend_by_last(ax2, fontsize=9, frameon=False, loc="upper left",
                       bbox_to_anchor=(0.012, 20_000_000 / 2.2),
                       bbox_transform=ax2.get_yaxis_transform())
        # Provenance, because "how was this measured" should not require
        # reading a commit message. n is the pooled read count; repeats are
        # separate 60 s phases at different positions in the run, summed as
        # histograms rather than averaged as percentiles.
        # When every condition was measured the same way, say that once rather
        # than repeating it per curve and running off the edge.
        ns = sorted(sum(c for _, c in hist(c0)) for c0, _, _ in have)
        reps = sorted({"1 x 240 s" for _ in have})
        fig2.tight_layout(rect=[0, 0.075, 1, 1])
        fig2.text(0.008, 0.010,
                  f"all {len(have)} conditions measured in one run, {reps[0]} each, identical settings: "
                  f"same binary, same pinned CPU, target_pid held on a sleeper throughout.\n"
                  f"{ns[0]/1e6:.0f}\u2013{ns[-1]/1e6:.0f} M individually timed reads per condition. "
                  f"No pooling, no averaging.\n"
                  f"A fifth condition, erases without writes, was measured and is kept in the data; "
                  f"it is omitted here because the combined case shows the same erase step.",
                  fontsize=7.6, color="#7A838A", va="bottom", linespacing=1.5)
        fig2.subplots_adjust(bottom=0.185)
        fig2.savefig(f"{OUT}/fig9b-quantiles.png", dpi=200)
        made.append("fig9b-quantiles")

        print("\n  per-read latency")
        print(f"    {'':34}" + "".join(f"{n:>11}" for _, n in QS))
        for cond, _, label in have:
            print(f"    {label:34}" + "".join(f"{fmt(pct(cond, q)):>11}" for q, _ in QS))

print("plotted:", ", ".join(made) if made else "nothing found")
for m in made: print(f"  {OUT}/{m}.png")
