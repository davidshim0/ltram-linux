# LtRAM kernel architecture (post-kmohr patch)

This file lays out the layered kernel architecture for LtRAM work,
incorporating the Gourry private-memory-nodes framework that kmohr
applied in commit `bc4b49e8fbde "applied private memory patch"` on
2026-04-26.


## Foundation already present in the tree (layer 1)

kmohr applied the Linux upstream "Private Memory Nodes with
Compressed RAM" series by Gourry. The full patch series is preserved
in `marc_gourry_private_memory_nodes_w_compressed_ram.mbx` in the
repo.

Key elements now available:

- **`N_MEMORY_PRIVATE`** node state in `include/linux/nodemask.h`.
- **`pgdat->private`** bool and **`pgdat->node_private`** RCU pointer
  in `include/linux/mmzone.h`. `pgdat_is_private()` predicate.
- **`__GFP_PRIVATE`** flag (reclaimed from the unused 0x200 GFP bit).
  Allocator refuses to hand out private-node pages without this flag.
- **`GFP_PRIVATE`** = `__GFP_PRIVATE | __GFP_THISNODE`. No fallback;
  caller meant this node specifically.
- **`CONFIG_CRAM`** kernel option enables the framework.
- **Two API surfaces**:
  - `cram.h`: simple registration + lifecycle (`cram_register_private_node`,
    flush callback)
  - `node_private.h`: full `node_private_ops` with callbacks for
    free_folio, folio_split, migrate_to, folio_migrate, handle_fault,
    reclaim_policy
- **Reference consumers**: `drivers/cxl/type3_drivers/cxl_compression`
  and `cxl_mempolicy`

**Important terminology correction**: this is a **node-level**
distinction, not a zone-level one. Node 1's standard zones
(`ZONE_NORMAL`, `ZONE_MOVABLE`) remain. The privacy bit lives on the
pgdat, not on a new zone enum value. When team members say "LtRAM
zone," what is happening at the kernel level is "LtRAM node."


## Caveats from the patch application

The patch was applied with conflicts. Three `.rej` files exist in the
tree:
- `mm/huge_memory.c.rej`
- `mm/mempolicy.c.rej`
- `mm/migrate.c.rej`

These hunks did not apply cleanly and need hand-merging. Before
building on this framework, confirm with kmohr that they have been
resolved. Without resolution, large-folio splitting on private nodes,
NUMA mempolicy interactions, and some migration paths are subtly
broken.

`.orig` files exist alongside every modified file (the patch system's
pre-modification snapshots). Once the merge is settled, those should
be cleaned up.


## Layered architecture for LtRAM work

```
Layer 5: Mixture of Predictors (MoP)
         Multiple rankers in parallel; meta-predictor picks per
         page or per workload class. Branch-prediction-style.
            |
            | candidate page
            v
Layer 4: Placement policy
         LRW queue, active/inactive write list split, fault-driven
         list maintenance. Operates on node 0 pages.
            |
            | "migrate this page to node 1"
            v
Layer 3: Migration mechanism
         FPGA-side DMA engine performs the data move. Destination
         allocated via GFP_PRIVATE | __GFP_THISNODE.
            |
            | hooks into Gourry callbacks
            v
Layer 2: enzian_ltram driver (to write)
         Registers node 1 as private via node_private_ops.
         Provides free_folio, folio_migrate, handle_fault, etc.
            |
            | uses
            v
Layer 1: Gourry private-memory framework
         Already in tree. N_MEMORY_PRIVATE, __GFP_PRIVATE, callbacks.
```


## Layer 2: enzian_ltram driver (next major code to write)

A new driver under `drivers/` (probably `drivers/enzian/ltram/` or
similar) that registers node 1 as private and implements the
LtRAM-specific lifecycle.

**Callbacks to implement**:

- `free_folio`: page on LtRAM freed. Decision point for erase
  scheduling. May return "defer me" to hold the page until erase
  completes.
- `folio_migrate`: fires after data copy, both folios locked. We
  use this to write-protect the destination PTE and bump our
  endurance counter for the destination's NOR address. Exactly the
  hook our DMA-driven migration design wanted.
- `handle_fault`: process wrote to a write-protected LtRAM page.
  Runs the repatriation path (alloc DRAM page, FPGA DMA copy back,
  swap PTEs, enroll new page on LRW).
- `migrate_to`: kernel wants to migrate folios to node 1. Hook in
  the FPGA DMA engine here. Lets us delegate rmap and PTE machinery
  to the framework.
- `reclaim_policy`: stub for now. LtRAM is not driven by reclaim
  pressure.

**Registration site**: probably a module init or platform driver
probe that fires once node 1 is online.


## Layer 3: Migration mechanism

Already detailed in `migration_mechanism.md`. The only change post
kmohr is destination allocation:

```c
/* Replaces alloc_pages_node(1, GFP_KERNEL, 0) */
struct page *dst = alloc_pages_node(1, GFP_PRIVATE, 0);
```

Cleaner architectural option: trigger the migration via the
standard `migrate_pages()` path and let the framework invoke our
`migrate_to` callback. The framework handles unmap, migration
entries, and PTE swap. Our callback just runs the DMA. Less code to
own, less surface for bugs.


## Layer 4: Placement policy

Already detailed in `lrw_design.md`. Independent of the Gourry
framework; operates on node 0 pages, tracking write-recency.
Interface to layer 3 is "pop LRW_INACTIVE head, ask framework to
migrate to node 1."


## Layer 5: Mixture of Predictors

Each sub-predictor produces a ranked list of candidates. The MoP
meta-predictor watches outcomes (did the migration stick for N
seconds without repatriation?) and learns which sub-predictor to
trust per page or per workload class.

Sub-predictors to include initially:
- LRU-cold (existing kernel LRU)
- LRW-cold (our LRW list, layer 4)
- Perceptron (per-VMA, features = recent fault count, time since
  last write, accessed-bit pattern, allocation age)
- Geometric history (3 write-recency counters at 10ms / 100ms / 1s
  timescales; candidate must be cold on all three)

Evaluation axes for the paper:
- Accuracy: fraction of migrations that did not repatriate within
  N seconds
- Coverage: fraction of true write-cold pages identified
- Cost: CPU cycles per decision, kernel state in bytes

Ablation: each predictor alone, full MoP, intermediate MoPs using
subsets. The deltas isolate each predictor's value and the wrapper's
value.


## Suggested implementation order

1. **Resolve kmohr's .rej files** with her. Cannot move until this
   is settled.
2. **Write the enzian_ltram driver skeleton** (layer 2): registers
   node 1 as private, stub callbacks. Confirm GFP_PRIVATE
   allocations land on node 1 and other allocations don't.
3. **Build the FPGA DMA test module** (the test todo): verify the
   end-to-end CPU-to-FPGA write path on real Enzian hardware.
4. **Implement the migrate_to callback** with the FPGA DMA engine.
   Verify standard `migrate_pages()` calls into our callback and
   data lands correctly on node 1.
5. **Implement handle_fault for repatriation**. Now the round trip
   works.
6. **Build the LRW queue** (layer 4) with fault-driven maintenance.
   This is the policy-layer work, independent of the framework.
7. **Build the MoP harness** (layer 5) with the four initial
   sub-predictors. Wire it up to drive layer 4.
8. **Evaluation**: ablation across sub-predictors, full MoP, and
   cost/accuracy/coverage breakdowns for the paper.
