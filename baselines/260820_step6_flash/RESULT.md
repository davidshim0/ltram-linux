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
| `writes_ok` | 17,128, climbing monotonically from 0 |
| `promote_failed` | **14,944, unchanged** — zero failures once the backend was live |
| `demoted` | 279 — the new guard fired and the machine kept running |
| digest | matched the DRAM control on every one of 15 runs |

**Steady-state cost with the whole region on flash: 31.3 s against a 6.432 s DRAM control —
4.87×.** That is *better* than the 6.0× random / 7.8× sequential media ratio measured in
§0.0, because the L2 still absorbs part of the stream.

The per-run curve separates migration cost from residency cost, which a single before/after
number cannot:

```
run  1   29.333 s     <- migrating
run  2   54.394 s
run  3   76.803 s
run  4   96.499 s
run  5  130.782 s     <- peak: migration and flash reads competing
...
run 11   31.285 s     <- promotion complete, steady state
run 12   31.326 s
run 13   31.345 s
```

The digest is re-checked **between every run** (exit 44 on mismatch), so runs 11-15 are each an
independent readback test of pages living on flash. All passed.

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
