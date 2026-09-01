# fig1 / fig1b — methodology and what the numbers say

Working notes for verification. Not paper prose. Everything below is traceable
to `scripts/sweep.sh`, `scripts/plot_sweep.py`, `workloads/matmul/matmul.c` and
`docs/figures/sweep.csv`.

---

## 1. What is plotted

fig1 and fig1b are the **same three curves**; fig1 is linear in y, fig1b is log.
They are **components, not totals**:

    Compute               = mean of mode `comp`
    DRAM access latency   = mean of `dram_cold` - mean of `comp`
    NOR access latency    = mean of `nor_cold`  - mean of `comp`

A total execution time for either medium is the compute curve plus that
medium's access-latency curve. The totals themselves are not drawn. The
subtraction is valid only if `comp` measures the same arithmetic with the
memory traffic removed — see the caveat in §6, which is the main thing to
check with your PI.

x is the weight-matrix size; y is seconds per pass.

`dram_warm` and `nor_warm` exist in the CSV but are **not** used by fig1/1b.
They are fig2 and fig3.

---

## 2. The workload

`workloads/matmul/matmul.c`, run as `x · W = y`:

    for i in 0..N-1:
        row = W + i*N
        for j in 0..N-1: acc += row[j] * x[j]
        y[i] += acc

* W is N x N `float` (4 B). x and y are vectors of N floats.
* This is **matrix-vector**, so **every element of W is read exactly once per
  pass**. There is no reuse of W to cache. x and y are small and stay resident.
* Consequence: within a pass, LLC size is irrelevant to W — every line of W is
  a compulsory miss. Across passes it matters enormously, which is what §4 is
  about.

**One pass** = `memset(y)`, then `cache_scrub()`, then `t0`, then the loop
above, then `t1`. The memset and the scrub are **outside** the timed region.

---

## 3. Sizes and pass counts

    N   = 90 128 181 256 362 512 724 1024 1448 2048 2896 4096 5793 8192 11585 16384
    |W| = N^2 * 4 bytes = 31 KiB ... 1 GiB

Alternate entries are exact powers of two; the ones between are those times
sqrt(2), so the x-axis is half-octave spaced.

Passes per size (`RUNS`), fewer at large sizes because one pass is already
seconds there:

    <= 4096 pages (16 MiB) : 300
    <= 32768 pages (128 MiB): 200
    otherwise               : 120

---

## 4. The five modes, and how each is produced

| mode | W lives in | LLC scrub per pass | used by fig1/1b |
|---|---|---|---|
| `comp` | L1D — compute floor | yes (32 MiB) | yes |
| `dram_cold` | DRAM | yes (32 MiB) | yes |
| `dram_warm` | DRAM | no | no |
| `nor_cold` | NOR | yes (32 MiB) | yes |
| `nor_warm` | NOR | no | no |

**Cache scrub (`--flush 32`).** A 32 MiB buffer — 2x the 16 MiB LLC — read one
word per 128 B line, called immediately before `t0` and excluded from the
timing. Capacity eviction rather than `dc civac` because it needs no privilege
and does not depend on which cache-maintenance instructions EL0 may issue.
This machine has **no private L2**: 16 MiB is the LLC, shared by all 48 cores.
Without the scrub, a W smaller than the LLC would still be resident from the
previous pass and passes 2..R would measure cache hits — NOR and DRAM would
come out identical. The scrub is the only thing preventing that at small sizes.

The scrub buffer also dirties one word per page of itself, so the placement
policy cannot mistake it for a promotion candidate. (On 2026-08-20 it did
exactly that: the scrub buffer is anonymous and read-mostly, `mmap` handed it
a lower address than the weights, and the scanner promoted all 16,384 of its
pages before reaching W — 457 s and 16,384 erases spent on the instrument.)

**Getting W onto NOR.** Not simulated; the real driver and the real policy.

1. `insmod nor_eci_fulltest_ltram.ko provide_ops=1 test=0 inline_erase=0 verify_erased=1`
2. `promote_batch = 512`
3. start matmul with `--print-ranges --phys --hold 5`
4. wait for its `RANGE` line, then write matmul's pid to
   `/sys/kernel/ltram/target_pid`
5. the kernel scanner migrates its read-mostly pages to LtRAM while the run
   proceeds
6. after the run, `target_pid = 0`

**Residency gate.** The log's `PHYS end weights ... LtRAM <n>` line is parsed.
If resident < pages/2 (and pages <= 65536), the row is **discarded, not
recorded**. This exists so a NOR run that never actually migrated cannot appear
as a ratio of 1.00 that silently means "the experiment did not happen".
Measured residency is in the CSV's `resident_pages` column and is essentially
complete up to the 256 MiB pool limit (65,526 / 65,536 pages at N=8192).

**Pool recycling between NOR points.** Promoted pages are dirty afterwards and
the pool is 65,536 sectors. If clean sectors drop below 40,000, the sweep sets
the erase watermarks to 65536/65535, waits for dirty to reach 0 (up to 2400 s),
then restores 8192/2048. Without this the sweep runs out of clean sectors around
the 128 MiB point and every later size quietly measures partial residency.

**Warm modes** (not in fig1) use `--iters` rather than more runs, sized so a run
lasts ~30 s, and the recorded time is divided by iters to stay per-pass. Both
warm modes use the same iters so the two sides stay comparable.

---

## 5. The reported statistic

`plateau()` in sweep.sh: **mean and sample standard deviation over the last 30%
of passes**, not the whole run.

Reason: the NOR runs migrate *during* the run. Averaging over all passes would
fold the migration ramp into the steady-state answer. The plateau is the part
after residency has settled. `samples` in the CSV is the number of passes that
went into the mean (so 90 of 300, 60 of 200, 36 of 120).

`--verify` computes a digest of `y` and it is recorded per row, so the same
arithmetic across modes can be confirmed rather than assumed.

---

## 6. Caveats — the things to actually check with your PI

**(a) The compute floor is valid at every size — but only since 2026-08-29,
and the comment in sweep.sh still describes the old broken version.**

`--compute-only` wraps BOTH operands into a fixed 8 KiB prefix:

    for j in 0..N-1:
        k = j & (COMP_FLOATS - 1)     /* COMP_FLOATS = 2048 floats */
        acc += W[k] * x[k]

2048 floats each is 16 KiB total, so the floor is L1D-resident at any N. Same
trip count, same FMA count, same instruction stream; the only thing removed is
the memory traffic, plus one AND per iteration whose cost is constant in N and
therefore cannot create a size-dependent artefact.

The earlier version pinned every row to W's row 0. That was not enough: a row
is N*4 bytes and x is another N*4, so above **N = 4096** the pair exceeds
32 KiB L1D and the "compute floor" started carrying exactly the traffic it
exists to exclude. Everything derived from it above 64 MB was a bound rather
than a value, and fig1/fig3 were shaded there to say so.

**This dataset is from after the fix.** `sweep.csv` was re-measured in
746f4424b, "figures: the compute floor is valid at every size now, shading
removed", the same day COMP_FLOATS was introduced (0e82af6de). So the
subtraction is sound at every point in the table, and the shading is gone
because the reason for it is gone.

Note for verification: `scripts/sweep.sh` line ~70 still comments "every row
pinned to row 0 ... EXACT only while one row fits in L1D (32 KiB, so N<=8192)".
That comment is stale on two counts — the mechanism changed, and the old
threshold was N=4096 rather than N=8192 because x has to fit alongside the row.
The code is right; the comment is not.

**(b) Above 256 MiB, W does not fit in NOR.** The pool is 65,536 pages
(256 MiB). At 511 MiB and 1 GiB, residency saturates at ~65,500 pages, so half
or three quarters of W is in DRAM. Those two points are **not** "NOR at 511 MiB";
they are a mixture. They should probably be dropped or marked.

**(c) Cold vs warm is a choice, and fig1 shows only cold.** Cold is the
pessimistic case for NOR. The warm case (fig2) shows the two media are
indistinguishable below the LLC, because W is cached and never re-read.

**(d) The access-latency curves are throughput-shaped, not latency-shaped.**
The matmul walks W sequentially and contiguously, so the hardware prefetcher
hides much of NOR's latency behind useful work. These curves therefore measure
what the workload *pays*, not the medium's latency. The medium's latency comes
from `--chase` (dependent-load pointer chase, Sattolo cycle), which is a
different experiment: 959 ns NOR vs 175 ns DRAM. **Do not quote fig1 as a
latency measurement.**

**(e) Single machine, single run of the sweep.** No repetition across boots.
`sd_s` in the CSV is the pass-to-pass spread within one run, not run-to-run.

---

## 7. What the numbers say

From `docs/figures/sweep.csv`, seconds per pass:

| W | compute | DRAM access | NOR access | NOR/DRAM access | NOR resident |
|---|---|---|---|---|---|
| 31 KiB | 0.00003 | 0.00002 | 0.00021 | 10.1 | 8/8 |
| 256 KiB | 0.00024 | 0.00017 | 0.00167 | 9.8 | 64/64 |
| 4 MiB | 0.00370 | 0.00280 | 0.02767 | 9.9 | 1024/1024 |
| 16 MiB | 0.01477 | 0.01136 | 0.11184 | 9.9 | 4096/4096 |
| 64 MiB | 0.05899 | 0.04955 | 0.45484 | 9.2 | 16384/16384 |
| 128 MiB | 0.11797 | 0.10815 | 0.91519 | 8.5 | 32773/32768 |
| 256 MiB | 0.23576 | 0.21674 | 1.83313 | 8.5 | 65526/65536 |
| 511 MiB | 0.47136 | 0.43399 | 2.04303 | 4.7 | 65270 — **W exceeds pool** |
| 1 GiB | 0.94281 | 0.87369 | 2.48635 | 2.9 | 65528 — **W exceeds pool** |

**1. Everything is linear in W.** Compute, DRAM access and NOR access all scale
with N^2 across four orders of magnitude. That is the expected shape for a
matrix-vector product with no reuse, and it is a sanity check that nothing
pathological is happening: doubling W doubles every component.

**2. NOR access latency is a near-constant ~9.8x DRAM's, from 31 KiB to
64 MiB.** It does not degrade with size. That is the central result of the
figure and it is stronger than a ratio at one point, because it holds across
2000x in working-set size. The two decades below the LLC are only visible on
fig1b's log axis.

**3. Compute is not negligible, and that is what makes the totals look better
than the components.** At 16 MiB, compute is 0.0148 s against 0.112 s of NOR
access — so the *total* NOR/DRAM ratio is (0.0148+0.112)/(0.0148+0.0114) =
4.8x, not 9.8x. fig2 plots that 4.8x. Both numbers are correct and they answer
different questions: 9.8x is the memory system, 4.8x is what the application
experiences. **They must not be conflated in the paper.**

**4. The ratio falling to 4.7x / 2.9x above 256 MiB is explained. The drop
from 9.8x to 8.5x between 64 and 256 MiB is not.**

Above 256 MiB it is caveat (b): W exceeds the 65,536-page pool, so half and
then three quarters of it is served from DRAM. Those points are a mixture and
the falling ratio is arithmetic, not physics.

The 64 -> 256 MiB drop is different and I cannot account for it. The compute
floor is **not** a candidate — it is L1D-resident at every size in this dataset
(caveat (a)) — which removes the explanation that would otherwise have been
reached for first. Residency is essentially complete at these sizes
(16384/16384, 32773/32768, 65526/65536), so partial migration is not it either.
What remains untested is pool pressure during migration: at 128 and 256 MiB the
run is filling most of the pool while being measured, and the plateau window
may still overlap erase activity. **Unexplained, and one experiment would
settle it: re-run 128 MiB with the pool pre-drained and a longer plateau.**
A reviewer looking at fig2 will see the same bend.

**5. The read-mostly premise is validated by the residency column, not by the
timings.** `resident_pages` shows the policy migrated essentially all of W with
no application hints — 65,526 of 65,536 pages at the pool limit. That is the
transparency claim, and it is measured here as a side effect.
