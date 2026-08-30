#!/usr/bin/env python3
"""Plot the working-set sweep as three separate figures.

  fig1-execution-time.png       where the time goes, linear y
  fig1b-execution-time-log.png  the same, log y, so the small sizes are legible
  fig2-slowdown.png             end-to-end ratio, NOR against DRAM
  fig3-memory-share.png         memory access as a share of the pass

  ./plot_sweep.py sweep.csv [-d docs/figures]
"""
import csv, argparse, math
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter

LLC = 16 * 1024**2
NOR = 256 * 1024**2
# The compute floor is now valid at EVERY size: matmul --compute-only wraps
# both operands into a fixed 2048-float prefix (16 KiB), L1D-resident at any N.
# It measures 112.4 ns/line flat from 32 MB to 1 GB. Before that fix it pinned
# a whole row, which exceeded L1D above N=4096 and inflated the floor by 22% at
# the large sizes -- so fig1's components and fig3's shares were bounds there
# rather than values, and both were shaded. No longer.
#
# The wrap costs one AND per iteration, which shows up as a constant ~1.5%
# ADDED to the floor at every size. That direction understates the memory
# terms slightly and uniformly, which is the harmless way round.

C_COMP, C_DRAM, C_NOR = "#6B7280", "#1F5F7A", "#9E2F33"

ap = argparse.ArgumentParser()
ap.add_argument("csv"); ap.add_argument("-d", "--dir", default="docs/figures")
a = ap.parse_args()

d = {}
for r in csv.DictReader(open(a.csv)):
    try: m = float(r["mean_s"])
    except ValueError: continue
    if not math.isfinite(m): continue
    d.setdefault(int(r["bytes"]), {})[r["mode"]] = (m, int(r["pages"]),
                                                    int(r["resident_pages"] or 0))
sizes = sorted(d)
def col(mode):
    return [d[s][mode][0] if mode in d[s] else float("nan") for s in sizes]

def human(b, _=None):
    """32 KB, 1 MB, 192 MB. The measured sizes are N*N*4 so they land near, not
    on, round numbers -- 201,299,536 bytes is 191.97 MB and means 192.

    Do NOT snap to a power of two. That worked while every size was one, and
    silently mislabelled 192 MB as 256 MB the moment a size in between was
    measured: round(log2(201299536)) is 28."""
    for u, n in (("GB", 2**30), ("MB", 2**20), ("KB", 2**10)):
        if b >= n:
            v = b / n
            if v >= 10 or abs(v - round(v)) < 0.05:
                return f"{round(v):g} {u}"
            return f"{v:.1f} {u}"
    return f"{b} B"

def frame(ax, title, ylab, note=None):
    ax.set_xscale("log", base=2)
    ax.set_xticks(sizes); ax.xaxis.set_major_formatter(FuncFormatter(human))
    ax.tick_params(axis="x", labelrotation=45, labelsize=8)
    ax.set_xlabel("weight matrix size"); ax.set_ylabel(ylab)
    ax.set_title(title, fontsize=13, weight="semibold", pad=12)
    ax.grid(alpha=0.25, lw=0.5)
    ax.axvline(LLC, color="#0B6A6F", lw=0.9, ls="--", alpha=0.7)
    ax.axvline(NOR, color=C_NOR, lw=0.9, ls="--", alpha=0.7)
    bot = ax.get_ylim()[0]
    ax.annotate("LLC, 16MB", (LLC, bot), fontsize=8, color="#0B6A6F",
                ha="right", va="bottom", rotation=90, xytext=(-3, 4),
                textcoords="offset points")
    ax.annotate("NOR, 256MB", (NOR, bot), fontsize=8, color=C_NOR,
                ha="right", va="bottom", rotation=90, xytext=(-3, 4),
                textcoords="offset points")
    # note is accepted and ignored: no prose under the axes.

# ---------------------------------------------------------------- 1. time ---
# The COMPONENTS, not the totals. Compute is the same work either way, so the
# two access-latency curves are the whole difference between the media, and
# reading either total off the chart is compute plus that medium's latency.
comp = col("comp")
wd = [t - c for t, c in zip(col("dram_cold"), comp)]
wn = [t - c for t, c in zip(col("nor_cold"), comp)]

fig, ax = plt.subplots(figsize=(9, 5.6))
ax.plot(sizes, wn,   "o-", color=C_NOR,  lw=2, label="NOR access latency")
ax.plot(sizes, wd,   "o-", color=C_DRAM, lw=2, label="DRAM access latency")
ax.plot(sizes, comp, "s--", color=C_COMP, lw=1.6, label="Compute")
ax.set_ylim(bottom=0)
frame(ax, "Execution time", "seconds per pass",
      "Components, not totals: a pass on either medium is Compute plus that medium's access "
      "latency.\nCompute is identical work in both cases. Linear axis, so everything below "
      "64 MB sits on the baseline. See fig1b for those.")
ax.legend(fontsize=9.5, loc="upper left", frameon=False)
fig.tight_layout(); fig.savefig(f"{a.dir}/fig1-execution-time.png", dpi=160)

# Same data, log y. Linear is honest about how steeply cost grows and hides the
# three orders of magnitude below 64 MB entirely, which is where the cache
# crossover happens. Both are worth having.
fig, ax = plt.subplots(figsize=(9, 5.6))
ax.plot(sizes, wn,   "o-", color=C_NOR,  lw=2, label="NOR access latency")
ax.plot(sizes, wd,   "o-", color=C_DRAM, lw=2, label="DRAM access latency")
ax.plot(sizes, comp, "s--", color=C_COMP, lw=1.6, label="Compute")
ax.set_yscale("log")
frame(ax, "Execution time (log scale)", "seconds per pass",
      "The same three curves. On a log axis the constant vertical gap between the two access "
      "latencies\nis the medium ratio, and it holds from 32 KB to 64 MB.")
ax.legend(fontsize=9.5, loc="upper left", frameon=False)
fig.tight_layout(); fig.savefig(f"{a.dir}/fig1b-execution-time-log.png", dpi=160)

# ------------------------------------------------------------ 2. slowdown ---
fig, ax = plt.subplots(figsize=(9, 5.6))
for cm, dm, lab, c, ls in [("nor_cold", "dram_cold",
                            "Cold cache: LLC flushed before every pass", C_NOR, "o-"),
                           ("nor_warm", "dram_warm",
                            "Warm cache: reuse across passes allowed", "#96631A", "s--")]:
    n, r = col(cm), col(dm)
    ax.plot(sizes, [x / y if y else float("nan") for x, y in zip(n, r)],
            ls, color=c, lw=2, label=lab)
ax.axhline(1.0, color="#868E8A", lw=0.9, ls=":")
ax.text(sizes[0], 1.03, "no penalty", fontsize=7.5, color="#868E8A")
ax.set_ylim(bottom=0)
frame(ax, "End-to-end performance ratio (NOR / DRAM)", "times slower than DRAM")
ax.legend(fontsize=9.5, loc="upper left", frameon=False)
fig.tight_layout(); fig.savefig(f"{a.dir}/fig2-slowdown.png", dpi=160)

# --------------------------------------------------------------- 3. share ---
fig, ax = plt.subplots(figsize=(9, 5.6))
# SHAPE is the medium, FILL is the cache state, and every line is solid.
#
# Cold and warm coincide above the LLC -- that is the point of the figure --
# and two filled markers of the same shape simply hide one another. A hollow
# marker over a filled one shows both are there, which a third shape or a
# dashed line cannot do.
for mode, lab, c, mk, filled in [("nor_cold",  "NOR, cold cache",  C_NOR,  "o", True),
                                 ("nor_warm",  "NOR, warm cache",  C_NOR,  "o", False),
                                 ("dram_cold", "DRAM, cold cache", C_DRAM, "s", True),
                                 ("dram_warm", "DRAM, warm cache", C_DRAM, "s", False)]:
    t = col(mode)
    ax.plot(sizes, [100 * (x - k) / x if x and x > k else float("nan")
                    for x, k in zip(t, comp)],
            marker=mk, ls="-", color=c, lw=1.6, ms=6.5,
            mfc=(c if filled else "none"), mec=c, mew=1.5,
            label=lab, alpha=0.95, zorder=(3 if filled else 4))
ax.set_ylim(0, 100)
frame(ax, "Memory access latency as a share of total run time", "% of the pass spent reaching the weights",
      "Measured as (total minus compute) over total, with the compute floor held L1-resident "
      "at every size.\nOn DRAM it is roughly half the pass, which is why a medium ~9x slower "
      "per access costs only ~4.6x end to end.")
# Lower right: the warm and cold curves have converged by then, so nothing
# else is down there. Centre left sat on top of the DRAM cold curve.
ax.legend(fontsize=9, loc="lower right", frameon=False)
fig.tight_layout(); fig.savefig(f"{a.dir}/fig3-memory-share.png", dpi=160)

print(f"wrote {a.dir}/fig1, fig1b, fig2, fig3")

hdr = (f"\n{'size':>8} {'comp s':>9} | {'DRAM s':>9} {'wDRAM s':>9} {'w%':>4} "
       f"| {'NOR s':>9} {'wNOR s':>9} {'w%':>4} | {'e2e':>5} {'wRatio':>6} {'res':>5}")
print(hdr); print("-" * len(hdr))
for s in sizes:
    e = d[s]
    for tag, dm, nm in (("cold", "dram_cold", "nor_cold"), ("warm", "dram_warm", "nor_warm")):
        if not {dm, nm, "comp"} <= set(e): continue
        dc, nc, cp = e[dm][0], e[nm][0], e["comp"][0]
        md, mn = dc - cp, nc - cp
        res = e[nm][2] / e[nm][1] * 100 if e[nm][1] else 0
        flag = ""
        print(f"{human(s):>8} {cp:9.5f} | {dc:9.5f} {md:9.5f} {100*md/dc:3.0f}% "
              f"| {nc:9.5f} {mn:9.5f} {100*mn/nc:3.0f}% "
              f"| {nc/dc:5.2f} {(mn/md if md > 0 else float('nan')):6.2f} {res:4.0f}% {tag}{flag}")

# Per cache line, cold. Nothing derived: both columns are a measured total
# divided by the line count, so this is the honest answer to "why does the
# ratio stop being flat above 64 MB".
print(f"\n{'size':>8} {'lines':>10} {'DRAM ns/line':>13} {'NOR ns/line':>12} {'ratio':>6} {'res':>5}")
for s in sizes:
    e = d[s]
    if not {"dram_cold", "nor_cold"} <= set(e): continue
    lines = s / 128.0
    dl, nl = e["dram_cold"][0] * 1e9 / lines, e["nor_cold"][0] * 1e9 / lines
    res = e["nor_cold"][2] / e["nor_cold"][1] * 100 if e["nor_cold"][1] else 0
    print(f"{human(s):>8} {lines:10.0f} {dl:13.1f} {nl:12.1f} {nl/dl:6.2f} {res:4.0f}%")
