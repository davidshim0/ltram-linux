# ltram-backend — the NOR write backend, as a test module

`mm/ltram.c` owns the zone and the policy and knows nothing about the FPGA; it calls out
through one function pointer, `struct ltram_flash_ops::write_page`. Until something
registers a backend, `ltram_write_page()` returns `-ENODEV` — which is the safety
property, not a gap.

Nothing in the kernel implements it. This directory holds the FPGA test harness
(`nor_eci_fulltest.c`) extended to supply one, because it already drives the DMA ring.

```bash
# register the backend and nothing else: no worker, no test traffic on the bus
sudo insmod nor_eci_fulltest.ko provide_ops=1
```

## What the shim does

`ft_ltram_write_page(dst_pfn, src)`:

1. maps the pfn to a sector — one page is exactly one sector here, `PAGE_SIZE == SECT_SZ
   == 4096`, and the module's `RD_BASE` is the kernel's `LTRAM_PHYS_BASE`
2. copies `src` into the shared DMA bounce buffer under a mutex
3. sets `chase_fill` so `write_sector()` ships the buffer verbatim instead of generating
   its own pattern
4. **erases, then waits on the FPGA's erase COUNTER** rather than sleeping a fixed time —
   a dropped trigger is invisible to a sleep and has bitten this project before
5. programs, and does not return until the DMA engine reports the full sector

It may sleep (erase ~16.4 ms, DMA ~1.3 ms) and must not return until the data is durably
committed: the caller publishes the migration immediately afterwards with no second
chance to notice a failure.

## Verified 2026-08-19

```
ltram: flash backend registered by nor_eci_fulltest
ltram: write_test pfn 335544520 pattern a5a5a5a5 -> 0
writes_ok 1   writes_failed 1
```

FPGA status word across that single call — the honest witness, because it is not a read:

| counter | before | after | delta |
|---|---|---|---|
| beats | 8192 | 8256 | **+64** |
| pages | 0 | 16 | **+16** |
| erases | 128 | 129 | **+1** |

Exactly the predicted signature for one 4 KiB page.

## Also fixed here: rmmod after a worker oops

`ft_exit()` called `kthread_stop(worker)` unconditionally. On a thread that had already
exited that gives `refcount_t: addition on 0; use-after-free` and then a NULL dereference,
so `rmmod` segfaults and the module can never be unloaded — which on this machine costs a
full reboot. It now stops the worker only if one exists and is alive. Verified: two
consecutive `rmmod` cycles, exit 0, zero refcount warnings.
