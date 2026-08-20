# 2026-08-20 — kernel panic, asynchronous SError from the coherent fabric

Kernel `6.8.0-ltram #3` (`244a66761`), z08, at kernel time **6352.3** — within two
seconds of the last line the off-box `dmesg -w` stream captured (`t=6350.57`, wall 08:28:20).

## What actually killed it

An **uncorrectable error on the coherent interconnect**, reported first by EDAC and then
escalated to an async SError, which arm64 always turns into a panic.

```
EDAC DEVICE2: UE: thunderx-l2c  L2C-TAD20  L2C_TAD_INT: 0000000000060000
  Uncorrected, LFB entry timeout
  Uncorrected, Global sync CCPI timeout
... the same on TAD1, 3, 4, 5, 6, 8 ...
SError Interrupt on CPU4, code 0x00000000be000000
Kernel panic - not syncing: Asynchronous SError Interrupt
```

**LFB entry timeout** = a cache line fill never returned. **Global sync CCPI timeout** = a
barrier across the coherent interconnect never completed. On Enzian the FPGA sits on that
interconnect, so both say the same thing: *a request to the LtRAM window was issued and no
response ever came back.*

## The trace, and what it is and is not worth

```
ltram_scan_thread -> migrate_pages -> migrate_pages_batch -> move_to_new_folio
  -> buffer_migrate_folio -> __buffer_migrate_folio -> folio_migrate_flags
```

**`__buffer_migrate_folio` is the file-backed page-cache migration path.** The scanner had
no anonymity filter, so it was promoting page-cache folios — and preferring them, because
read-only file text is the most read-mostly memory a process has and sits at the low
addresses the batch fills from first. Fixed in `53fc3a101`.

**The PC is not the fault site.** An asynchronous SError is imprecise: it is delivered
whenever the error propagates back, and reported against whatever the CPU was executing at
that moment. `folio_migrate_flags+0x68` is where CPU4 happened to be. Two other CPUs took
the same SError at unrelated PCs (`wp_page_reuse` in `ltram-writeback`, `move_page_tables`
in `apport`), which is the signature of a fabric-wide failure rather than one bad
instruction. So the trace **names a real bug but does not prove it was the trigger.**

## Our own variables, which must be removed before anything else is suspected

1. **`promote_batch=256`** — eight times the validated default of 32, which I raised purely
   to make the test finish sooner.
2. **The page leak** (`e6f915089`) — a quarter of the window was gone, so every pass was
   issuing hundreds of allocations that failed and migrations that were retried.
3. **Page-cache folios on the migration path** (`53fc3a101`).
4. **Root filesystem at 100%**, with `apport` and `journald` thrashing it during the run.

All four were true simultaneously. Re-run with 1-4 removed, `promote_batch` at its default,
before drawing any conclusion about the hardware.

## Not a reason to reopen the FPGA question

`169_phy200` is golden and settled. Nothing here is evidence against it: the load profile at
the moment of failure was one this project has never applied before, and four of our own
defects were live at the same time.

## Collateral

`pstore: backend (efi_pstore) writing error (-5)` — as always on this machine, so nothing was
captured on the box itself. **The off-box `dmesg -w` stream is the only reason this panic was
recoverable at all.** Start it before every run.

`SMP: failed to stop secondary CPUs 4,14,26` — the panic did not complete cleanly, which is
why the box still answered TCP on port 22 while closing every connection before key exchange.
That behaviour looked like a full-disk sshd failure and was misread as one at the time.
