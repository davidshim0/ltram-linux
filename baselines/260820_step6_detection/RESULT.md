# 260820_step6_detection — issue 10 closed: the scanner detects read-mostly pages

**Verified on hardware 2026-08-20**, zuestoll08, kernel `6.8.0-ltram` `#3 Thu Aug 20 06:22:55 CEST 2026`,
tree `260819_step4_backend-6-g244a66761` (clean), Image sha `5c6524e5…13920a`, bitstream `169_phy200`.

Issue 10 was the blocker: the policy scanned 5.1 M pages per attach and promoted none of the
weights. **It is fixed.** Detection now works, and works with no application cooperation of
any kind.

## What was actually wrong

The scanner asked `pte_dirty(pte)` — *has this page ever been written?*

On arm64 that expands to `pte_sw_dirty || pte_hw_dirty`, and `pte_sw_dirty` is bit 55, set when
the page is first written. Nothing in LtRAM ever clears it: `pte_wrprotect()` **sets** it,
`pte_modify()` preserves it, and `pte_mkclean()` — the only function that clears it — is called
nowhere in this subsystem. This CPU has no hardware dirty-bit management (`ID_AA64MMFR1_EL1`
HAFDBS = 0) and arm64 has no soft-dirty at all, so once the weights were filled in, every one of
the 50,176 pages was permanently ineligible. The re-arm step was working correctly and could not
help, because the bit it needed to influence was not the bit being read.

The fix asks `pte_write(pte)` — *is this page writable right now?* That has an answer which can
change, and one the scanner itself changes by write-protecting the page and waiting to see
whether the program faults.

## The two commits

| commit | what |
|---|---|
| `f558a316f` | **a flash page may never be writable, and may never reach buddy.** Guards `do_wp_page`'s reuse path (forcing the copy, which *is* the demotion), `remove_migration_pte`'s `pte_mkwrite`, and `__folio_put_small`'s free path, plus a `free_pages_prepare` backstop. Adds the `demoted` counter. This is what makes running without `--protect-weights` safe. |
| `244a66761` | **ask whether a page was written, not whether it was ever written.** `written = pte_write(p)` replaces `dirty = pte_dirty(p)`; the re-arm is gated on `written`; counters become `rej_writable` / `rej_runs_short` / `stale_dirty` / `sel_isolated` / `sel_isolate_fail` / `rearmed`. |

`a06b6973c` came first and only added counters — it is what turned a hypothesis into a
measurement, and is worth keeping for that reason alone.

## The measurement

`./issue10-fix.sh` (archived here). Two phases, same binary, same size, same 240 s attach window,
counters sampled before and after and reported as deltas so no reboot is needed between them.
`--protect-weights` differs between the phases and is an **experiment control, not a kernel
hint** — the policy reads PTE state and never looks at VMA flags.

### Phase A — with `--protect-weights`, against the 08-19 diagnostic run

| counter | 08-19 (broken) | 08-20 (fixed) |
|---|---|---|
| `scanned` | 13,258,954 | **110,732** |
| `rej_dirty_ro` → `rej_writable` | 13,247,550 | **41** |
| `stale_dirty` | *(structurally 0)* | **108,999** |
| `rej_runs_short` | 760 | 101,154 |
| `sel_isolated` | 8,384 | 7,456 |
| `sel_isolate_fail` | 2,220 | 2,081 |
| `rearmed` | 40 | 41 |
| `promoted` / `promote_failed` | 0 / 8,384 | 0 / 7,456 |

`41 + 101,154 + 7,456 + 2,081 = 110,732` — the partition closes exactly against `scanned`.

**`stale_dirty` = 108,999, or 98.4% of every page scanned.** That counter increments on exactly
`pte_dirty(p) && !pte_write(p)` — the pages the old rule rejected and this one accepts. It is the
direct measurement of the defect.

### Phase B — no `--protect-weights`, no application hint at all

```
rej_writable      50179     rearmed           50179
rej_runs_short   100360     stale_dirty      108450
sel_isolated       7488     sel_isolate_fail   1627
scanned          159654     demoted               0
```

`50,179 + 100,360 + 7,488 + 1,627 = 159,654` — closes exactly.

**`rej_writable == rearmed == 50,179`, and the weights are exactly 50,176 pages**
(205,520,896 B / 4096, exact; the other three are heap/data). The whole mechanism is visible in
that one equality:

- Pass 1: all 50,176 weight pages are `rw-p` and writable → rejected, and every one write-protected.
- The application never writes them again — it is read-mostly, the kernel simply did not know yet.
- Pass 2 onward: `pte_write()` is false → they read clean → clean runs accumulate → selected.

Armed **exactly once each, never twice**: no weight page was ever re-dirtied. The kernel measured
the read-mostly property instead of being told it.

### The hint bought nothing, which is the point

| | A: `--protect-weights` | B: no hint |
|---|---|---|
| `sel_isolated` | 7,456 | **7,488** |
| `stale_dirty` | 108,999 | 108,450 |
| `scanned` | 110,732 | 159,654 |
| `rearmed` | 41 | 50,179 |
| DIGEST | `eac22204…980e4994` | `eac22204…980e4994` |
| mean | 344.864 s | 344.867 s |

Identical promotion rate. The entire cost of transparency is the `scanned` difference —
159,654 − 110,732 ≈ 48,900, one extra sweep of the weights to arm them — **paid once**. After
that the two paths are indistinguishable. `mprotect` only ever skipped pass 1.

Digest is identical across phase A, phase B and the 08-19 run, so nothing was corrupted, and
344.867 s vs 345.679 s is no regression.

## Two things the numbers say that are NOT wins

**`scanned` collapsing from 13.2 M to 110,732 is expected, not a regression.** `ltram_scan_once()`
calls `walk_page_range(mm, 0, TASK_SIZE, …)` — it restarts at address 0 every pass with no cursor
— and the pte loop is gated on `ctx->nr < promote_batch`. Once 32 pages are selected the rest of
the pass short-circuits and stops counting. Under the old rule nothing was ever selected, so every
pass walked all 13.2 M pages to the end. **A large `scanned` was the symptom of finding nothing.**

110,732 / 233 passes = 475 pages per pass. `/proc/PID/maps` confirms why that is the weights and
not incidental heap: the 196 MiB anonymous region is the *lowest* mapping in the mmap area, with
only ~39 pages of binary and heap below it.

```
c5c2ffc00000-c5c2ffc04000 r-xp  /home/enzian/matmul
c5c33994c000-c5c33996d000 rw-p  [heap]
ff9103a00000-ff910fe00000 rw-p          <-- the weights, 50,176 pages
ff910fe80000-ff911001a000 r-xp  libc.so.6
```

**`promote_batch = 32` at one pass per second caps promotion at 32 pages/s.** `sel_isolated`
7,456 ≈ 32 × 233 in A and 7,488 in B — the batch cap is saturated on every single pass in both
phases. The 50,176 weight pages would take **26 minutes** to drain even with detection perfect.
It is live-writable at `/sys/module/ltram_policy/parameters/promote_batch`, so it is a knob, not a
rebuild. The 32/s default was chosen against the wear budget (41.5 erases/s sustained) and should
only be raised for measurement, not left raised.

## What this run does NOT prove

- **Nothing reached flash.** The `nor_eci` backend was deliberately **not loaded**, so station 3
  of the pipeline was unstaffed: all 7,456 / 7,488 isolated pages found no write path and were
  put back in DRAM intact. `promoted 0` and `promote_failed == sel_isolated` are arithmetic, not
  symptoms. Held out for two reasons — the 08-19 comparison run also had no backend, and changing
  the predicate and the driver together would make the delta unattributable.
- **`demoted 0`, so the guard in `f558a316f` is still unexercised.** It only fires once a page is
  actually on flash. A store to the flash read window is silently discarded by the hardware, so if
  that guard is wrong the failure mode is silent corruption or a hang.
- **`sel_isolate_fail` is 1,627–2,081, ~18–22% of every selection** — `folio_isolate_lru()`
  refusing. Not new and not caused by this fix, but it is a fifth of the promotion budget being
  dropped and it was counted nowhere before `a06b6973c`.

## The next run

Load the backend (`insmod ~/nor_eci/nor_eci_fulltest_ltram.ko provide_ops=1`) and repeat. That
turns `promoted` non-zero and is **the first exercise of the demotion path**. Per HANDOFF's
issue-12 note, attach a console first, and do not `rmmod` the backend while a target pid is set.
