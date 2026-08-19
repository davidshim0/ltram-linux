# Step 4 — the write backend: REFUSAL HALF VERIFIED, write path NOT YET TESTABLE

**19 August 2026, zuestoll08.** Kernel `6.8.0-ltram` `#1 ... Wed Aug 19 13:54:31`,
tag `260819_step4_backend`, Image `ef4700a1d7e629831c89ebb74a908b6b076dd4b4e1796348e86c56499d2bc2c5`,
bitstream `169_phy200`.

## What passed

Every interface exists, and the refusal works:

```
/sys/kernel/debug/ltram/  ->  start_pfn  end_pfn  stray_allocs
                              writes_ok  writes_failed  write_test

echo "335544420 a5a5a5a5" > write_test
  exit 1
  ltram: write_test pfn 335544420 pattern a5a5a5a5 -> -19     (-19 = -ENODEV)

writes_ok      0        (unchanged)
writes_failed  1
```

**This is the property the subsystem exists for.** With no backend registered the write is
refused, the failure is counted, and `writes_ok` stays zero — a caller can never mistake
"nothing wrote it" for success. On this hardware a discarded store is the *default*
outcome of getting it wrong, so the refusal is the whole safety story.

Earlier gates still hold on this kernel:

| check | result |
|---|---|
| self-test (step 3) | passes, checksum `099708ea` — **identical to the step 3 boot**, so it is deterministic |
| zone | `spanned 65536` `present 65536` `managed 0` |
| allocator refuses node 1 | `membind=1` exit 1 |
| faults in dmesg | 0 |

## What is NOT verified, and why

**No page has been programmed through this interface.** Nothing in this kernel implements
`write_page()` — the NOR driver that would call `ltram_register_flash_ops()` does not
exist. So the positive half of the gate is untestable as things stand:

- a page actually programmed through the driver
- read back three ways: kernel read with the cache evicted, userspace via the harness, and
  the **FPGA status word** (`beats +64, pages +16, erases +1`)

The status word is the honest witness and is not a read at all: a write that silently did
nothing leaves correct-looking cached data and zero counter movement.

Also untested: `try_module_get()` pinning the provider across an `rmmod` mid-write, since
there is no provider to pin.

## What closing it needs

The harness module already drives the DMA ring — `write_sector()` does the descriptor
store and completion wait that `write_page()` needs. Binding them means implementing
`struct ltram_flash_ops` in the harness and registering it on load.
`ltram_write_page` and `ltram_copy_to_flash` are already `EXPORT_SYMBOL`'d, so no kernel
change is required.

That shim is also what step 5 needs: without a registered backend there is nothing for a
migration to write *into*.
