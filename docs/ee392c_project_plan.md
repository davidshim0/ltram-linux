# EE392C Project Plan: LtRAM Placement on Linux

Branch: `250522_Implementation`. Target: a working LtRAM placement
prototype on the existing x86 QEMU testbench, producing two milestone
figures plus a wear-leveling characterization.


## 1. Project scope

A class project, not a research paper. Constraints:

- QEMU x86 only. No real Enzian hardware for this submission.
- Out-of-tree loadable kernel module under `drivers/misc/ltram/` (or
  similar). Does not depend on kmohr's private-memory framework.
- Single developer, ~5 to 6 weeks remaining.

What we will **not** ship in this version:
- LRW queue / active-inactive split
- Mixture of Predictors
- FPGA DMA migration
- Integration with Gourry's framework
- Real ARM64 / DTS work

What we **will** ship:
- A driver that exposes `/dev/ltram` and forces mapped memory onto
  node 1.
- A write-fault repatriation path that migrates pages back to node 0
  via the kernel's `migrate_pages` API.
- Per-page write counters for wear measurement.
- Two milestone experiments with figures.


## 2. Operating model

The simplest tier semantics that demonstrates the idea:

1. Workload `mmap`s `/dev/ltram` to allocate a region.
2. Reads fault into the `.fault` callback, which allocates a fresh
   page on **node 1** (LtRAM) and installs it read-only.
3. Writes trap into `.page_mkwrite`, which uses `migrate_pages` to
   move the page to **node 0** (DRAM), writable.
4. Repatriated pages stay on node 0 for the rest of the run.

Steady state: cold data ends up on LtRAM, anything that ever gets
written ends up on DRAM. Read-heavy workloads naturally settle most
pages on LtRAM; write-heavy workloads naturally end up with most
pages on DRAM. This is the placement signal we want to demonstrate.


## 3. Milestone 1: placement correctness, Configuration A

Configuration A is the existing default in `scripts/run-vm.sh`: both
guest NUMA nodes backed by host node 0. No real latency asymmetry.
Tests **placement decisions** in isolation from the latency-effect
question.

### What this milestone shows
- Read-heavy workloads place most pages on LtRAM and they stay there.
- Write-heavy workloads correctly repatriate.
- Wear is distributed across LtRAM pages rather than concentrated.

### Figure 1A: per-workload placement outcome (stacked bar chart)

X-axis: workloads (matmul, pagerank, YCSB-C, plus baseline read-only
microbenchmark and write-only microbenchmark for calibration).
Y-axis: number of pages, stacked into three categories at end of run:
- Pages still on LtRAM (the win condition)
- Pages repatriated to DRAM
- Pages never accessed (allocated but unused)

Expected shape: YCSB-C tall blue bar (LtRAM-resident); matmul tall
red bar (repatriated); pagerank somewhere in between.

### Figure 1B: wear distribution

X-axis: LtRAM page index (sorted by write count descending).
Y-axis: number of writes received by that page over the run.
One line per workload.

Plus a summary table:
- Min, max, mean, median, std-dev of writes per LtRAM page
- Max/min ratio (excluding zeros)
- Coefficient of variation (std-dev / mean)
- Gini coefficient over write counts

Expected shape: relatively flat distribution. If we see a hot tail,
that is a real result we should explain.

### Metrics to collect

Per-event counters (incremented in the driver, exposed via debugfs):
- `nr_alloc_node1`: pages allocated on LtRAM (read fault path)
- `nr_repat`: pages repatriated to DRAM (page_mkwrite path)
- `nr_repat_within_1s`: repatriations within 1 second of placement
  (a measure of "false positives" if we treat all allocs as
  speculative placement)
- `nr_pages_resident_node1`: instantaneous count, sampled periodically
- `repat_total_us`: cumulative microseconds spent in repatriation
- Per-page write-count array (kept in the driver's per-VMA state)

Derived metrics:
- **Placement effectiveness**: `(nr_alloc_node1 - nr_repat) / nr_alloc_node1`
- **Repatriation cost share**: `repat_total_us / total_runtime_us`
- **Steady-state residency**: pages on LtRAM at end / total mapped pages
- **Cold coverage**: pages that were never written / pages on LtRAM at end
  (high coverage = we found the cold pages correctly)

### Other measures of correct placement worth considering

These go beyond what the user listed; we can include some as
secondary plots if time allows:

1. **Page residency CDF on LtRAM**: distribution of "time spent on
   LtRAM before either run-end or repatriation." Confirms cold pages
   stay a long time, hot pages get evicted quickly.
2. **Per-VMA placement outcome**: stratify Figure 1A by VMA (heap vs
   anon mmaps vs file-backed). YCSB-C's value table should look
   different from its index VMA.
3. **Write fault rate over time**: faults/second curve. Should be
   high in the first second (initial population) and drop toward
   steady state. If it stays flat-high, the workload is genuinely
   write-active and the LtRAM model is a bad fit.
4. **False placement rate**: of all placements, what fraction
   repatriated within T seconds. Different T (100 ms, 1 s, 10 s)
   gives different views of policy accuracy.
5. **Read traffic on LtRAM as fraction of total memory reads**:
   ideally high. Requires instrumenting reads, which is hard
   without hardware counters; can skip if too expensive.

### Workloads

From the existing testbench:
- **YCSB-C**: read-only, large value table. Expected: most pages
  stay on LtRAM.
- **matmul**: writes the result matrix. Expected: result repatriates,
  input matrices stay on LtRAM.
- **pagerank** (GAPBS): mixed; rank vectors update each iteration.
  Expected: most graph data stays on LtRAM, rank arrays repatriate.

Plus two calibration microbenchmarks (new code):
- **read_only_calib**: allocates a region via `/dev/ltram` and only
  reads. Should produce 100% LtRAM residency.
- **write_only_calib**: allocates a region via `/dev/ltram` and only
  writes. Should produce 100% repatriation.

### Wear-leveling notes

The naive implementation (alloc_pages_node always returns the
allocator's preferred page) tends to reuse the same pages over and
over as they get freed and reallocated, which concentrates wear.

For Milestone 1 we will measure this raw behavior and report it
honestly, then introduce a simple wear-leveling tweak:

- **Round-robin allocator wrapper** in the driver: maintain a free
  list of node-1 pages, draw from it in rotation rather than letting
  the buddy allocator hand back the most-recently-freed page.

If round-robin is enough to keep CoV under (say) 1.5 across the
workloads, we are fine. If not, we will discuss the gap and what a
real wear-leveling pass would look like (without implementing it).


## 4. Milestone 2: latency impact, Configuration B

Configuration B (commented out in `scripts/run-vm.sh` currently):
m0 bound to host node 0, m1 bound to host node 1. Accesses to guest
node 1 incur real cross-socket DRAM latency.

### What this milestone shows
- The latency penalty of putting cold pages on LtRAM is bounded.
- Extrapolated to realistic MRAM / RRAM latencies, the design is
  still viable for read-heavy workloads.

### Figure 2A: workload end-to-end time

X-axis: workloads.
Y-axis: wall-clock runtime.
Two bars per workload:
- "All DRAM" baseline: normal mmap, all pages on node 0.
- "LtRAM enabled": mmap of `/dev/ltram`, pages on node 1 until
  repatriated.

Expected: small slowdown for read-heavy workloads, larger slowdown
for write-heavy (driven by repatriation cost, not access latency).

### Figure 2B: extrapolation curve

X-axis: assumed LtRAM read latency multiplier vs DRAM (1x to 10x).
Y-axis: predicted workload slowdown for a given workload (YCSB-C is
the prime candidate).

Method:
1. Microbenchmark measures actual node 0 vs node 1 latency on
   Configuration B. Call this ratio R_measured.
2. Measure workload slowdown S_measured at R_measured.
3. Assuming first-order linearity in memory-bound region, predict
   slowdown S_predicted at other R values:
   `S_predicted(R) = 1 + (S_measured - 1) * (R - 1) / (R_measured - 1)`
4. Annotate the curve with known substrate ratios: MRAM (~5x),
   RRAM (~5 to 10x), PCM (~10x).

This is the figure that says "if LtRAM were actually MRAM, the
workload would see X% slowdown, which is acceptable for the density
benefit."

### Microbenchmarks for Milestone 2

- **Pointer-chasing latency**: allocate a circular linked list with
  cache-defeating stride, time per-hop access on each node. Reports
  raw read latency.
- **Sequential bandwidth**: STREAM-style copy/triad on each node.
  Reports peak bandwidth.

Both runs once per node (using `numactl --membind=N`) to characterize
the platform before running the workloads.


## 5. Implementation tasks

### A. Kernel module (in `drivers/misc/ltram/`)

1. **`Kconfig`** and **`Makefile`**. Build as `tristate` so it can be
   loaded/unloaded without rebooting.
2. **Module init/exit**: registers `/dev/ltram` char device.
3. **`ltram_mmap(file, vma)`**: install `vm_ops`, set `VM_SHARED`,
   stash per-VMA state in `vma->vm_private_data`.
4. **`ltram_fault(vmf)`**: read fault handler. Allocate page on
   node 1 via `alloc_pages_node(1, GFP_HIGHUSER | __GFP_ZERO, 0)`.
   Lock, set `vmf->page`, return `VM_FAULT_LOCKED`. Bump
   `nr_alloc_node1`.
5. **`ltram_page_mkwrite(vmf)`**: write fault handler. Call
   `migrate_pages` with a callback that allocates on node 0.
   Increment per-page write count. Bump `nr_repat`, accumulate
   `repat_total_us`.
6. **Per-VMA state struct** with per-page write counter array
   (`u32 *writes` indexed by VMA offset / PAGE_SIZE).
7. **`ltram_close(vma)`**: free remaining node-1 pages, dump
   per-page write counts to debugfs/results file.
8. **Round-robin alloc wrapper** to spread wear. Maintain a per-fd
   free list; allocate pages in rotation.
9. **debugfs interface** at `/sys/kernel/debug/ltram/`:
   - `stats`: counters
   - `per_page_writes`: dump array per active VMA
   - `clear`: reset counters

### B. Userspace test programs (in `workloads/`)

10. **`workloads/basic_tests/ltram_smoke.c`**: smoke test that mmaps,
    reads, writes, verifies, exits. First validation gate.
11. **`workloads/basic_tests/read_only_calib.c`** and
    **`write_only_calib.c`**: calibration microbenchmarks.
12. **`workloads/microbench/ptr_chase.c`**: pointer-chase latency.
13. **`workloads/microbench/stream.c`**: STREAM bandwidth.
14. **Wrappers around YCSB-C / matmul / pagerank** that route their
    main data structures through `/dev/ltram`. For YCSB-C, the
    Redis value table. For matmul, input matrices A and B. For
    pagerank, the graph CSR.

### C. Testbench changes

15. **`scripts/run-vm.sh`**: add `ltram_a` and `ltram_b` workload
    modes corresponding to Configurations A and B.
16. **`overlay/etc/init.d/S51ltramrun`**: `insmod ltram.ko`, run
    workload, dump stats, `rmmod`.
17. **Plotting scripts** under `workloads/monitoring/`:
    - `placement_outcome.py` for Figure 1A
    - `wear_histogram.py` for Figure 1B
    - `runtime_compare.py` for Figure 2A
    - `latency_extrapolation.py` for Figure 2B

### D. Documentation

18. Brief README in `drivers/misc/ltram/` describing the module.
19. Final writeup combining the four figures with analysis.


## 6. Timeline (5 to 6 weeks)

| Week | Goal |
|------|------|
| 1 | Module skeleton + smoke test passes. `/dev/ltram` mmap returns memory on node 1, reads work, no migration yet. |
| 2 | Repatriation working. Calibration microbenchmarks pass: read-only stays on node 1, write-only fully repatriates. |
| 3 | Per-page write counters and round-robin allocator. debugfs stats interface. |
| 4 | Workload integration. Modify YCSB-C / matmul / pagerank to use `/dev/ltram` for tracked data. Run full Milestone 1 experiments. |
| 5 | Configuration B. Latency microbenchmarks. Milestone 2 runs. |
| 6 | Analysis, plots, writeup. Buffer for surprises. |

If we hit a snag in Week 2 (migrate_pages issues, fault path
edge cases), the timeline absorbs one bad week without sliding the
final deliverable.


## 7. Risks and contingencies

| Risk | Mitigation |
|------|------------|
| `migrate_pages` does not work the way we expect inside `page_mkwrite` | Fallback: manually allocate a new page, `copy_page`, replace `vmf->page`. Less elegant but bypasses the migration framework if it disagrees with our use case. |
| Repatriation overhead dwarfs the access-latency savings | This is itself a result; report it honestly. The fix in production is the active/inactive split, which is out of scope here. |
| Workload modifications are invasive | Start with the calibration microbenchmarks. Add one real workload at a time. YCSB-C first (the most likely to show the desired pattern). |
| Wear concentrates badly even with round-robin | Report measurement, propose a real wear-leveling strategy in the writeup without implementing it. |
| Configuration B is hard to set up in our environment | The QEMU args are commented out and ready; the host needs >1 NUMA node. If the host has only one, fall back to "all DRAM" baseline and note the limitation. |


## 8. Two milestone figures and what each demonstrates

| Figure | What we are claiming |
|--------|---------------------|
| 1A (placement outcome) | "Read-heavy data correctly settles on LtRAM; write-heavy data correctly migrates back to DRAM. The placement decision is right." |
| 1B (wear distribution) | "Wear across LtRAM pages is approximately uniform with round-robin allocation. Worst-case wear is within X% of average. Endurance is not a deployment-blocker." |
| 2A (end-to-end runtime) | "On a substrate with real cross-socket latency, the workload-level slowdown of placement is small for read-heavy workloads." |
| 2B (extrapolation) | "Projected to MRAM-class or RRAM-class latencies, the design remains viable. The case for LtRAM as a peer tier is real." |

If we can produce these four figures with clean numbers, the project
has its narrative: placement works, wear is manageable, performance
is acceptable, the substrate generalizes.
