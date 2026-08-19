# Step 2 with the linear-map fix — PASSED, including the sub-check gate 2 was missing

**19 August 2026, zuestoll08.** Step 2 is now solid: the zone is declared, the allocator
cannot touch it, and — new — **kernel code can actually reach the window**.

| | |
|---|---|
| kernel | `6.8.0-ltram` `#1 SMP PREEMPT_DYNAMIC Wed Aug 19 07:52:06 CEST 2026` |
| branch | `step2-fix` = `260819_step2_zone` + one fix commit. **Step 2 only** — no self-test, no backend, no policy (11 ltram symbols) |
| Image | `cea9ee4a678fd96dea7a09fcafb89f7cf75d3579e96d66fd66b9be56e9084a6b`, verified byte-identical on the gateway before boot |
| bitstream | **`169_phy200`, confirmed by `st_wait` 738 us** (179/t51 would be ~1329) |

## The fix

The window was registered as memory but never mapped, so `ioremap_cache()` returned an
unmapped `__phys_to_virt()` address, `ioremap()` returned NULL, and
`memremap(MEMREMAP_WB)` followed `ioremap_cache()`. First touch was a level-0 translation
fault. **Two causes, not one:**

1. `should_skip_region()` backs **both** `for_each_free_mem_range()` (allocation) and
   `for_each_mem_range()`, which `map_mem()` walks to build the linear map. The range
   filter there kept boot allocations off flash *and* kept the window from ever being
   mapped. Moving the declaration earlier alone would not have helped.

   Replaced with `memblock_reserve()`. Reservation is subtracted from the allocation
   iterator and skipped by `memblock_free_all()`, but plain memory iteration still sees
   it — exactly the distinction needed.

2. `ltram_declare_node()` ran from `bootmem_init()`, after `paging_init()`. Moved to the
   end of `arm64_memblock_init()`, after `high_memory` is computed so
   `memblock_end_of_DRAM()` does not push it past the window.

## Results

```
ltram: node 1 = 0x14000000000 + 256 MiB, pfn 335544320..335609856 (65536 pages), reserved
mempolicy: Disabling automatic NUMA balancing.
NUMA: NODE_DATA(1) on node 0
```

| check | result |
|---|---|
| zone | `spanned 65536` `present 65536` **`managed 0`** — the reservation preserved it |
| boot leak | `stray_allocs 0` |
| pfn range | 335544320 .. 335609856, matches the boot line |
| allocator refuses node 1 | `numactl --membind=1` exit 1; node 1 size 0 MB |
| **window reachable from kernel** | **module loaded, no oops** |
| NUMA balancing | **off at boot**, by config, not by sysctl |

### The new sub-check did more than prove the mapping

```
WCOV3 PASS 0 done: erase bad=0  write bad=0  (64 sectors) clean
dma-write   : 64 issued | us min/avg/max 1321/1325/1365
status-word : bytes=4096 beats=4096 pages=0 erases=64
st_wait     : 64 waits | us avg/max 738/778
```

64 sectors erased, written, and verified word by word — **65,536 words read back correctly
through the new linear-map address**, the exact operation that took a translation fault
before the fix. The FPGA counters moved, so the traffic genuinely reached the hardware.

`managed 0` surviving is the load-bearing result. It is the property the whole residency
model rests on and the one most at risk from swapping an iteration filter for a
reservation.

## Reading the harness output

`ERASE-1st @+0x0 got=00000000` and `WRITE-1st @+0x0 got=00000000` look alarming and are
not. Those fields are only populated **when the corresponding check fails**; with
`erase ok` / `write ok` they were never assigned. `0 FF` is likewise correct — it counts
words still reading `0xFFFFFFFF` *after* a write, which should be zero.

`CENSUS sector-read: 0 issued` is bookkeeping: WCOV3 verifies inline with
`readl(rd_win + b)` rather than through the counted read path. The reads happened.

## Reversibility

- **branch** — `main` and all tags untouched; `260819_step2_zone` is the pre-fix kernel
- **gateway** — `vmlinuz.prev-step2` = `a196b3f3...`, the kernel that was running
- **runtime** — `ltram=off` on the command line skips the declaration entirely; the image
  boots equivalently to `CONFIG_LTRAM=n`

## A flaw in the test script, fixed

`gate2.sh` ran `dmesg -C` before the insmod to isolate module output, and destroyed the
boot log with it — the evidence section E exists to capture. Recovered here from
`journalctl -k -b`. The script now filters instead of clearing.
