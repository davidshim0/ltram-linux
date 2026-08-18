# LRW design alternatives for LtRAM placement

The job of the LRW (least-recently-written) ranking is to identify
**write-cold** DRAM pages so the placement policy can migrate them to
LtRAM. The decision is **placement**, not demotion: LtRAM is a peer to
DRAM, not a slow tier below it, and migration is driven by the page's
write pattern, not by DRAM pressure.

LtRAM-resident pages do not need an LRW ranking. They are
PTE-write-protected, and any write traps to the fault handler, which
migrates the page back to DRAM. The DRAM-side LRW machinery handles
all the ranking; LtRAM-side trap-on-write handles all the repatriation.


## Vocabulary

Use placement vocabulary throughout, not reclaim vocabulary:

| Avoid (reclaim) | Prefer (placement) |
|---|---|
| demote to LtRAM | place on LtRAM, migrate to LtRAM |
| promote back to DRAM | repatriate on write, migrate back to DRAM |
| reclaim path | placement path, migration path |
| memory pressure triggers demotion | periodic policy or write event triggers migration |
| evict cold pages | segregate write-cold pages |


## Design space, in order of memory cost

### Path A: no list, scan-based (cheapest, best scalability)

No `list_head` on `struct page`. A scanner thread walks the physical
PFN range periodically. For each page, it reads the PTE dirty bit via
rmap, decides, optionally records a small per-page state.

Three sub-options for the per-page state:
- **No state**: decide on the fly. Cheap but coarse (false positives
  when a page is incidentally clean during one scan).
- **Two flag bits in struct page->flags**: "observed dirty this scan,"
  "observed dirty last scan." Pages with both clear across consecutive
  scans are real cold candidates. Zero new memory.
- **Page_ext age counter**: 1 byte per page in a lazy parallel array.
  Skip pages we do not care about.

Memory cost: 0 to 1 byte per page.

CPU cost: scanner walks every DRAM page every interval. For 128 GB
DRAM at 4 KB pages, that is 32M pages per scan. At 25 ms intervals,
~1.3 GB/scan of metadata reading, ~50 GB/sec sustained. ThunderX-1
can manage this if you scan less often (200 ms) or scan partial ranges
per tick.

**Best choice if you care about scaling to large LtRAM deployments.**


### Path B: single LRW list with clock semantics

One `struct list_head lrw` field in `struct page` (16 bytes per page,
0.4% of RAM). One list head per node. Operations:

- Allocation: page added to tail
- Migration thread peeks head:
  - If PTE dirty: clear dirty bit, move page to tail, peek again
  - If PTE clean: this is a migration candidate
- Optional periodic scan that pushes dirty pages to tail and clears
  their bits, keeping the list approximately ordered

This is the classic clock / second-chance algorithm. O(1) per head
check, O(K) per migration where K is the number of false candidates.

Memory cost: 16 bytes per page system-wide. 2.5 GB for 640 GB total
memory. Acceptable but not free at full Enzian capacity.

**Best choice for first prototype: simple, well-understood, easy to
reason about.**


### Path C: split active/inactive LRW

Same as Path B but with separate active-write and inactive-write
lists, mirroring the LRU shape. Same 16-byte cost; the page is on one
list or the other, not both. The split prevents a fresh-write page
from getting accidentally migrated just because the scanner walked
past its head position before its dirty bit was observed.

**Best choice only if Path B's metrics show that fresh-write pages are
being mis-migrated.** Otherwise the simplicity of Path B wins.


### Recommended sequence

1. Build Path B for first prototype on the bench. Simple, easy to
   instrument. 256 MB LtRAM today means the 16-byte cost is
   negligible.
2. When deploying to full Enzian (512 GB LtRAM), revisit and consider
   migrating to Path A for scalability.
3. The paper can present both as an ablation:
   "Path B is the easy path; Path A scales further with X% added CPU
   cost and 0 metadata cost." The deltas are the contribution.


## ARMv8.0 / DBM-absent reality on Enzian, reframed as a strength

**Enzian uses ThunderX-1, which is ARMv8.0. There is no DBM.** All
three paths above use the PTE dirty bit as the write signal. On
ARMv8.0 that bit is **not set by hardware** on writes. The kernel
falls back to software emulation: clean PTEs are marked non-writable,
the first write to a clean page faults, and the fault handler sets
the dirty bit in software and re-enables writes.

Cost per clean-to-dirty transition: roughly 1 to 5 microseconds (a
page fault). On DBM-equipped silicon: nanoseconds.

### Fault-as-event-signal: why this is actually advantageous

With DBM, hardware sets the dirty bit transparently. The OS learns
about the write only when it scans PTEs, and all it sees is state, not
timing. A bit set 24 ms ago looks identical to a bit set 1 ms ago.

With no DBM, every first-write fault hits the kernel **at the moment
of the write**. The signal is event, not state: precise, timestamped,
per-page. The OS-managed placement scheme can:

- Move the page to the tail of the LRW list **inside the fault
  handler itself**. No periodic scanner needed for the inactive set.
  The list is always sorted by recency-of-last-write at fault
  granularity.
- Maintain a per-page fault counter (one byte). Counter increments on
  each first-write fault. Across clearing cycles, the counter is the
  page's write-warmth signal.
- Implement an active/inactive split (see below) cheaply, since the
  fault stream tells us exactly which pages are still being written.

**Paper framing**: we design our placement around the write-fault
event rather than the dirty-bit state. We pay a microsecond-scale
fault per clean-to-dirty transition; in return we get strictly better
write-recency information than a DBM-equipped scanner would provide.
For read-heavy workloads the fault rate is bounded, and the policy
benefits from the precise signal.


## Active/inactive write list, driven by fault counts

The placement queue is split into two lists:

- **LRW_INACTIVE**: candidates for migration. Their PTEs are marked
  non-writable-clean. First write faults; the fault handler bumps the
  page's counter and moves it to the tail of LRW_INACTIVE.
- **LRW_ACTIVE**: write-hot pages, exempt from the clearing cycle.
  Their PTEs stay writable-dirty permanently. No faults on them.
  Invisible to the migration walk.

Transitions:

- **Inactive to active (promotion)**: page's fault counter exceeds a
  threshold within some window. For example, faulted in 3 consecutive
  clearing cycles. Move to LRW_ACTIVE, stop clearing its PTE.
- **Active to inactive (demotion)**: periodically (much less often
  than the inactive clearing cycle, perhaps every 10 cycles), test a
  fraction of the active list by re-clearing PTE write permission.
  Pages that do not fault again for several test cycles are no longer
  write-hot; move back to LRW_INACTIVE.

Migration picks from the head of LRW_INACTIVE only.

**Steady-state cost**: write-hot pages pay zero fault overhead.
Application fault rate is bounded by the rate of new pages becoming
write-cold or write-warm, not by total write rate. For read-heavy
workloads, this keeps the steady-state fault overhead modest.

**Memory cost**: one `struct list_head` per page (a page is on
exactly one of inactive or active, not both, so a single field
suffices), plus one byte of fault counter. About 17 bytes per page,
0.4% of system RAM.


## Lazy migration thread, no fixed scanner cadence

The migration thread runs on demand, when the placement policy
decides it wants a candidate (LtRAM has free capacity, or we are
below a quota threshold, or the system is otherwise idle). Each
invocation:

1. Peek the head of LRW_INACTIVE.
2. If the head's PTE is dirty (write happened between the last clear
   and now): clear the dirty bit, move the page to tail, peek again.
3. If the head's PTE is clean: this is a write-cold candidate.
   Migrate it.

No separate periodic scanner. The fault handler does the list
maintenance lazily on every clean-to-dirty event; the clearing pass
runs only as part of migration. Work happens only when migration
happens.


## On FPGA assistance, and what does or does not work

The FPGA is on the coherent fabric and can read DRAM, including page
tables. Mechanically, the FPGA could walk page tables and read PTEs.

**This does NOT solve the DBM problem.** On ARMv8.0 the PTE dirty bit
is only set by software fault handlers; reading the PTE faster does
not eliminate the fault. The FPGA scanning PTEs is functionally
equivalent to the CPU scanning PTEs.

To observe writes without faults on ARMv8.0, the FPGA would have to
**snoop the coherent fabric for write transactions to DRAM
addresses.** That is a new HW feature (the "FPGA write tracker") and
inflates the HW contribution claim. **The user's design choice is to
NOT add this.**

### One narrow FPGA capability that DOES fit "just a memory controller"

The LtRAM memory controller naturally sees every write to LtRAM
addresses because those writes pass through it. **Exposing per-page
write counters as a control-register region is part of being a
controller**, not an extension to it. A DRAM controller exposes ECC
counts and refresh statistics; an LtRAM controller exposing per-page
write counts is consistent with the endurance-state contract from the
DIMES paper.

This gives **fast LtRAM-side repatriation detection** for free. It
does **not** help with DRAM-side LRW (which is the bulk of the work),
but it is a natural-feeling capability that does not require defending
new HW.


## Paths for write tracking without new HW

Three options, in order of intrusiveness:

1. **Linux soft-dirty mechanism** (`/proc/PID/clear_refs`,
   `/proc/PID/pagemap`). Userspace API, already exists. A placement
   daemon can drive it. Slow but zero new kernel code.
2. **In-kernel periodic dirty observation** modeled on soft-dirty but
   driven by the placement policy. Same cost model, kernel owns the
   loop. **Recommended for the ASPLOS submission.**
3. **NUMA-balancing-style PROT_NONE sampling.** Faults on any access,
   not just writes. Useful if read patterns also matter; overkill
   otherwise.


## Open implementation questions

- Scan interval: 25 ms is a starting guess. Tune to workload write
  rate.
- Where the scanner lives: probably a separate kernel thread, since
  semantics differ from kswapd or kcompactd.
- Anon vs file-backed: file-backed writes already feed writeback; do
  we also place file-backed pages on LtRAM, or only anon?
- Paper metric: write amplification factor, LtRAM hit rate, fraction
  of working set kept off DRAM, or some combination.
