# Step 3 — the self-test: PASSED

**19 August 2026, zuestoll08.** The first gate that tests something new rather than
unbreaking something. It could not have passed an hour earlier: `memremap(MEMREMAP_WB)`
resolves through `ioremap_cache()`, which was returning an unmapped address until the
step-2 linear-map fix.

| | |
|---|---|
| kernel | `6.8.0-ltram` `#1 SMP PREEMPT_DYNAMIC Wed Aug 19 13:36:10 CEST 2026` |
| tag | `260819_step3_selftest`, on the FIXED step 2 |
| Image | `aa59322cd774a34d6ce306602a96ef38d599db1d049f30131abfe8417f04a410`, verified on the gateway before boot |
| bitstream | `169_phy200` — `st_wait` 739 us |
| contents | self-test only: no write backend, no policy |

## The gate

```
ltram: self-test sample +0x00000000 = 00000011
ltram: self-test sample +0x0000400c = 0000401d
ltram: self-test sample +0x00008018 = 00008009
ltram: self-test sample +0x0000c024 = 0000c035
ltram: self-test read 1024 words, stride 4099: erased(ff) 0, zero 0, checksum 099708ea
ltram: self-test completed without abort -- window is readable from kernel context
```

The last line existing at all is the gate. **No aborts anywhere in the boot.**

### The samples prove more than the gate asked for

The self-test only claims the window is *readable*. These samples show it read the
**correct data at the correct offsets**. The harness had written sectors 0-63 with the
address stamp `word = offset ^ seed`, seed `0x11`:

| offset | expected `offset ^ 0x11` | read |
|---|---|---|
| `0x00000000` | `0x00000011` | `00000011` |
| `0x0000400c` | `0x0000401d` | `0000401d` |
| `0x00008018` | `0x00008009` | `00008009` |
| `0x0000c024` | `0x0000c035` | `0000c035` |

That is **two independent code paths agreeing on flash content** — the module's
`ioremap_cache()` wrote it, the kernel's `memremap()` read it back. Neither could reach
the window before the fix.

`erased(ff) 0, zero 0` is expected: the device has been written across the whole address
space by past campaigns, so a walk finds data rather than erased words.

`checksum 099708ea` is a fingerprint for future comparison, not a pass criterion. The
seed is per sector, so asserting one seed across the device would report mismatches on
healthy hardware; the test reports rather than asserts, by design.

### Why stride 4099 words

Coprime with both the 4 KiB sector and the 128 B cache line, so the walk crosses
boundaries instead of sitting in one region. The known read-side capture fault appears
only on the **first word of a line**; a test that never lands there cannot see it.

### Why late_initcall

Reading before the FPGA is programmed and the ECI link is up is an external abort, and an
abort there is a dead boot with nothing after it on the console.

## Gate 2 still holds on this kernel

| check | result |
|---|---|
| zone | `spanned 65536` `present 65536` `managed 0` |
| boot leak | `stray_allocs 0` |
| allocator refuses node 1 | `membind=1` exit 1 |
| window reachable | harness: 64 sectors erased+written+verified, `bad=0` |
| faults in dmesg | **0** |

```
CENSUS dma-write   : 64 issued | us min/avg/max 1320/1326/1354
CENSUS status-word : bytes=4096 beats=8192 erases=128
CENSUS st_wait     : 64 waits | us avg/max 739/767
PHASED SUMMARY: erase_fail=0 ff_bad=0 wr_timeout=0 data_bad=0 aborted=0
```

## Not yet shown

Nothing about writing from kernel context. The self-test reads only, because the CPU
cannot store to the read window and no backend is registered until step 4. That limit is
the hardware, not an omission.
