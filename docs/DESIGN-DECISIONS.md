# LtRAM policy engine — design decisions

Living document. Every decision here was reached deliberately; where a decision was
reversed, the reversal and its reason are kept so it does not get re-litigated.

- **Last updated:** 2026-08-14
- **Kernel:** Linux 6.8.0 (`~/ltram-policy-bench/linux`), arm64
- **Hardware:** Enzian, Cavium ThunderX (`MIDR 0x431f0a11`), host `zuestoll08`
- **Bitstream:** `169_phy200` (golden), flash clock 200.020 MHz

Legend: **[D]** decided · **[O]** open · **[R]** rejected, with reason

---

## 0. Measured constraints — the numbers everything else derives from

All measured on this machine, not assumed.

| quantity | value | source |
|---|---:|---|
| **NOR read, per 128 B line** | **787 ns** | `test=43`, 2026-08-15. Identical within 0.3% for sequential, random, dependent and independent access — see §0.3 |
| **DRAM read, per 128 B line** | **124 ns** | `test=43` dependent chase; a userspace control gave 137 ns |
| **NOR : DRAM** | **6.0x random, 7.8x sequential** | 787 ns against §0.0's 132 ns / 101 ns. NOR is pattern-insensitive (§0.3) and DRAM is not, so the ratio *depends on the access pattern* — the single 6.3x figure previously here assumed it did not |
| flash access alone (`r4`) | 98 cycles (min = max) | VIO, 33.3 M reads |
| write, beats landed (`st_wait=1`) | 918 µs | **not safe to read** |
| write, pages retired (`st_wait=2`) | **1330 µs** | safe to repoint a PTE |
| sector erase, typical / worst | 16.5 ms / 72.8 ms | harness |
| capacity | 256 MiB = 65,536 sectors × 4 KB | — |
| page ↔ sector | **1:1** (both 4 KB) | no translation layer needed |
| rated endurance | 100,000 erases/sector | datasheet |
| **hardware DBM (FEAT_HAFDBS)** | **ABSENT** — `HAFDBS=0` | probe module, 2026-08-14 |

### 0.0 The CPU side, measured 2026-08-18 — added because the NOR figures above had no comparable counterpart **[D]**

**The memory hierarchy, in three lines.** These are the numbers to quote; everything
below is the supporting detail.

```
L1D     32 KiB   per core            1.52 ns    3.0 cycles
L2      16 MiB   shared, all 48 cpus  22.4 ns   44.9 cycles     <- this IS the LLC
DRAM                                  132 ns              random dependent chase
                                      101 ns              sequential — locality is worth 1.31x
```

Measured with `workloads/tools/cache-probe`, one dependent load per 128 B line: no
prefetch to exploit, no memory-level parallelism to hide behind, so these are latencies
rather than throughputs. NOR's 787 ns sits directly against the 132 ns line.

| quantity | value | source |
|---|---:|---|
| core clock (RCLK) | **2000 MHz** | Cavium boot stub, `SKU: CN8890-2000BG2601` |
| cache line | 128 B | `getconf LEVEL1_DCACHE_LINESIZE`; matches the 128 B line used throughout |
| **L1 D-cache** | **32 KiB** per core | `matmul` sweep — floor holds to a 24.4 KiB working set, first miss at 32.3 KiB |
| **L2 (= LLC)** | **16,384 KiB**, shared by **all 48 cores** | boot stub `L2: 16384 KB`; `shared_cpu_list 0-47` |
| L2 hit cost over L1 | +0.39 ns/element (~0.8 cycles) | sweep — flat from 65 KiB to 8 MiB, a 128× range |
| **FMA dependency floor** | **3.566 ns/element = 7.13 cycles** | sweep floor at 24.4 KiB; `acc += row[j]*x[j]` contracts to one FMADD and the chain is serial on `acc` |
| **L1 D hit latency** | **1.52 ns = 3.0 cycles** | `cache-probe` dependent chase, flat below 32 KiB |
| **L2 hit latency** | **22.4 ns = 44.9 cycles** | `cache-probe --seq` plateau, 8 MiB down to 64 KiB |
| **effective L2 capacity** | **~8 MiB** for one stream (67% hits at the nominal 16 MiB) | `cache-probe --seq` |
| DRAM, dependent **random** chase | 132 ns per 128 B line | `cache-probe` top plateau |
| DRAM, dependent **sequential** chase | 101 ns per 128 B line | `cache-probe --seq`; row-buffer and TLB locality, worth 1.31x |
| **DRAM read, streaming** | **~99 ns per 128 B line** | 196 MiB matmul point minus the FMA floor |

#### There is no private L2 — that is why the L1→L2 step is so large **[D]**

ThunderX goes L1 straight to a **chip-wide shared cache**. What the boot stub and
`sysfs` call "L2" is architecturally an LLC: 16 MiB, `shared_cpu_list 0-47`, and
**44.9 cycles** away. On a machine with a private mid-level you would see two moderate
steps (ba8, x86: L1 0.74 ns → L2 2.57 ns → L3 9.5 ns); here there is one 14.7x step
from 1.52 ns to 22.4 ns and then nothing until DRAM. Two data cache levels, not three.

Consequence for placement: **there is no cheap tier between the core and a 16 MiB
structure shared with 47 other cores.** Any argument that reasons about "L2-resident
working sets" is reasoning about a resource the whole machine competes for.

#### Latency and throughput differ by the overlap factor, and both are recorded above **[D]**

The per-line latencies and the per-element figures are the same hardware seen two ways;
mixing them is how §0.3 went wrong the first time.

| level | latency (per 128 B line) | serialised per element | matmul, measured | overlap |
|---|---:|---:|---:|---:|
| L2 | 22.4 ns | 0.70 ns | 0.39 ns | 1.8x |
| DRAM | 101 ns (seq) | 3.16 ns | 3.10 ns | ~1.0x |

**`cache-probe --seq` (101 ns/line) and matmul's streaming cost (99 ns/line) agree to 2%**,
from completely independent instruments — one a dependent pointer chase, the other a
strided FMA loop. Together with `test=43`'s 124 ns chase and the probe's 132 ns random
chase, the DRAM figure is now confirmed four ways.

`sysfs` reports none of this: `/sys/devices/system/cpu/cpu0/cache/index*/size` does not
exist on this kernel and every `getconf` cache size returns 0, because arm64 takes cache
geometry from firmware and the device tree carries no `cache-size` property. The boot stub
and a working-set sweep are the only sources.

**Two figures previously in circulation were wrong**: L1 is 32 KiB, not 16 KiB, and L2 is
16 MiB, not 256 KiB. The sweep and the boot stub agree against both.

#### Why the L2 knee is a ramp and not a cliff **[D]**

A sequential sweep gives every line a reuse distance of exactly `|W|`, so under true LRU
the transition would be a **step**: ~100% hits below capacity, ~0% above (sequential
thrashing). Measured, it is a ramp — implied hit rate 93% at 12 MiB, 68% at 16 MiB, 45% at
20 MiB, 27% at 32 MiB, 8% at 196 MiB, tracking `C/W` rather than falling off a cliff. That
is the signature of **random or pseudo-random replacement**, which degrades gracefully.

Two consequences worth carrying:

- **Effective capacity is well under 16 MiB.** Hit rate at exactly 16 MiB is ~68%, not
  ~100%, because that L2 is shared with 47 other cores, the page tables and everything else
  running. Never size an experiment assuming the whole 16 MiB.
- **The knee is not small, it is distributed.** No single step exceeds +17.6%, but
  8 MiB → 196 MiB is **+68%** in total: 3.95 → 6.667 ns/element, a swing of 87 ns per
  128 B line — which is the DRAM latency, arriving spread over a decade of working-set
  size instead of at one boundary.

### 0.1 The wear budget — the binding constraint on the whole system **[D]**

```
total erase budget  = 65,536 sectors × 100,000 = 6.5536e9 erases
5 years (365 d)     = 1.5768e8 s
sustained rate      = 41.6 page-migrations / second
                    = one erase every 24.06 ms, device-wide
full-device rewrite = 65,536 / 41.6 = 26.3 minutes
```

Compare against the hardware's *throughput* limit of 1/1330 µs = 752 pages/s:

> **Wear is 18× more restrictive than write throughput.**

This is the single most important number in the design. Every earlier discussion treated
1330 µs as the cost of a migration; it is not. The binding cost is that each migration
spends 1/6.5-billionth of the device, and the device runs out long before the clock does.

**Corollary:** a *misclassified* page — promoted, then written, then evicted — costs the
budget twice for zero benefit. Classification accuracy is a wear property, not a
performance property.

### 0.2 Two budgets, stated separately **[D]**

Five years is a *product* lifetime and a policy choice, not a physical limit.

- **Production budget:** 41.6 migrations/s (5 years, 100% of device).
- **Experiment budget:** decide the fraction of device life the research campaign may
  spend. Burning 20% over a six-month campaign allows ~2× the production rate.

Both figures go in the paper. Do not silently use one where the other belongs.

---

### 0.3 RESOLVED 2026-08-15 — the honest number is 787 ns, flat across all access patterns **[D]**

Two rounds of correction, both now settled by measurement (`test=43`).

**Round 1 (2026-08-14), from adversarial review.** `lat_pass()` times a *sequential* sweep of
*independent* loads and divides by the line count, so 858 ns was a throughput figure, not a
latency. The concern: it "already contains every drop of MLP and prefetch," making any
"sequential is good because MLP hides latency" argument circular.

**Round 2 (2026-08-15), from hardware.** Structurally right, empirically void — **this CPU
provides neither MLP nor prefetch**, so there was nothing for the sequential pattern to
exploit. Four arms over the same 64 MB region:

```
SEQ    864 ns   independent, sequential   |  lat_pass, accumulates into `volatile u64 sink`
SCRAM  852 ns   independent, scrambled    |
DEP    786 ns   dependent,   random       |  chase-shape loop, no accumulator
IND    788 ns   independent, sequential   |
DRAM   124 ns   dependent,   random          <- positive control
```

Three findings:

1. **Dependency costs nothing.** DEP 786 vs IND 788 — noise. There is no memory-level
   parallelism to lose, so serialising the accesses changes nothing. Independently
   confirmed in userspace: a 1-chain and a 16-chain pointer chase are identical.
2. **No prefetching.** SEQ vs SCRAM is the same primitive with only the address order
   changed: 864 vs 852, with scrambled marginally *faster*.
3. **The 76 ns gap was instrumentation, not access pattern.** SEQ (864) vs IND (788) is the
   identical pattern in a different loop. `lat_pass()` accumulates into a `volatile u64`,
   costing a load-add-store per iteration that the compiler cannot register-allocate.

> **Every latency figure this project has taken through `lat_pass()` is inflated by ~9%.**

**Use 787 ns.** It holds for every access pattern within 0.3%, which makes the medium
unusually easy to model: one number, no pattern sensitivity. The harness's `1055 ns`
calibration annotation (`nor_eci_fulltest.c:5633`) is wrong — nothing reaches it.

## 1. Architecture — scanner and mover are separate **[D]**

**Three** components with **independent rate limits**, because they are constrained by
completely different resources:

| component | does | limited by | cost |
|---|---|---|---|
| **scanner** | `pte_mkclean()` a batch, later read `pte_dirty()` | **CPU / TLB** | PTE writes + TLB refill |
| **eraser** | DIRTY → ERASING → FREE, maintaining a watermark | **erase throughput** | 16.5 ms typ, 72.8 ms worst |
| **mover** | migrate a page DRAM → LtRAM (or back) | **flash endurance** | one erase per sector recycled |

**Never couple them.** An earlier draft had one interval doing scan and migration, which
forced a false trade-off: the interval that gives good classification is far slower than
the interval that fills the zone in experiment time. A later draft still folded erasing
into migration, which hides the constraint in §3.7.

**The wear event is the ERASE, not the write.** A promotion into an already-FREE sector
costs no erase at that moment — the erase was paid earlier, when that sector was recycled.
In steady state promotion rate = eviction rate = erase rate, so the three converge; during
initial fill they do not (§3.7).

### 1.1 Threading **[D, revisit]**

**Two kthreads.** The mover blocks ~1330 µs per page on DMA; at 1 page/25 ms that is a
5% duty cycle, but it must not delay the scanner. A single thread with two deadlines
would work only with async DMA completion, which is more work than a second thread.

Candidate queue between them: a small bounded ring. If it fills, the scanner drops
candidates rather than blocking — dropping is free and correct, since a still-clean page
will be offered again next sweep.

---

## 2. The scanner

### 2.1 K = 1 — no stability counter **[D]**

A page observed clean at a single check is a candidate. **No per-page state at all.**

The reasoning that settled this: K=3 at 333 ms and K=1 at 1 s have *identical detection
power* — both catch any write within a 1 s span. Splitting the window into three does not
catch anything the single window misses; it only requires a counter to remember where you
were. **The window length does the work, not the number of samples.**

So the counter buys nothing and costs an array, a hash table, or a `struct page` field.
Dropped.

### 2.2 The scan interval `T` sets classification quality — and nothing else **[D]**

`T` is exactly *the write inter-arrival time you can detect*. A page written less often
than once per `T` will look clean and be promoted.

Since scanning touches no flash, **`T` has no wear cost whatsoever.** It is a CPU
decision and a correctness decision, never a wear decision.

Short `T` is *not* free, but the cost is indirect: it promotes pages that are merely
briefly quiet, and each wrong promotion is charged to the wear budget (§0.1).

**Starting value: `T` = 1 s**, matching `numa_balancing_scan_period_min_ms`. Justify any
shorter value with measurement.

### 2.3 Batch size and coverage **[D]**

Each tick: check the batch armed last tick, then arm the next batch (cursor advances,
wraps). Scan batch should be ≥ `M / f` where `f` is the observed clean fraction, so a full
complement of candidates is usually found. Scanning 64 to promote 16 costs nothing.

Required rate is tiny — roughly **400–2,000 PTE re-arms/s**, i.e. 1.5–3% of what NUMA
balancing already sustains on this machine. This is not a performance problem at any
plausible setting.

### 2.4 Calibration reference **[D]**

`CONFIG_NUMA_BALANCING=y` on this kernel, so a structurally identical scanner is already
running. Confirmed live defaults (`/sys/kernel/debug/sched/numa_balancing/`, **not**
`/proc/sys` in 6.8):

```
scan_delay_ms 1000   scan_period_min_ms 1000   scan_period_max_ms 60000
scan_size_mb   256   hot_threshold_ms   1000
```

Read `task_numa_work()` in `kernel/sched/fair.c` before writing the scanner. Two
structural ideas from it are worth copying regardless of interval:

- **bounded work per invocation with a saved cursor** — never sweep the whole zone at once
- **per-region adaptive period** — back off on regions that keep coming back clean

Free measurement available on the running system: the delta of

```
grep -E "^numa_(pte_updates|hint_faults|hint_faults_local)" /proc/vmstat
```

over 60 s under load gives the real PTE-update and fault rates this silicon sustains, and
their **ratio** is an empirical estimate of `f`.

---

## 3. The eraser and the mover — rate and wear management

### 3.1 `M` promotions per tick; only `M/T` is wear-constrained **[D]**

Decoupling `M` from `T` is what makes both goals reachable:

| `T` | `M` | migrations/s | % production budget | time to fill 256 MiB |
|---:|---:|---:|---:|---:|
| 1 s | 16 | 16 | 38% | 68 min |
| 1 s | 41 | 41 | 99% | 27 min |
| 500 ms | 20 | 40 | 96% | 27 min |
| 1 s | **1** | 1 | 2.4% | **18.2 h** ← too slow for experiments |

That last row is why `M` exists. One migration per tick at a defensible `T` cannot fill
the zone inside a benchmark run, and a partially-filled zone reproduces finding C6's
failure — a clean-looking number that measured almost nothing.

### 3.2 Adaptive rate from *reachable* endurance **[D]**

The mover's interval is derived from remaining device life rather than fixed.

**The rate input is the head of the free list, not the global total.**

```
P         = number of RECYCLABLE sectors (FREE + DIRTY + ERASING; i.e. not pinned)
e_head    = erase count of the head of the free list (least-erased FREE sector)
spendable = P x (E_max - e_head)
rate      = spendable / L_remaining
```

Sanity check at full health: `P = 65,536`, `e_head = 0` gives 41.6 pages/s — exactly the
§0.1 baseline.

#### Why NOT the global budget

The obvious formula is `remaining = N x E_max - E_used`, one running counter, no free-list
inspection. **It is wrong, and it fails in the direction that destroys the device.**

Counter-example that settled it: read-only or very read-mostly data pins most sectors,
while a handful cycle continuously. Then `E_used` is *low* — most sectors have never been
erased — so the global formula reports a huge remaining budget and runs the mover at full
speed, while the few sectors in circulation are being worn out.

The defect: **the global formula treats endurance as fungible across sectors, and it is
not.** Only the endurance of *reachable* sectors is spendable. Endurance sitting in pinned
VALID sectors is stranded, and counting it inflates the budget by exactly the amount that
cannot be used.

`P` in the formula above is what excludes stranded endurance, and it is free — it is just
the queue lengths from the four-queue lifecycle.

#### The queue lengths diagnose *why* the free list is short

`e_head` alone cannot tell a backed-up eraser from genuine pinning, and the two have
opposite correct responses. The four-queue lifecycle answers it directly, and — unlike a
scalar `P` — it says which knob to turn:

| FREE | DIRTY | VALID | diagnosis | correct action |
|---|---|---|---|---|
| ≥ watermark | any | — | steady state | nothing |
| **empty** | **high** | — | **the eraser is behind** | erase faster, or throttle the mover. **Do not** slow on wear grounds — the endurance is queued, not stranded |
| **empty** | low | **high** | **device genuinely full** | success. Nothing to erase; see §3.8 |
| ≥ watermark | high | — | eraser catching up | nothing |

**The two are complementary, not alternatives:**

- `P` is the **rate input** — it goes into the formula above and continuously scales the
  mover. Counting DIRTY and ERASING inside it is what stops a backed-up erase queue from
  being mistaken for stranded endurance.
- The queue table is the **diagnostic** — it says which component is at fault when the free
  list runs dry, which a scalar cannot.

Keep both. `P` throttles; the table triages.

**Simpler variant**, if one variable is preferred: `rate_base x (E_max - e_head) / E_max`.
Monotonic, O(1), captures the essential behaviour — it just under-reacts when `P` is small.

#### The closed loop — why this design is self-consistent **[D]**

The rate signal and the corrective action of §3.4 share the same input, so the throttle and
the cure cannot disagree:

```
pinned state  --> e_head rises  --> mover rate drops          (protect the device)
                       |
                       +---------> skew trigger fires
                                   static levelling moves cold data out of
                                   fresh sectors; fresh sectors return to the
                                   free list; e_head falls --> rate rises again
```

One measured quantity drives both. The rejected global-budget version needed a separate
skew metric bolted alongside a budget that was already lying to it.

### 3.3 Free list ordered by ascending erase count **[D]**

Allocation always takes the **least-erased free sector**. This is dynamic wear levelling
and it is the baseline.

**Data structure:** not a sorted list — 65,536 entries re-sorted on every insert is far
too slow. Erase counts only ever increment by 1, so use **buckets indexed by erase
count**, each holding a list, plus a cached min pointer. Insert O(1), pop-min O(1)
amortised. Bucket count can be capped (e.g. 1024) with the tail sharing a bucket.

### 3.4 Static wear levelling on skew **[D]**

Dynamic levelling alone is insufficient: **cold data sits in low-erase sectors forever,
stranding their remaining endurance**, while hot data cycles through the rest and wears it
out. This is the standard failure mode of dynamic-only levelling.

**The correction:** read a *least-erased* sector's contents and rewrite them into a
*most-erased* free sector. The low-erase sector returns to the head of the free list; the
worn sector retires gracefully holding data that will not be rewritten.

Why the heuristic is self-consistent: a low erase count *means* that sector has rarely been
rewritten, so its occupant is by definition cold. Moving cold data to a nearly-dead sector
is exactly right — it will not be rewritten there either.

**Two things this must respect:**

1. **It costs erases and produces zero application benefit.** One move burns ~1–2 erases.
   It must be **rate-limited and charged against the same budget** as ordinary promotions.
   It is a net win only when the skew is large enough that the recovered stranded endurance
   exceeds what levelling spends — so **trigger on a skew threshold, not continuously**.
2. It must not be allowed to starve real promotions. Suggested split: static levelling may
   consume at most some fraction (start at 10%) of the erase budget per interval.

### 3.5 Fixed-rate fallback **[D]**

Ship a fixed `1 page / 25 ms` mode alongside the adaptive one, selectable at runtime. The
adaptive path has more ways to be wrong, and a fixed rate is the control case for any
experiment that measures the adaptive path.

### 3.6 Erase counts: DRAM-only, reset to 0 at every boot **[D]**

A production FTL persists per-block erase counts in a metadata area, and an SSD reports
them over SMART. **We do neither.** This is a single 256 MiB NOR chip with no spare area
reserved, and NOR does not self-report wear — there is genuinely nothing to read back at
power-on.

So the counter array lives in DRAM, is built by the driver at `insmod`, and **starts at
zero every boot, assuming a fresh device.**

Three consequences, to be stated in the paper rather than discovered by a reader:

1. **The mover always starts optimistic.** At boot `e_head = 0`, so §3.2 runs at full rate
   regardless of the device's true physical condition.
2. Wear levelling is correct *within* a session. Across reboots the tracked distribution
   drifts from the physical one, without any signal that it has.
3. **Wear figures are model-derived, not measured.** The policy's *relative* behaviour is
   still evaluated correctly, which is what the research question needs — but no claim
   about actual device lifetime is supported by this setup.

**Planned mitigation — chosen mechanism, implementation deferred [D]:** persist the array
to the *host* filesystem on module unload and reload it at `insmod`. Not on the flash, but
real persistence for ~256 KB, and it makes a multi-day experiment coherent.

This is **the** answer to the persistence question, not one option among several — the
alternatives (reserved flash sectors with their own wear problem; reconstruction at boot
from sector metadata that does not exist) are both worse for a single-chip prototype.
Build it when a multi-session experiment first needs it; until then, §3.6's three
consequences stand and go in the paper.

Note the failure mode when it is built: an unclean shutdown loses the session's history
silently. If that matters, checkpoint periodically rather than only at unload.

### 3.7 Erase throughput may become the binding constraint before wear does **[D — watch this]**

```
wear budget       41.6 migrations/s  ->  41.6 erases/s in steady state
erase, typical    16.5 ms            ->  60.6 erases/s  ->  69% duty cycle
erase, worst      72.8 ms            ->  13.7 erases/s  ->  CANNOT KEEP UP
```

At the full wear-budgeted rate the erase engine is already ~69% utilised. **Erase time
grows with wear**, so an aged device drifts toward the worst case, where the eraser
sustains only 13.7/s — a third of what the wear budget permits.

**The binding constraint therefore migrates over device life: wear-bound when fresh,
erase-throughput-bound when worn.** The free-list watermark is the only buffer between that
and a stalled mover.

**Rule: the mover NEVER erases inline.** It pops a pre-erased FREE sector or it stalls
(dropping the candidate, which is free — a still-clean page is offered again next sweep).
An inline erase is 16.5–72.8 ms, three orders of magnitude above the 1330 µs write, and
indefensible on a migration path, let alone a fault path.

**Watermark sizing — CORRECTED 2026-08-14.** An earlier draft claimed a 12,288-sector
watermark "reserves 19% of the device" as capacity that cannot hold data. **That is wrong.**
The watermark is replenished from the **DIRTY** queue, never by evicting VALID sectors. When
FREE drops below it, the eraser pulls a DIRTY sector and erases it; live data is untouched.

So the watermark costs **no capacity at all**:

- During initial fill there is nothing DIRTY to recycle, so FREE simply drains and VALID
  grows toward 65,536. The watermark is unmet, and that is correct.
- In steady state the erases happen either way; a larger watermark only schedules them
  *earlier*. Erased sectors sitting idle cost nothing.

**The real cost of a large watermark is boot latency**, not capacity: B-3's pre-erase to
12,288 takes 12,288 x 16.5 ms = **3.4 minutes** before promotion opens. That is the number
to trade off, and it is a startup cost paid once.

**Initialization is exempt from the wear budget.** Erasing the whole device once is 65,536
erases = 0.001% of the 6.55e9 budget. Run the initial fill flat out; only steady-state
recycling is wear-constrained.

**Two questions for the FPGA side:**

1. Can the controller run erases concurrently (multi-bank / multi-plane)? If not, 60.6/s
   typical is a hard ceiling.
2. **Does an in-progress erase block reads?** If it does, background erasing directly
   damages the read latency being measured — which would make B-4's erase-free measurement
   window a *requirement*, not a convenience.

### 3.8 Erase blocks reads — ANSWERED, and it is the project's hardest constraint **[D]**

Open question O5 is answered from evidence already in this project, in the worst direction:

- `WriteManager/scheduler.cpp:15` — **erases are enqueued on `nor_read_q`**, which is
  `depth=2`, at strict priority over writes. A 16.5 ms erase is head-of-line blocking for
  every read behind it.
- `169_phy200/hdl/nor_controller.v` — no erase-suspend, no bank awareness. One chip, one
  operation at a time.
- Measured: 6653/6653 erase probes returned `FF` — **a read during an erase always stalls,
  never answers early.**
- `test=37` at `wr_pct=50`: **93.8% of reads stalled**, median 758 ns/word vs ~47 ns/word
  warm.

**The arithmetic that makes this the central problem.** At §0.1's production rate:

```
41.6 erases/s x 16.5 ms = 686 ms per second = 68.6% of wall-clock time erasing
effective read bandwidth ~ 0.31 x 244 MB/s ~ 76 MB/s
```

**Under its own wear budget, the device is unavailable roughly two-thirds of the time.**
Any placement policy that puts frequently-read data on flash collides with this directly,
and it is an independent reason P2 (§5.4) fails.

#### Three ways out, in increasing order of cost

**(a) Skip erases entirely — an emulation mode [D, for measurement only]**

Once correctness has been demonstrated separately, run experiments with erase disabled.
Reads then never stall, and the timing profile is that of a **write-in-place NVM** —
MRAM, RRAM, PCM — on this interconnect. This is a legitimate methodology: the literature
routinely emulates NVM timing on other media.

Be precise about what it models: it removes the *erase* property, **not** the *fast-write*
property. Program time stays at 1330 µs/page, which a real MRAM would beat by orders of
magnitude. So it answers "what if this medium had no erase," not "what if this were MRAM."

Two constraints, non-negotiable:

- NOR programming can only clear bits (1 → 0). Without erase, repeated writes to a sector
  **monotonically degrade toward all-zeros**. Data is wrong in a specific, predictable way.
- Therefore: **run no correctness check in this mode**, and ensure the workload never
  depends on reading back what it wrote. Label every resulting figure as emulation.

The wear budget also vanishes in this mode, which removes §0.1 as a constraint — worth
noting so the two configurations' numbers are never compared directly.

**(b) Two chips, mirrored [O — hardware change]**

Erase chip A while serving reads from chip B; switch when A is programmed. Solves
read-blocking completely and simply. Cost: **50% density** — two chips of capacity for one
chip of storage.

**(c) N+1 chips with parity and an XOR engine [O — hardware change, best]**

Stripe across N chips plus parity. While one chip erases, reconstruct its data by reading
the other N and XOR-ing. Advantages over (b):

- **Density**: `N/(N+1)` instead of 50%. Needs **N ≥ 3 total chips** to beat mirroring —
  with exactly two chips, RAID 4/5 degenerates *into* a mirror, so the density argument
  requires designing the FMC card for 4 or 8 devices.
- **Write throughput scales with N** — independent chips program in parallel. This
  directly relieves §3.7's erase-throughput ceiling as well, since N chips can erase
  concurrently.
- **The XOR is free.** It is an FPGA; the cost is not compute but the extra reads —
  serving one line from the erasing chip costs N reads. Since only one chip erases at a
  time and reads to the others are unaffected, average impact is far below (b)'s blocking.

This is the strongest architectural result available from the project: *erase-blocking is
the fundamental limit of single-chip NOR-as-memory, and a parity-organised array removes
it while improving both density and write throughput over mirroring.* Scope it honestly —
it needs a new FMC board.

### 3.10 UBI is the closest in-tree precedent — evaluate before building **[O]**

`drivers/mtd/ubi/wl.c` solves a large part of §3.2-3.4 already, in-tree, field-tested.

**UBI** = *Unsorted Block Images*. A volume-management layer for raw flash sitting between
MTD (the raw device) and a filesystem such as UBIFS. It owns logical-to-physical erase-block
mapping, bad-block handling, scrubbing, and wear levelling. **`wl.c` is the wear-levelling
implementation** — `wl` does stand for wear levelling.

What it already has that we specified independently:

| our design | UBI's version |
|---|---|
| per-sector erase counters | `e->ec` on every `struct ubi_wl_entry` |
| free list ordered by erase count (§3.3) | RB-trees keyed on `ec` — `wl.c:150,257` |
| background erase thread (§1) | `ubi_thread` draining a work queue |
| static levelling on skew (§3.4) | `wl.c:334-371`, triggered when `last->ec - first->ec >= WL_FREE_MAX_DIFF` |
| skew threshold (open question O2) | `WL_FREE_MAX_DIFF`, a constant with field history |

**Status: consider, not adopt.** Read it before writing the eraser — even if none of it is
reused, `WL_FREE_MAX_DIFF` is a better-founded answer to O2 than the 10%-of-budget guess
currently recorded there. Differences that may rule it out: UBI assumes MTD semantics and
its own on-flash metadata (which §3.6 explicitly does not have), and it is a volume manager
rather than a memory tier, so its mapping layer is redundant here — our page/sector mapping
is 1:1.

### 3.9 Replacement policy when the device is full **[O]**

`FREE = 0, DIRTY = 0, VALID = 65,536` is the success state — the flash is fully utilised
holding read-mostly data. But it raises an unanswered question: if the scanner then finds a
better candidate, is anything evicted?

| option | note |
|---|---|
| **no replacement** (first-come, first-served) | Simplest, matches K=1's no-state philosophy. **But promotion order then determines final contents** — a real bias that must be reported |
| LRU-ish on read frequency | Needs per-page access tracking on LtRAM pages; the access flag is also software-managed here (`HAFDBS=0`) |
| natural churn only | Replace only when a write fault evicts something |

Defensible first implementation: **no replacement**, with the ordering bias stated in the
paper.

---

## 4. Page states and PTE rules

### 4.1 No hardware DBM — every write to a protected page faults **[D]**

`HAFDBS = 0` on this silicon (measured). `TCR_EL1.HD = 0`. The hardware **ignores PTE bit
51 entirely**; bit 51 is a pure software annotation that only Linux reads.

This is **simpler and safer**, not a limitation:

| page kind | bit 51 (`PTE_WRITE`/DBM) | bit 7 (`PTE_RDONLY`) | write fault lands in |
|---|:--:|:--:|---|
| LtRAM page | **0** | 1 | `do_wp_page()` → **migrate back to DRAM** |
| watched DRAM page | 1 | 1 | `pte_mkdirty()` → **dirty tracking** |

Both branches are one line apart in `handle_pte_fault()` (`mm/memory.c:5244`). Software
dirty tracking is automatic and already implemented — no code required for it.

**Why the absence is a safety win:** on DBM-capable hardware, a page that accidentally had
bit 51 set would be *silently* granted write permission and the CPU would store into flash
— no fault, nothing any software guard could catch, data gone. Here that is physically
impossible.

### 4.2 PTE rules — non-negotiable **[D]**

1. **Never set `PTE_WRITE` on an LtRAM PTE.** Not because of this machine — so the code
   stays correct on hardware that does have DBM.
2. **Always use `pte_wrprotect()`. Never hand-manipulate the bits.** A half-finished
   write-protect lands off-diagonal in the state matrix and fails *silently* in one of two
   ways: clear DBM without setting RDONLY → page is immediately writable while
   `pte_write()` reports protected; set RDONLY without clearing DBM → the hardware
   re-permits on the next store with no fault.
3. **`pte_wrprotect()` order matters:** transcribe hardware-dirty into `PTE_DIRTY` (bit 55)
   *first*, then clear bit 51, then set bit 7. Clearing bit 51 first destroys the record.
4. **Never load/modify/store a live PTE.** Use the `ptep_*` helpers — they use `cmpxchg`
   because a second writer exists.

### 4.3 `st_wait = 2` before repointing a PTE **[D]**

The PTE must not be repointed at a freshly written page until all 16 page-programs have
retired — **1330 µs**, not the 918 µs "beats landed" mark. Measured: repointing at
`st_wait=1` produced 1,632 transient bad words with a perfectly clean final sweep. The
writes were fine; reading too early was not.

### 4.4 Read-back verification is debug-only **[D]**

Verifying a 4 KB write means forcing 32 lines out of every cache level with the
`CVMCACHE` op and re-reading at 858 ns each. The eviction is the expensive part, and it
perturbs exactly the cache behaviour the benchmark measures.

Behind a `debugfs` knob, **default off, never on during an experiment**. Safety comes from
`st_wait=2` (§4.3), not from verifying afterwards.

---

## 5. Placement — what may live on flash

### 5.1 Private anonymous, base-page only, placed by migration **[D]**

Two independent lines of reasoning converge on this, which is why it is load-bearing:

- **Correctness (finding C3):** it closes most of the ~14 sites that can set a writable
  PTE or PMD, and all of the routes that bypass page tables entirely
  (`write(2)`, `read_folio`, direct-map stores). What remains is a short auditable list.
- **Attributability (finding C6):** the page cache is machine-wide and indexed by
  (file, offset), so the first process to fault a shared page decides its placement for
  everyone. Per-process opt-in cannot give per-process placement for file-backed memory.

### 5.2 Allocation-time routing is REJECTED **[R]**

`__get_fault_gfp_mask()`'s LtRAM block must be **deleted or gated off**:

- A routed page-cache folio **cannot be filled**: `filemap_read_folio()` writes into it
  with CPU stores that the ECI window silently discards, then marks it up-to-date holding
  raw NOR contents. **Live silent corruption.**
- It has no per-process scoping — every process on the machine.
- It reaches almost nothing that matters: `vmf->gfp_mask` has three consumers, readahead
  bypasses the only live one, and the anonymous path never reads it.
- "Read-mostly" is a property of *history*; an unfaulted page has none. Allocation time is
  structurally the wrong moment to decide.

`ltram_copy_to_flash()` at `mm/migrate.c:674` is the **only correct write path** in the
kernel. Migration is not one option among several — it is the only one wired for writes.

### 5.3 Scoping via `prctl` **[D]**

```c
#define MMF_LTRAM_ENABLE       31          /* bit 30 is MMF_VM_MERGE_ANY — taken */
#define MMF_LTRAM_ENABLE_MASK  (1UL << MMF_LTRAM_ENABLE)   /* 1UL, not 1 */
```

**Must be added to `MMF_INIT_MASK`** or it is cleared on *both* fork and execve —
`mm_init()` is the common constructor for `dup_mm()` and `mm_alloc()`, and it filters
through `mmf_init_flags()`. Without that one-line addition the wrapper-then-exec design is
a silent no-op.

If bit 31 is used, write the mask as `1UL << 31`: every existing `MMF_*_MASK` uses a plain
`int`, and `1 << 31` is signed overflow that sign-extends into bits 31–63.

---

### 5.4 Which pages are the *best* residents — reviewed 2026-08-14 **[D]**

Three candidate policies were proposed and adversarially reviewed. Verdicts:

#### The bandwidth arithmetic that settles it

The device is a **~244–256 MB/s pipe fronting 256 MiB of capacity**. Therefore:

```
per-page bandwidth share = 256 MB/s / 65,536 pages = 3,906 B/s
                         = 0.95 page-reads per second, per page
```

**The average resident can be read about once per second before the device saturates.**
That is a definition of cold. Inverted: if "hot" means 100 page-reads/s, the device can
host `256e6 / (4096 x 100)` = **625 pages = 2.56 MB — 0.95% of its capacity.**

And 2.56 MB fits entirely inside the ~16 MB LLC, so a resident set small enough to be
legitimately hot is small enough that the medium underneath is never touched.

Corollary: the per-LLC-miss penalty (858 − 102 = 756 ns) reaches 1% of a core at ~413
page-reads/s, but the bandwidth share caps the average at 0.95/s. **Bandwidth binds ~435x
before CPU time does.** Any placement argument conducted in latency units is answering the
non-binding constraint.

#### P1 — "no replacement is fine, the resident data is already ideal" — **decision kept, justification REFUTED**

Keep **no replacement** as the first implementation (§3.8) — it is simple and defensible.
Do **not** justify it by claiming the residents are optimal:

- §2.2: at `K=1`, residency certifies *one `T`-second observation*, not a property. A page
  written less often than once per `T` is promoted.
- §0.1's own corollary budgets for misclassification, which cannot coexist with "ideal by
  construction."
- Two write-clean pages can differ by orders of magnitude in LLC-miss rate. "No way to
  improve on data that is not written" is true only if read cost is zero.
- No-replacement maximally pins sectors — the exact pathology §3.2's counter-example
  identifies as destroying the device.

#### P2 — "hot read-only data beats cold" — **REFUTED**

Refuted by the arithmetic above, not by appeal to convention. Additional failures:

- **Conflates access rate with LLC-miss rate.** The cost is paid per miss. Under that
  correction P2 is either false, or a restatement of the standard cold-page policy in the
  wrong units.
- **Its load-bearing premise — "cold data will be swapped out anyway" — is not
  instantiated.** There is no swap device in `scripts/run-vm.sh`,
  `CONFIG_ZSWAP_DEFAULT_ON` is unset, and **the word "swap" appeared zero times in this
  document** before this section. With no tier below, NOR competes with DRAM directly and
  the optimum reverts to coldest-write-clean.
- **NOR's position is backwards from the claim.** vs NVMe: latency 858 ns vs ~80 µs
  (~100x better), bandwidth 256 MB/s vs 3–7 GB/s (**worse**). NOR's differentiated value is
  *small unpredictable random reads that must stay addressable* — not hot data, and not
  large sequential data.
- **Unimplementable as designed.** The scanner has no read-hotness signal — only
  `pte_mkclean()`/`pte_dirty()`. With `HAFDBS=0` there is no hardware access flag, so read
  tracking costs a fault *per read*, landing hardest on exactly the pages P2 targets. The
  design is cheap **because** it never looks at reads.

**Two objectives must be separated and labelled**, since they give different optimal
residents: *free DRAM capacity* (coldest write-clean) versus *demonstrate NOR is usable as
memory* (residents that are actually accessed). Choosing residents because they make the
graph move, while presenting it as a capacity policy, is how C6 happened once already.
Ship two configurations, named.

#### P3 — "large sequential chunks beat scattered small accesses" — **right conclusion, wrong mechanism**

The conclusion holds. The stated mechanism (MLP + prefetch) is **measured absent**
(§0.3): dependency costs nothing, prefetch does nothing, and every access pattern lands at
787 ns within 0.3%. What remains is line granularity alone. Evidence:

- **MLP is capped at 2 in RTL.** `RM_PEND_N = 2` (`read_manager.cpp:145`), `nor_read_q`
  `depth=2` (`scheduler.cpp:15`), and `nor_controller.v` arms only while `cmd_valid` is
  LOW so **back-to-back commands are impossible by construction**. Bus occupancy is 100.0
  cycles/line: the media serves one line at a time.
- **The flat 2→64 reader sweep proves MLP does NOT help.** Flat throughput with rising
  concurrency means latency grows linearly with concurrency (Little's Law) — a saturated
  serial resource. Total available MLP win: single-thread sequential 149 MB/s → 240 MB/s at
  two readers = **1.63x ceiling**, and one thread cannot reach it.
- **PHY-direct bounds the argument rather than supporting it.** Knee moved 4 → 2, ceiling
  unchanged at 240.0 = 240.0. "Two is enough, more changes nothing" is evidence against
  MLP hiding latency.
- **What actually makes sequential better:** (i) line granularity — 16 useful 8-byte words
  per 787 ns instead of 1; (ii) LLC residency on re-read. Both are caching effects, hard-
  bounded at 16x, and **exactly zero beyond 128 B**, now measured rather than argued.

**The design rule that survives:** pack data so that every 128 B line you touch is fully
used. Beyond line granularity, layout is irrelevant on this machine.

Note the sting: scattered accesses that *fill* a 4 KB page and sequential access over that
page touch the same 32 lines and cost the same. The categories only separate at sub-line
density — or via prefetch, which is unmeasured (§0.3).

## 6. Sequencing

Phases 0 and 1 gate the credibility of every measurement taken afterwards.

### Phase 0 — three fixes, before anything else **[D]**

| # | change | why first |
|---|---|---|
| 0a | `mm/ltram.c:169` — add `__GFP_THISNODE \| __GFP_DIRECT_RECLAIM \| __GFP_IO \| __GFP_FS \| __GFP_MOVABLE` to `mtc.gfp_mask` | Currently bare `GFP_LTRAM`, so under pressure `ltram_migrate_to()` takes node 1's *fallback* zonelist into node 0 DRAM and **silently returns a DRAM page for a migrate-to-flash request** |
| 0b | `mm/page_alloc.c:3346` — `VM_BUG_ON` → `WARN_ONCE` | Compiles to nothing without `CONFIG_DEBUG_VM`, so the LtRAM→DRAM direction is invisible on a production build. The always-on guard at 3205 checks the direction that never happens |
| 0c | Delete/gate the `__get_fault_gfp_mask()` LtRAM block; delete the false vDSO claim in the comment at `mm/memory.c:2953` | Stops the live corruption of §5.2 |

### Phase 1 — counters, before any measurement **[D]**

Per-`mm` and global, in sysfs: pages migrated to flash, pages repatriated, LtRAM read
faults, write faults on LtRAM folios, allocation failures, `ZONE_LTRAM` free pages, total
erases, per-sector erase histogram.

> **Standing rule: a benchmark result is inadmissible unless reported alongside a non-zero
> `pages_on_ltram`.** Five lines of `atomic_long_inc()`, and it is the direct antidote to
> the entire C6 failure class.

### Phase 2 — scoping (§5.3), landed and booted alone

Confirm the machine is unaffected and `ZONE_LTRAM` stays empty. That proves the opt-in
works before anything depends on it.

### Phase 3 — write protection + migration

Must land **in the same commit** as the first caller of `ltram_migrate_to()`:

- the zone check in `remove_migration_pte()` (`mm/migrate.c:224`) — otherwise migration
  itself restores a writable PTE onto flash, with no fault and no `mprotect`
- the `st_wait=2` gate (§4.3)

### Phase 4 — scanner and mover

### Phase 5 — measure

---

## 7. Open questions

| # | question | why it matters |
|---|---|---|
| O1 | What is `f`, the observed clean fraction? | Sets the scan batch size. Measurable today from `/proc/vmstat` NUMA counters under load (§2.4) |
| O2 | Skew threshold that triggers static levelling, and its budget share | Started at 10% of erase budget — a guess, not a measurement |
| O3 | Boot-time erase strategy — B-3 (start all DIRTY, background pre-erase to `wmark_promo`, promotion gated until ready) vs B-4 (B-3 plus a sysfs trigger to pre-erase the whole device before a run) | B-4 gives an erase-free measurement window. Still owed a written explainer |
| O4 | Free-list watermark size | §3.7 — B-3's 12,288 reserves 19% of the device; variance needs far less. Start small, raise on observed stalls |
| ~~O5~~ | **ANSWERED 2026-08-14, in the worst direction — see §3.8** | Erases ride the 2-deep `nor_read_q` at strict priority; no erase-suspend, no bank parallelism; 6653/6653 probes stalled |
| O6 | Replacement policy when full | §3.9 — first implementation is "none"; the promotion-order bias must be reported |
| O7 | Experiment budget as a fraction of device life | §0.2 — must be decided and stated in the paper |

---

## 8. Rejected, with reasons

| idea | why rejected |
|---|---|
| Stability counter `K > 1` | Identical detection power to `K=1` with a longer interval, but requires per-page state (§2.2) |
| One interval for both scan and migration | Forces a false trade-off between classification quality and fill time (§1) |
| **Global remaining-erase budget** (`N x E_max - E_used`) as the mover's rate input | Treats endurance as fungible across sectors. In the pinned case — read-mostly data holding most sectors while a few cycle — `E_used` is low, so it reports a huge budget and runs at full speed while the circulating sectors are destroyed. Kept only as a note (§3.2) |
| Allocation-time routing (`__get_fault_gfp_mask`) | Corrupts the page cache; wrong reach; structurally the wrong moment (§5.2) |
| Read-back verify in the production write path | Perturbs the cache behaviour being measured (§4.4) |
| Sorted list for the free list | 65,536 entries re-sorted per insert. Use erase-count buckets (§3.3) |
| Relying on hardware DBM | Absent on this silicon — and would have been *dangerous* for LtRAM pages anyway (§4.1) |

---

## 9. Provenance

Every source line cited here was read from `~/ltram-policy-bench/linux` at 6.8.0 on
2026-08-13/14. Findings C3, C5 and C6 were adversarially reviewed against that tree on
2026-08-13; C5 (the `DC ZVA` hazard) was **withdrawn** — the Arm ARM states `DC ZVA` sets
`CM = 0`, so the fault classification is correct. Its residual value is rule §4.2.
