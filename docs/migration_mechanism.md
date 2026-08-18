# Migration mechanism using FPGA-side DMA engine

The LRW queue tells us **which** DRAM page to migrate. This document
covers **how** the migration happens. Source page lives in node 0
DRAM, destination page lives in node 1 LtRAM, and the FPGA-side soft
DMA engine performs the data motion.


## Architectural choice: DMA engine on the FPGA side

Putting the engine on the FPGA side means:
- For migration to LtRAM: source data crosses ECI once (FPGA pulls
  from node 0 DRAM), destination write is local to FPGA.
- For repatriation: source read is local to FPGA, destination crosses
  ECI once (FPGA pushes to node 0 DRAM).

In both directions only the heavy traffic leg uses ECI, and only
once. Engine on the CPU side would either need two ECI hops or would
contend with application code for CPU cycles. Engine on the FPGA side
is the correct choice for this workload.

The CPU is free during migration. The kernel submits the DMA,
sleeps until the completion interrupt, then does the bookkeeping
(PTE update, rmap fix-up). Migration cost in CPU cycles is the
bookkeeping, not the data motion.


## Cache and coherency notes

Coherent DMA over ECI means cache-maintenance steps (`dma_map_*`'s
flush hooks) are essentially no-ops. The hardware consults CPU cache
state automatically; lines in CPU cache are supplied cache-to-cache.

Even for write-cold pages, read-hot cache lines may exist. They sit
in shared state. Coherent DMA reads from shared state are cheap and
do not invalidate. So cache activity during migration is nonzero but
inexpensive.

Keep the full safety measures (`dma_map_page`, page locking,
completion barriers) because they do **more than cache maintenance**:
they pin pages, handle IOMMU entries (if stage-1 SMMU is present),
manage refcounts, and encode ordering between DMA completion and PTE
updates. None of that is no-op on the coherent path.


## End-to-end migration sequence (page placement, DRAM -> LtRAM)

1. **Pop LRW_INACTIVE head under the list lock**. Detach the page
   from the list while holding the spinlock; release the lock before
   doing anything else.
2. **Re-check the page is still write-cold.** If the dirty bit got
   observed in the window between picking and acting, requeue and
   try a different candidate.
3. **Lock the source page** (`lock_page(src)`).
4. **Unmap from all processes** via `try_to_unmap(src,
   TTU_MIGRATION)`. PTEs are replaced with migration entries that
   block any concurrent access until migration completes.
5. **Allocate destination on node 1**: `alloc_pages_node(1,
   GFP_KERNEL, 0)`. Lock destination (`lock_page(dst)`).
6. **DMA mapping**: `dma_map_page(dev, src, 0, PAGE_SIZE,
   DMA_TO_DEVICE)` returns `src_dma`. Destination address is
   `page_to_phys(dst)` (bit 40 already set). Optionally
   `dma_map_page(..., DMA_FROM_DEVICE)` the destination for idiomatic
   correctness; no-op on coherent path.
7. **Program FPGA DMA engine via MMIO** (AXI lite registers in the
   `0x9000_0000_0000` range): SRC, DST, LEN, CMD=GO.
8. **Wait for completion** via interrupt
   (`wait_for_completion(&dma_done)`). Task sleeps; CPU does other
   work.
9. **Unmap DMA**: `dma_unmap_page(dev, src_dma, PAGE_SIZE,
   DMA_TO_DEVICE)`.
10. **Swap the reverse mapping**:
    `remove_migration_ptes(src, dst, /*write_protect=*/true)`.
    All PTEs that pointed at `src` now point at `dst`, marked clean
    and non-writable. The write-protect is the hook that triggers
    repatriation on any future write.
11. **Unlock** dst and src, **free** src (`__free_page(src)`).


## Grace period: abortable migration with commit deadline

A write that arrives shortly after migration costs a NOR write +
NOR erase + new NOR write to repatriate. To avoid this thrashing,
the migration is **abortable** for a brief grace period after the
copy completes.

### Phase A: copy

Standard migration entries block writes during the copy. Unchanged.

### Phase B: grace period (new)

After copy completes, instead of immediately committing:
- Reinstall PTE pointing at the **original DRAM page**, marked
  **clean and non-writable**
- The LtRAM copy exists but is not yet referenced by any PTE
- Start grace timer (a few ms, tunable)
- DO NOT free the original DRAM page yet

### Phase C outcome 1: grace expires cleanly

No write trap during grace. Commit the migration:
- Update PTE to point at LtRAM page (still write-protected)
- Free the original DRAM page
- Drop the LtRAM page out of the pending-grace set

### Phase C outcome 2: write trap during grace

Migration aborts:
- Fault handler sees pending-grace state
- Restore PTE to point at DRAM, writable and dirty
- Release the LtRAM page back to node 1's free list (the NOR sector
  will eventually be erased when next reused; sunk cost)
- Mark the DRAM page "recently aborted" so the placement policy
  avoids it for a cool-down window

### Why this is strictly better than commit-immediately

Wasted commit cost: 1 NOR write + 1 erase + 1 new write (repatriate).
Wasted abort cost: 1 NOR write (the original copy).

For any mis-classified page caught within grace, abort is cheaper.
For correctly-classified pages, grace expires cleanly and the only
overhead is the brief PTE-pending-grace state plus the deferred DRAM
free.

### Implementation note

The grace-period write trap and the post-commit repatriation trap
funnel through the same write-fault handler. The handler reads the
page's state (pending-grace vs committed) and branches. One new
state, one new branch. The rest of the machinery is unchanged.

Writes that arrived during phase A (blocked by migration entries)
also funnel through this same abort path: the waiter wakes up after
the PTE is reinstalled in write-protected state, retries, traps,
and the handler sees pending-grace and aborts. No separate logic
needed.


## Repatriation sequence (write fault on LtRAM page, LtRAM -> DRAM)

Mirror of the placement path. Triggered by a write that traps because
the LtRAM page's PTE is non-writable:

1. Fault handler locks the LtRAM page.
2. Allocates fresh node 0 page (`alloc_pages_node(0, GFP_KERNEL, 0)`).
3. Programs FPGA DMA engine: source = LtRAM PA (local read), dest =
   new DRAM PA (write over ECI).
4. Waits for completion.
5. Swaps PTEs: new PTE is writable, points at the new DRAM page.
6. Frees the LtRAM page back to node 1 free list.
7. Enrolls the new DRAM page on **LRW_ACTIVE** (it just demonstrated
   write activity; exempt from the clear cycle; not a migration
   candidate). The page only rejoins the candidate pool
   (LRW_INACTIVE) when the periodic active-to-inactive re-test finds
   it has stopped faulting.
8. Optionally bumps a per-page "recently repatriated" cool-down
   counter so the placement policy avoids reconsidering this page
   for K seconds even after eventual demotion. Prevents oscillation
   for bimodal write patterns.
9. Unlocks and returns from the fault.

The process re-executes its write instruction, the new PTE permits
the write, the page is now in DRAM.


## What the kernel gives us for free

The migration entry / `try_to_unmap` / `remove_migration_ptes`
machinery in `mm/migrate.c` is already battle-tested. We are
replacing the `copy_page` step with a DMA call but keeping the
surrounding state machine intact. Specifically:

- `try_to_unmap` handles the per-process PTE removal, including COW
  pages, shared anonymous mappings, and file-backed mappings.
- The migration entry mechanism blocks concurrent accesses cleanly.
- `remove_migration_ptes` re-walks rmap and reinstalls real PTEs.

We mostly own:
- Step 7 (program the DMA engine instead of `copy_page`)
- The `write_protect=true` hook in step 10
- The fault-handler-driven repatriation path on the write side


## Read paths: application reads direct via ECI, migration reads via DMA

Same LtRAM page, two contexts, two paths.

**Application reads** (small, frequent, latency-sensitive): CPU
issues a load, MMU translates, physical address has bit 40 set,
memory controller routes over ECI, FPGA returns the cache line.
Transparent to the application. Do NOT put DMA in this path; the
overhead of programming the DMA engine for a single load would be
far worse than the direct ECI read.

**Migration reads** (page-sized, infrequent, bandwidth-bound):
FPGA-side DMA engine streams the data; CPU is free to do other work
while waiting for the completion interrupt.

The general principle: the right path depends on **granularity and
frequency**, not on the data itself. Tiny synchronous request from a
user load goes direct; page-sized batched request from migration
code goes through DMA.


## Locking discipline on the LRW queue

The LRW operation pattern is **multi-writer / single-reader**, even
with one user process:
- Multi-writer: fault handlers on any CPU, allocation paths, kswapd,
  kcompactd, NUMA balancing
- Single-reader: the migration thread

`struct list_head` is not safe for lock-free use (non-atomic
next/prev updates). Use a `spin_lock_irqsave` with a tight critical
section: hold only for the `list_del` / `list_add` itself, never
across migration work. Cost is 20 to 50 ns on ThunderX-1, in the
noise.

If contention shows up in profiling, fall back to per-CPU LRW
shards (similar to `lru_pvecs`). The migration thread peeks across
all per-CPU lists. Treat this as an optimization to apply only when
needed, not the default.


## Open questions for the prototype

- For the first prototype, scope to anonymous pages only. File-backed
  migration adds page cache coordination that we do not need for the
  ASPLOS pitch.
- Interrupt routing from FPGA to ThunderX: confirm with Enzian team
  how soft-IP interrupts surface to Linux.
- Multi-page batching: programming the DMA engine has fixed overhead.
  Migrating in batches of N adjacent or rmap-related pages amortizes
  that overhead. Worth measuring before committing to per-page or
  per-batch granularity.
- Failure paths: rollback if DMA fails, if destination allocation
  fails after unmap, etc. Standard kernel migration code has these
  patterns; we copy them.
