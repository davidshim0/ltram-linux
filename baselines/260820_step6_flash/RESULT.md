# 260820_step6_flash — pages reach NOR and read back correctly (station 4)

**Partially verified on hardware 2026-08-20**, zuestoll08, kernel `6.8.0-ltram` `#3`,
tree `244a66761`, bitstream `169_phy200` (`STATUS WORD` at module load, `st_wait` fingerprint).

Station 4 **PASSED**. The run that followed it exposed a real bug — a flash page leak — which
took the machine's root filesystem with it. Both are recorded here; the second is the more
useful finding.

## Station 4 — PASS

`matmul --n 4096 --iters 60 --runs 15 --verify --protect-weights --phys`, 64 MiB of weights
= 16,384 pages, `promote_batch=256`, backend loaded with `provide_ops=1 test=0`.

| | |
|---|---|
| weight pages resident on NOR | **16,384 / 16,384 (100%)** |
| result buffer on NOR | **0 / 4** — correctly never promoted |
| `promoted` | 17,249 |
| `writes_ok` | 17,249 — exactly one flash write per promoted page |
| `promote_failed` | **unchanged at 14,944** — every isolation succeeded, zero failures |
| `sel_isolate_fail` | 2,628 of 19,877 selections (13.2%) refused by `folio_isolate_lru()` |
| `demoted` | 380 — the new guard fired repeatedly and the machine kept running |
| digest | **`8873ba56…c23302`, identical to the DRAM control** |

**Steady state with the whole region on flash: 31.299 s against a 6.432 s DRAM control —
4.87x**, with 0.08% run-to-run spread. That is *better* than the 6.0x random / 7.8x sequential
media ratio in §0.0, because L2 still absorbs part of the stream.

The per-run curve separates migration cost from residency cost, which a single before/after
number cannot:

```
run  1   29.333 s   <- migrating          run  9   31.286 s
run  2   54.394 s                         run 10   31.279 s
run  3   76.803 s                         run 11   31.285 s
run  4   96.499 s                         run 12   31.326 s
run  5  130.782 s   <- peak: migration    run 13   31.345 s
run  6   39.735 s   <- promotion done     run 14   31.288 s
run  7   31.995 s                         run 15   31.283 s
run  8   31.538 s                              control 6.432 s
```

**Read `RESULT mean 47.345 s sd 64.28%` and matmul's own `GATE FAIL` in `p1.log` correctly.**
That gate exists to certify a *control* measurement, and it is right to reject this one: the
run deliberately transitions from DRAM to flash partway through, so its mean averages two
different machines. The steady state is runs 9-15, and that is what 31.299 s refers to.

The digest is re-checked **between every run** (matmul exits 44 on mismatch), so runs 6-15 are
each an independent readback test of pages living on flash. All passed.

## IMPORTANT — how to read the run above

That run was **correct, but for the wrong reason.** It passed because a page leak meant no
flash sector was ever reused, so every promotion landed on a virgin sector. A second bug
(stale CPU cache lines on sector reuse, `529197685`) was latent underneath it and could not
fire. Do not cite the 15/15 digest match from that run as evidence the write path is sound;
cite the re-run below.

## The re-run that actually proves it — 2026-08-20 16:24, kernel #5

Same workload, `promote_batch=32` (the validated default this time), with the leak fixed, the
anon-only filter in, and `inval_sector()` on the write path.

| | |
|---|---|
| digest | **`8873ba56…c23302` — identical to the DRAM control, 15/15 runs** |
| `promoted` / `writes_ok` | 6,784 / 6,784 — exactly one flash write per page |
| `promote_failed` / `writes_failed` | **0 / 0** |
| `late_free` | 6,779 — flash pages returned via the batch free path at process exit |
| `pages_in_use` after exit | 1 — the leak is gone |
| `rej_not_anon` | 848 file-backed folios correctly refused |
| `sel_isolate_fail` | 208 of 6,992 (3.0%) |
| `demoted` | 7 |

**`late_free` is not evidence of sector reuse, and an earlier version of this file said it
was.** It counts flash pages that reached `free_pages_prepare()` -- the generic buddy funnel --
instead of being caught by the `__folio_put_small()` hook. `release_pages()` and
`free_unref_page_list()` bypass that hook, and process exit and `munmap` both use them, so a
process holding thousands of mapped flash pages frees essentially all of them that way. It is
the normal path, and `late_free` ~ `promoted` at exit is the leak fix working.

**No sector was reused in either run.** `pages_in_use` rises monotonically 0 -> 6,778 and never
drops, and `stats.after` was captured after matmul had already exited. The same is true of the
failing run. So reuse cannot be the reason the digest broke at run 12.

**What the A/B does establish:** `inval_sector()` was the only difference between the failing
and passing runs, so stale CPU cache lines remain the mechanism. The likeliest source is not
reuse but **hardware prefetch**: the allocator hands out sectors ascending and contiguous
(`find_first_bit`), matmul reads the weights sequentially, so reading resident flash pages pulls
adjacent *allocated-but-not-yet-written* sectors into cache; the FPGA then DMAs them without
invalidating anything, and the first read after mapping is served stale. That accounts for the
corruption appearing only once enough contiguous pages are resident. **This is a hypothesis
consistent with the evidence, not a proven mechanism** -- the fix is validated, the explanation
is not.

**n = 1.** One clean 15-run pass against one failure at run 12 of 15 is suggestive, not
conclusive.

**This run does not reach steady state.** At `promote_batch=32` only ~3,514 pages (21%) had
migrated by run 15, so the 13.766 s mean is a ramp, not a plateau, and it is not comparable to
the 31.299 s figure above. Timings climbed 9.659 → 18.406 s against a 6.435 s control. A
steady-state number needs either more runs or a higher batch, and belongs to step 7.

## The bug this run found — a flash page leak

`free_pages_prepare()` reported **16,502 stray LtRAM pages**, a quarter of the window.

The guard added in `f558a316f` hooks `__folio_put_small()`, which sees only single-page puts.
`release_pages()` and `free_unref_page_list()` — the batch path that **process exit and munmap
take** — bypass it entirely. Those pages hit the backstop, were correctly kept out of buddy,
and were then dropped: nothing returned them to the bitmap.

Fixed in `e6f915089` by returning the page at the backstop itself. `free_pages_prepare()` is
the single funnel every free path in the kernel passes through, so hooking it covers all of
them **by construction** rather than by enumerating call sites — which is the mistake the
first version made. The allocator lock becomes `irqsave` because that path can reach it from
softirq with interrupts already off. Recoveries are now counted as `late_free` instead of
printed per page.

## The cascade, worth not repeating

1. The leak printed one line per page: 16,502 `pr_info` calls.
2. z08's root filesystem was already **100% full with 14 MB free** (BASELINES.md warns about
   this; it is a 4.4 GB root and `/var/log` alone held 180 MB with 145 MB of journals).
3. journald wrote the flood to the last free megabytes.
4. `sshd` then accepted connections and closed them before key exchange —
   `kex_exchange_identification: Connection closed by remote host` — because it could no
   longer fork a session.

The kernel stayed up throughout: the pre-existing `dmesg -w` stream held its connection with
30 s keepalives long after new logins stopped working. **Streaming the kernel log off-box
before starting is what made the diagnosis possible**, and is worth keeping as standard
practice on this machine.

**Use `/scratch` for run output, never `$HOME`.** It is NFS with 507 GB, and it survives a
reset of the board. z08's root cannot absorb even a modest log.

## Not established

- **Capacity behaviour (324 MiB into a 256 MiB window) — contaminated, must be re-run.** The
  leak had already consumed a quarter of the window, so "full" would have been reached for the
  wrong reason. The refusal path is the same code either way, but the measurement is not clean.
- **Writeback / demotion — not reached.** `demoted 279` shows the path executes without
  crashing, but nothing has yet written to a flash-resident page and verified the data came
  back. `ltram-writeback` is built and staged for exactly this and has never run.
- `sel_isolate_fail` still drops 18-22% of selections, unexplained.

---

# Station 6 — the fault path back to DRAM — PASS, 2026-08-20

**The hazard this closes.** A store to the NOR read window is silently discarded by the
hardware: no fault, no error, no signal. If a flash-backed page is ever left writable, the
write evaporates and nothing notices. Every test before this one only ever *read*, and
`--protect-weights` turned a stray write into a `SIGSEGV`, so this path had never run against
real data.

`ltram-writeback` fills 32 MiB with pattern A, stops writing, gets promoted **by only reading
it**, then writes pattern B over every page and verifies. The pattern is position-dependent
(`idx * 0x9E3779B97F4A7C15 ^ epoch`), so a page restored from the *wrong* sector fails too,
not merely one that never took the write.

| | |
|---|---|
| resident on NOR when the write was released | **7,517 / 8,192 (91.8%)** |
| `STALE` — words still pattern A | **0** — no write was discarded |
| `WRONG` — words neither A nor B | **0** — no corruption, no misplaced page |
| `demoted` | 13 → 7,532, **delta 7,519 against 7,517 resident** |
| write | 8,192 pages in 0.316 s (101.2 MiB/s), including 7,517 faults and copies out of NOR |
| `promote_failed` | 0 |

**`demoted` delta ≈ resident pages is the load-bearing number.** One demotion per
flash-resident page means `do_wp_page()` refused to reuse every one of them and fell through to
`wp_page_copy()`, which is the demotion. Had the guard in `f558a316f` been wrong for even a
handful, those pages would have been reused in place, their stores swallowed, and `STALE` would
be non-zero.

## Two things this run also shows, for free

**Transparent detection, independently, on a program that is not matmul.**
`rej_writable` and `rearmed` both move by **8,211** — equal, against 8,192 pages. Pass 1
rejects every writable page and arms it exactly once; the program then only reads; later passes
find them clean and promote. The region is **`rw` for the entire run** — no `mprotect`, no
hints, and it *has* to be, because the test writes to it at the end.

**The two free paths are genuinely distinct.** `late_free` moved by only **7** across this
whole run, against 7,519 demotions. Demotion frees its flash page through
`folio_put()` → `__folio_put_small()`, which the early hook catches; the backstop in
`free_pages_prepare()` sees almost none of it. The batch path that dominated station 4
(`late_free` ≈ `promoted` at process exit) and the single-page path exercised here are
different routes, and both are now covered.

## Method note

The scanner is detached two seconds *before* the write is released. Otherwise it could
re-promote pages mid-write, and a genuine writeback failure would be indistinguishable from a
page moved back under the writer.

**n = 1**, one region, one size. Not yet run: a write to a page that is *concurrently* being
promoted, which is the race this ordering deliberately avoids rather than tests.
