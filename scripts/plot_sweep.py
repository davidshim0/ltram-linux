#!/usr/bin/env python3
"""Plot the working-set sweep.

Reads the CSV sweep.sh emits and produces three panels:

  1. slowdown vs working set, NOR against DRAM, cold and warm
  2. absolute time per pass, with the compute floor drawn under it
  3. where the time goes: compute against memory, as a share of each pass

The third panel is the one that matters, because the claim it settles is the
one we asserted for weeks without measuring: that a medium roughly 10x slower
per access yields a ~5x end-to-end slowdown because only about half the time
was ever spent reaching memory.

  ./plot_sweep.py sweep.csv [-o out.png]
"""
import csv, sys, argparse
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

LLC = 16 * 1024 * 1024
NOR = 256 * 1024 * 1024

ap = argparse.ArgumentParser()
ap.add_argument("csv"); ap.add_argument("-o", "--out", default="sweep.png")
a = ap.parse_args()

d = {}
for r in csv.DictReader(open(a.csv)):
    try: m = float(r["mean_s"])
    except ValueError: continue
    d.setdefault(int(r["bytes"]), {})[r["mode"]] = (
        m, float(r["sd_s"] or 0), int(r["pages"]), int(r["resident_pages"] or 0))

sizes = sorted(d)
mib = [s / 2**20 for s in sizes]
def col(mode, i=0):
    return [d[s][mode][i] if mode in d[s] else float("nan") for s in sizes]

fig, ax = plt.subplots(3, 1, figsize=(9, 12), sharex=True)
fig.suptitle("LtRAM: cost of putting read-mostly data on NOR flash",
             fontsize=13, y=0.995)

# --- 1. slowdown -----------------------------------------------------------
for cold, warm, lab, c in [("nor_cold", "dram_cold", "forced cold, every access reaches the medium", "#0B6A6F"),
                           ("nor_warm", "dram_warm", "cache allowed, what an application sees", "#96631A")]:
    n, r = col(cold), col(warm)
    ax[0].plot(mib, [x / y if y else float("nan") for x, y in zip(n, r)],
               "o-", color=c, label=lab)
ax[0].axhline(1.0, color="#828E96", lw=0.8, ls=":")
ax[0].set_ylabel("NOR / DRAM, time per pass")
ax[0].legend(fontsize=8, loc="upper left")
ax[0].set_title("Slowdown. Flat above ~2x LLC is the asymptote; the fall past "
                "256 MiB is the device running out of room", fontsize=9)

# --- 2. absolute time ------------------------------------------------------
ax[1].plot(mib, col("dram_cold"), "o-", color="#1F5F7A", label="DRAM, cold")
ax[1].plot(mib, col("nor_cold"),  "o-", color="#9E2F33", label="NOR, cold")
ax[1].plot(mib, col("comp"),      "s--", color="#828E96", label="compute floor (row pinned to L1)")
ax[1].set_yscale("log"); ax[1].set_ylabel("seconds per pass")
ax[1].legend(fontsize=8, loc="upper left")
ax[1].set_title("Absolute cost. The grey line is the same arithmetic with the "
                "memory traffic removed", fontsize=9)

# --- 3. where the time goes ------------------------------------------------
# Cold and warm both decompose against the SAME compute floor. The pinned row
# is N*4 bytes, so a scrub costs it at most ~12 us of re-fetch against a ~30 ms
# floor, which is four hundredths of a percent and below the run-to-run noise.
for mode, lab, c, ls in [("dram_cold", "DRAM, forced cold", "#1F5F7A", "o-"),
                         ("nor_cold",  "NOR, forced cold",  "#9E2F33", "o-"),
                         ("dram_warm", "DRAM, cache allowed", "#1F5F7A", "s--"),
                         ("nor_warm",  "NOR, cache allowed",  "#9E2F33", "s--")]:
    t, comp = col(mode), col("comp")
    ax[2].plot(mib, [100 * (x - k) / x if x and x > k else float("nan")
                     for x, k in zip(t, comp)], ls, color=c, label=lab, alpha=0.9)
ax[2].axhline(50, color="#828E96", lw=0.8, ls=":")
ax[2].set_ylabel("% of pass spent on memory"); ax[2].set_ylim(0, 100)
ax[2].set_xlabel("weight matrix size (MiB)")
ax[2].legend(fontsize=7, loc="lower right", ncol=2)
ax[2].set_title("Measured, not inferred. If DRAM sits near 50%, a 10x medium "
                "gives ~5x end to end", fontsize=9)

for x in ax:
    x.set_xscale("log", base=2); x.grid(alpha=0.25, lw=0.5)
    x.axvline(LLC / 2**20, color="#0B6A6F", lw=0.8, ls="--", alpha=0.6)
    x.axvline(NOR / 2**20, color="#9E2F33", lw=0.8, ls="--", alpha=0.6)
ax[0].annotate("LLC 16 MiB", (LLC / 2**20, ax[0].get_ylim()[1]), fontsize=7,
               color="#0B6A6F", ha="right", va="top", rotation=90)
ax[0].annotate("NOR 256 MiB", (NOR / 2**20, ax[0].get_ylim()[1]), fontsize=7,
               color="#9E2F33", ha="right", va="top", rotation=90)

fig.tight_layout(rect=[0, 0, 1, 0.985])
fig.savefig(a.out, dpi=150)
print(f"wrote {a.out}")

# The table matters as much as the picture: these are the numbers for a slide.
hdr = (f"\n{'MiB':>8} {'comp s':>9} | {'DRAM s':>9} {'wDRAM s':>9} {'w%':>5} "
       f"| {'NOR s':>9} {'wNOR s':>9} {'w%':>5} | {'ratio':>5} {'wRatio':>6} {'res':>6}")
print(hdr); print("-" * len(hdr))
for s in sizes:
    e = d[s]
    for tag, dm, nm in (("cold", "dram_cold", "nor_cold"),
                        ("warm", "dram_warm", "nor_warm")):
        if not {dm, nm, "comp"} <= set(e): continue
        dc, nc, cp = e[dm][0], e[nm][0], e["comp"][0]
        md, mn = dc - cp, nc - cp          # seconds spent reaching the weights
        res = e[nm][3] / e[nm][2] * 100 if e[nm][2] else 0
        print(f"{s/2**20:8.3f} {cp:9.5f} | {dc:9.5f} {md:9.5f} {100*md/dc:4.0f}% "
              f"| {nc:9.5f} {mn:9.5f} {100*mn/nc:4.0f}% "
              f"| {nc/dc:5.2f} {(mn/md if md>0 else float('nan')):6.2f} {res:5.0f}%  {tag}")
