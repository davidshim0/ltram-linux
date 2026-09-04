#!/usr/bin/env python3
"""The placement sweep: what happens as the working set outgrows the pool.

    ./plot_gapbs_sweep.py baselines/260903_gapbs_sweep -o docs/figures

Five Kronecker graphs against one 256 MB pool, from a working set that fits
inside it to one 8x larger. Condition A throughout: the graph is loaded from a
serialised .sg file, so the CSR is read-only from a known instant, and the
policy is attached only after a DRAM baseline has been measured on the same
process.

    fig12   slowdown against residency, measured against the naive model
    fig12b  the promotion ledger -- where every promoted page ended up
    fig12c  marginal cost of a flash page against residency

fig12c is the one that explains fig12. The naive model assumes a flash access
costs a fixed multiple of a DRAM access, so slowdown should be a straight line
in residency. It is not: the marginal cost per access RISES with residency,
because at low coverage flash reads interleave with fast DRAM misses and
memory-level parallelism hides most of the latency, while at high coverage
there is little fast traffic left to overlap with and one NOR chip serialises.
"""
import csv, os, sys, argparse
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

ap = argparse.ArgumentParser()
ap.add_argument("src")
ap.add_argument("-o", "--out", default="docs/figures")
a = ap.parse_args()

TAB_CB = ["#006BA4", "#FF800E", "#595959", "#C85200", "#5F9ED1", "#ABABAB"]
FAINT = "#7A838A"
GREY = "#3d474e"

# Effective PageRank iterations, MEASURED per graph as t(-i 20)/t(-i 1): the
# -i 20 cap is never reached, and kron23 is genuinely different from the rest.
ITERS = {20: 4.9, 21: 4.9, 22: 4.8, 23: 8.7, 24: 4.9}
# 128 B lines on ThunderX -- getconf LEVEL1_DCACHE_LINESIZE, DESIGN-DECISIONS
# section 0. Assuming 64 B halves every derived latency.
LINES_PER_PAGE = 32
# The memory-system ratio from sweep.csv: NOR access / DRAM access, near
# constant at 9.8x from 31 KiB to 64 MiB. NOT the 4.8x application-level ratio
# in fig2, and not a number derived from fig8's settled-read figure.
RATIO = 9.8
# Directly measured per-128 B-line penalty: 787 ns NOR - 124 ns DRAM (test=43).
ADDED_NS = 787 - 124

# Working-set size, for labelling. RSS = .sg file + two float arrays of N.
MB = {20: 138, 21: 277, 22: 558, 23: 1110, 24: 2200}


def load(d):
    out = {}
    for s in (20, 21, 22, 23, 24):
        p = os.path.join(d, f"s{s}", "A-pr-summary.csv")
        if not os.path.exists(p):
            continue
        with open(p) as f:
            out[s] = {r[0]: r[1] for r in csv.reader(f) if len(r) == 2}
    return out


def frame(ax, xl, yl, title, sub=None):
    ax.set_xlabel(xl, fontsize=10.5); ax.set_ylabel(yl, fontsize=10.5)
    ax.set_title(title, fontsize=13, weight="semibold", pad=26 if sub else 10)
    if sub:
        ax.text(0.5, 1.015, sub, transform=ax.transAxes, ha="center",
                va="bottom", fontsize=9.2, color=FAINT)
    ax.grid(alpha=.25, lw=.5); ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)


def note(fig, txt):
    """Footnote, and reserve room for it. A fixed rect works for two lines and
    silently runs the third one through the x-axis label."""
    fig.text(0.012, 0.012, txt, fontsize=7.8, color=FAINT, va="bottom",
             linespacing=1.5)
    fig.tight_layout(rect=(0, 0.03 * (txt.count("\n") + 1) + 0.02, 1, 1))


def save(fig, name):
    p = os.path.join(a.out, name)
    fig.savefig(p, dpi=200); plt.close(fig); print("  ->", p)


D = load(a.src)
os.makedirs(a.out, exist_ok=True)
S = sorted(D)
res = [float(D[s]["peak_ltram_pct"]) for s in S]
slow = [float(D[s]["slowdown_x"]) for s in S]

# ---- fig12: slowdown against residency, measured vs the naive model ---------
fig, ax = plt.subplots(figsize=(8.4, 5.2))
xs = [f / 100 for f in res]
model_x = [i / 100 for i in range(0, 101)]
ax.plot([x * 100 for x in model_x], [(1 - x) + x * RATIO for x in model_x],
        lw=1.6, ls="--", color=FAINT, zorder=2,
        label=f"if a flash access cost a flat {RATIO:.1f}× DRAM (sweep.csv)")
ax.plot(res, slow, marker="o", ms=7, lw=2, color=TAB_CB[0], zorder=3,
        label="measured")
for s, x, y in zip(S, res, slow):
    ax.annotate(f"scale {s}\n{MB[s]} MB", (x, y), textcoords="offset points",
                xytext=(0, 13), ha="center", fontsize=8.4, color=GREY,
                linespacing=1.25)
frame(ax, "Share of the process's pages resident on flash  (%)",
      "Execution time ÷ DRAM baseline  (×)",
      "Slowdown tracks coverage — but not linearly",
      "GAPBS PageRank · 256 MB pool · promotion ungoverned · DRAM baseline measured in-run")
ax.set_xlim(0, 100); ax.set_ylim(0, 17)
ax.legend(fontsize=9.5, frameon=False, loc="upper left")
note(fig, "The model is not a fit — it is what the measured 9.8x memory-system ratio would produce\n"
          "at a flat per-access cost. Measured CROSSES it: below at low coverage, above at high.\n"
          "fig12c is why.")
save(fig, "fig12-slowdown-vs-residency.png")

# ---- fig12b: the promotion ledger ------------------------------------------
# Every promoted page ends in exactly one of three states. The third is waste:
# an erase spent on a page that no longer exists. It is zero at every scale.
fig, ax = plt.subplots(figsize=(8.4, 5.2))
xs = range(len(S))
resid = [int(D[s]["resident_at_end"]) for s in S]
fb = [int(D[s]["faulted_back"]) for s in S]
fr = [int(D[s]["promoted_then_freed"]) for s in S]
ax.bar(xs, resid, .58, color=TAB_CB[0], label="still resident on flash", zorder=3)
ax.bar(xs, fb, .58, bottom=resid, color=TAB_CB[1], zorder=3,
       label="faulted back to DRAM (written)")
ax.bar(xs, fr, .58, bottom=[r + f for r, f in zip(resid, fb)],
       color="#C00000", zorder=3, label="promoted then freed — wasted erase")
for i, (r, f) in enumerate(zip(resid, fb)):
    if f > 500:
        ax.annotate(f"{f:,}", (i, r + f), textcoords="offset points",
                    xytext=(0, 5), ha="center", fontsize=8.6, color=TAB_CB[1])
ax.set_xticks(list(xs))
ax.set_xticklabels([f"scale {s}\n{MB[s]} MB\n{res[i]:.0f}% resident"
                    for i, s in enumerate(S)], fontsize=9)
frame(ax, "", "Pages promoted during the run",
      "Every promoted page, and where it ended up",
      "Wasted erases: 0 at every scale")
ax.legend(fontsize=9.5, frameon=False, loc="upper right")
note(fig, "Scale 20 is the control: with room for the whole working set the policy has no\n"
          "reason to be selective, promotes PageRank's rewritten score arrays, and 45% of its\n"
          "promotions come straight back. Capacity pressure makes the policy accidentally precise.")
save(fig, "fig12b-promotion-ledger.png")

# ---- fig12c: marginal cost per access against residency --------------------
fig, ax = plt.subplots(figsize=(8.4, 5.2))
ns = []
for s in S:
    add = float(D[s]["ltram_settled_median_s"]) - float(D[s]["dram_median_s"])
    per_page_trial = add / int(D[s]["resident_at_end"])
    ns.append(per_page_trial * 1e9 / (ITERS[s] * LINES_PER_PAGE))
ax.plot(res, ns, marker="o", ms=7, lw=2, color=TAB_CB[3], zorder=3,
        label="measured marginal cost")
ax.axhline(ADDED_NS, ls="--", lw=1.6, color=FAINT, zorder=2)
ax.annotate(f"{ADDED_NS} ns — unloaded penalty, measured directly\n"
            "(787 ns NOR − 124 ns DRAM, per 128 B line)",
            (2, ADDED_NS), textcoords="offset points", xytext=(0, -24), fontsize=8.6,
            color=FAINT, linespacing=1.4)
for s, x, y in zip(S, res, ns):
    ax.annotate(f"scale {s}", (x, y), textcoords="offset points",
                xytext=(0, 12), ha="center", fontsize=8.4, color=GREY)
frame(ax, "Share of the process's pages resident on flash  (%)",
      "Added cost per cache-line fetch  (ns)",
      "A flash page costs more when more of the working set is on flash",
      "added time ÷ (resident pages × measured iterations × 32 lines of 128 B per page)")
ax.set_xlim(0, 100); ax.set_ylim(0, 800)
ax.legend(fontsize=9.5, frameon=False, loc="lower right")
note(fig, "At 84% coverage the marginal cost reaches 92% of the unloaded penalty — almost nothing\n"
          "is hidden. At 11% coverage barely half of it lands, because flash reads interleave with\n"
          "fast DRAM misses and memory-level parallelism absorbs the rest.")
save(fig, "fig12c-marginal-cost.png")
