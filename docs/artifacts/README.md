# Artifacts — the written record, kept in the repo

These are the published artifacts, saved here so they survive a session ending, a context
compaction, or the artifact host being unreachable. **The repo copy is the durable one.** The
live URLs are the editable ones; if you change a page there, re-save it here.

To view: open the `.html` files directly in a browser. They are self-contained — no external
scripts, no CDN, all data embedded — so they work offline.

| file | live URL | what it is |
|---|---|---|
| `ltram-implementation-and-method.html` | [e0841696](https://claude.ai/code/artifact/e0841696-6411-4a3e-a062-fa0fcabb6885) | **The main reference.** Every kernel change with file and line; every test with exactly what is inside the timer and what is excluded; every number with its provenance and confidence; and what would falsify each one. Answers the address-map, zone, allocator, wear, locking and migration questions directly. |
| `nor-cost-of-living.html` | [f90c71d6](https://claude.ai/code/artifact/f90c71d6-dbe6-4dfd-be66-445e6777e5d7) | The step-7 measurement, visualised. Per-pass timings as a 64 MiB working set migrates onto NOR, log axis with residency overlaid. Separates the one-time migration cost from what residency actually costs. |
| `design-review-outline.html` | [4a55eda5](https://claude.ai/code/artifact/4a55eda5-148d-46fa-8d31-d5bdce52a04d) | Slide-by-slide outline for the Enzian design review — hardware campaign, kernel implementation, and the evidence for each claim. 33 slides with a cutting guide to 30 min. |
| `issue10-resolved.html` | [5a22f854](https://claude.ai/code/artifact/5a22f854-d1e9-41c8-b74f-58bedbf07a69) | Issue 10, resolved. Part of the 12-page issue series; the rest live only at their URLs. |

## The test scripts, as run

Kept beside the artifacts because the methodology sections cite them, and a result whose script
has been lost is not reproducible.

| script | what it measures |
|---|---|
| `issue10-fix.sh` | detection with and without an application hint (T1) |
| `station4.sh` | end to end — do pages reach NOR and read back correctly (T2/T3/T4) |
| `station5.sh` | capacity and writeback, chained (T5 driver; capacity never ran cleanly) |
| `station6.sh` | the fault path back to DRAM (T5) |
| `station7.sh` | the cost measurement, DRAM control vs migrating vs resident (T6) |
| `check1a.sh` | page-state shadow validation — hammers the transitions and asserts the invariant |

## Numbers to trust, in one place

| claim | value | confidence |
|---|---|---|
| DRAM baseline, one cold pass | 0.1075 s (sd 0.09%, n=1500) | high |
| Steady state on NOR | 0.5139 s (sd 0.08%, n=222) | high |
| **NOR / DRAM, steady state** | **4.78x** | high — 4.87x reproduced independently |
| Peak during migration | 2.971 s (27.6x) | shape robust, exact peak is n=1 |
| Transparent detection | `rej_writable == rearmed`, two programs | high |
| Writeback correctness | 0 stale, 0 wrong, 7,517 pages | n=1, one region |

**Read the caveats in §4 of the main artifact before quoting any of these.** In particular the
on/off delta still mixes flash read latency, NUMA distance and migration cost, and separating
them needs a DRAM-backed node-1 control that does not exist.
