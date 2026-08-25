# 260826_step1b_buckets — allocation moves to the erase-count buckets

**Verified on hardware 2026-08-26**, zuestoll08, kernel `6.8.0-ltram #10`
(`7af90458f`), bitstream `169_phy200`.

Same script, same workload, two kernels. `check1a.sh` runs
`matmul --n 2048 --iters 200 --runs 12` with the **backend deliberately not loaded**, so
every migration fails at the flash write and each allocated page is released immediately.
That is an artificial failure loop, and it is the point: it drives thousands of
allocate/free cycles per minute, which stresses the allocator and its bookkeeping far
harder than pages that are allocated once and left resident.

## The result

| | 1a — `find_first_bit` | 1b — bucket pop |
|---|---|---|
| `total_allocs` | not reported | **2,752** |
| `touched` | **1** page | **2,752** pages |
| max erase count | **1,518** | **1** |
| spread (max − min) | 1,518 | **1** |
| `most_worn` | `0 : 1518`, nothing else | flat, every entry `: 1` |
| `invariant` | ok | **ok** |

**`total_allocs == touched == 2752`.** Every allocation went to a different page; not one
page was handed out twice in the entire run. Peak wear on any single sector fell from
**1,518 to 1**.

The log2 histogram confirms it independently — `never 62784` plus `[1..1] 2752` is exactly
65,536 — and `free_buckets` still shows all pages in `[0..999]`, correct while no count has
reached 1,000.

## Why 1a concentrated on one page

`find_first_bit()` returns the lowest free index. With no backend the destination is
released before the next candidate's is allocated, so the next call returns *the same
index*. Allocate → refuse → free → allocate the same page again, 1,518 times, while 65,535
sectors were never touched.

1b pops the head of the lowest non-empty bucket and frees to the tail, so a released page
goes behind every other free page. That is round-robin while erase counts are equal, which
they are at the start of life.

## What this run does NOT establish

- **The workload is the artificial no-backend failure loop.** It proves rotation; it says
  nothing about behaviour when pages actually stay resident.
- **`touched` is 2,752 of 65,536 — 4% of the window.** The allocations spread across the
  pages *needed*, not across the device. Full-window spread only appears once pages remain
  resident and the allocator has to keep walking forward.
- **`total_allocs` is exactly 2× `promote_failed` (2,752 vs 1,376).** Each candidate takes
  two destination allocations, presumably one retry inside `migrate_pages`. Worth
  understanding before the erase count is used as a wear figure, since it means the count
  currently overstates real erases by 2× on the failure path.
- **Erase counts remain an upper bound** — incremented at allocation, not at erase
  completion. 1c replaces that with an erase-completion hook.

## The instrumentation earned itself here

The 1000-granularity histogram this replaced could not have shown any of it: `1` and `47`
and `999` all land in the same bucket. `total_allocs`, `touched` and `most_worn` were added
on 2026-08-25 specifically because the earlier claim about wear skew rested on the maximum
plus arithmetic rather than on a distribution anyone could read. This run is the first where
the answer is simply visible.
