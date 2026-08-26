# 260826_step1c_dirty — the DIRTY state and the erase worker, on hardware

**Verified 2026-08-26**, zuestoll08, kernel `6.8.0-ltram` (`602f3499c`), bitstream
`169_phy200`. Backend rebuilt against this kernel; `erase_page`/`device_idle` deliberately
**not** implemented, so the state machine is validated before anything can erase.

## Two results

### The scan cursor works — first direct evidence

`check1a.sh`, no backend:

```
sweeps         1                    the cursor completed a sweep and wrapped
scan_cursor    0xf89666fbe000       advanced high into the address space
ptes_examined  5599   vs  chosen 1479          ratio 3.8x
```

Before the cursor that ratio was **1507 / 1504 — essentially 1:1**, because the walk restarted
at address 0 every pass and immediately re-hit the same eligible pages. At 3.8x it is walking
past regions and finding candidates spread across the address space, which is the whole point.

Two changes made this observable at all: `target_pid_store()` no longer resets the cursor on
DETACH (only when a different target arrives), and `--protect-weights` was dropped so the
transparent arm-then-promote cycle actually runs.

### Station 4 passes with the DIRTY state live

Same `station4.sh`, now with no application hint and `promote_batch=32`:

| | |
|---|---|
| digest | **`8873ba56…c23302` on both control and flash** |
| `moved_to_ltram` | 6,599 |
| `moved_to_dram` | 3 |
| control / flash mean | 6.444 s / 13.136 s (ramping — only ~40% migrated) |

**The digest is the load-bearing result.** If the `was_programmed` split were inverted, a used
sector would have gone back to FREE unerased and been handed out still holding the previous
tenant's data. It matched.

## The transition, captured

A sampler ran alongside, because the interesting moment is process exit -- ~6.5k pages move
VALID -> DIRTY at once and a single snapshot would miss it:

```
 t     valid  dirty   free
265     6532      3  59001
270        8   6591  58937     <- process exit
275        8   6591  58937
```

`8 + 6591 + 58937 = 65536`, exact, and `free_from_bucketlist` still equals `free`.

## Lazy erase is correct, and it invalidates an assertion we had

DIRTY does **not** drain, and should not: `free` is 58,937, far above `erase_high_water`
(8,192), so the worker deliberately does nothing. Spending erases on sectors nobody needs yet
is pure wear.

So **"free returns to 65,536" is the wrong success criterion** once a backend is loaded. The
right one is the invariant summing to 65,536. `check1a.sh` still checks the old thing, which is
harmless only because it runs with no backend and therefore never populates DIRTY.

## An 8-page leak — OPEN

```
moved_to_ltram      6599
freed_via_backstop  6591      difference: exactly 8
valid                  8      persisting with no owner process
```

8 of 6,599 promoted pages (0.12%) were never returned, and they are still VALID long after
matmul exited. Not urgent, but it is the class of bug that compounds, and the next question is
whether it accumulates across runs — 8 becoming 16 would settle it.

## One thing this confirms about the free path

**All 6,591 frees went through the backstop; none through `__folio_put_small()`.** Process exit
uses the batch path exclusively — the same path that lost 16,502 pages before `e6f915089`
hooked `free_pages_prepare()`. The catch-all is not a belt-and-braces addition, it is the only
path that matters at exit.

## Not established

- `erases_done 0` throughout: the backend has no `erase_page` op. The worker, the watermark and
  the device-idle gate are all **compiled and unexercised**.
- Steady state was not reached — only ~6,599 of 16,384 pages migrated in the window, so the
  13.136 s mean is a ramp, not a plateau.
