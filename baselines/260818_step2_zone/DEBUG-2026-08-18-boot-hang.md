# Step 2 hung before any console existed — what it was, and how it was found

**2026-08-18.** The first attempt to boot `260818_step2_zone` on zuestoll08 stopped dead
after the EFI stub, with nothing on the console at all. This is the record of the hunt,
kept because the diagnostic technique is reusable and the root cause was not where the
first guess put it.

## The symptom

```
EFI stub: Booting Linux Kernel...
EFI stub: ERROR: FIRMWARE BUG: Image BSS overlaps adjacent EFI memory region
EFI stub: Using DTB from configuration table
EFI stub: Exiting boot services...
<nothing, ever>
```

Adding `earlycon nokaslr ignore_loglevel` at the GRUB prompt changed nothing. Still silent.

## The move that made it diagnosable: boot vanilla with the SAME arguments

Silence is only evidence if you know the console works. Booting
`260818_step0_vanilla68` with the identical command line answered that:

```
[    0.000000] Booting Linux on physical CPU 0x0000000000 [0x431f0a11]
[    0.000000] Machine model: Enzian
[    0.000000] earlycon: pl11 at MMIO 0x000087e024000000 (options '115200n8')
[    0.000000] printk: legacy bootconsole [pl11] enabled
```

The full log is `baselines/260818_step0_vanilla68/vanilla-boot.log` — the first boot log
archived for any baseline. **Do this first, every time.** It cost one boot cycle and
turned "no information" into an exact bracket.

## The reusable diagnostic

`earlycon` registers in `parse_early_param()`, and printk buffers everything before that
and flushes the instant a console appears. So on this platform:

> **A completely silent boot means the kernel died before `arch/arm64/kernel/setup.c:318`.**
> Any failure after that point prints the banner first.

The map, from `setup_arch()`:

| line | call | note |
|---|---|---|
| 308 | `early_fixmap_init()` | |
| 311 | `setup_machine_fdt()` | prints `Machine model: Enzian`; does top-down memblock work |
| **318** | **`parse_early_param()`** | **earlycon registers; the buffer flushes here** |
| 345 | `arm64_memblock_init()` | |
| 347 | `paging_init()` | |
| 357 | `bootmem_init()` | `ltram_declare_node()` is called from the top of this |

Step 2 printed nothing, so it died before 318 — which means `ltram_declare_node()` never
ran, and neither did any change in `sparse.c`, `mm_init.c` or `page_alloc.c`. By
elimination the only step-2 code that executes that early is the `memblock.c` patch.

## The root cause

`__next_mem_range_rev()` rewrote **every** `NUMA_NO_NODE` request to node 0:

```c
if (nid == NUMA_NO_NODE)
        nid = 0;
```

But `memblock_add()` — the path arm64 uses for system RAM from the EFI map and the DT —
tags regions with `MAX_NUMNODES`, and real node IDs are only assigned later, by
`memblock_set_node()` inside `numa_init()`, which runs in `bootmem_init()` at line 357.

So `should_skip_region()`'s

```c
if (nid != NUMA_NO_NODE && nid != m_nid)
        return true;
```

saw `nid == 0` against `m_nid == MAX_NUMNODES` and **skipped every region**. Every
top-down memblock allocation failed, starting with the ones `setup_machine_fdt()` makes
at line 311 — seven lines before a console could have reported it.

## The fix

Skip the LtRAM **range** instead of rewriting the request. It is the direct expression of
the intent — *a `NUMA_NO_NODE` request must never be answered with flash* — and it has no
ordering dependency, because `ltram_end_pfn` is 0 until the window is declared:

```c
if (nid != LTRAM_NUMA_NODE && ltram_end_pfn &&
    PFN_DOWN(m->base) >= ltram_start_pfn &&
    PFN_DOWN(m->base) < ltram_end_pfn)
        return true;
```

Region `nid` semantics are left exactly as upstream, so `MAX_NUMNODES`-tagged regions
behave normally and everything before `bootmem_init()` is bit-identical to a kernel
without LtRAM. `for_each_mem_pfn_range()` does not go through `should_skip_region()`, so
zone and memmap setup still see the LtRAM region.

Branch `step2-debug`, off tag `260818_step2_zone`:

| commit | what |
|---|---|
| `6f39baa34` | build-identity cherry-picked from `4c3022102` (scripts only) |
| `46f939ab1` | the memblock fix, plus `ltram-dbg:` prints across `bootmem_init()` |

## Platform facts worth not re-deriving

| fact | value | where from |
|---|---|---|
| UART | `0x87e024000000`, pl011, 115200n8 | vanilla boot log |
| DT console path | `chosen/stdout-path = serial0:115200n8` | z08 sysfs |
| explicit earlycon, if the DT path ever fails | `earlycon=pl011,mmio32,0x87e024000000` | derived |
| DRAM top | `Normal [mem 0x100000000-0x1fffffffff]` — 128 GiB | vanilla boot log |
| LtRAM window | `0x14000000000` — 1.25 TiB, far above DRAM | by design |
| `max_pfn` after declaration | `0x2000000` → `0x14010000`, a 10x expansion | derived |
| `FIRMWARE BUG: Image BSS overlaps...` | **pre-existing**, vanilla prints it too | vanilla console |

## Two process notes

**GRUB command-line edits do not persist.** `earlycon nokaslr ignore_loglevel` has to be
re-entered at the menu on every boot: highlight the `zuestoll08 with root ...` entry
(not `Reboot`), `e`, Ctrl-E to the end of the `linux` line, append, Ctrl-X.

**z08 netboots, so no `emg release`/`acquire` is needed to change kernels.** Publish the
Image to the gateway and reboot with `echo b > /proc/sysrq-trigger`. Never `sudo reboot`:
iSCSI tears down before root unmounts, systemd freezes, and the BMC cycle that recovers
it wipes the FPGA.
