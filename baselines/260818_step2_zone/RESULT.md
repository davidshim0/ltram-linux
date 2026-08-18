# Step 2 — NOR window declared as NUMA node 1 and ZONE_LTRAM

**Built 2026-08-18. NOT YET BOOTED** — the hardware gates below are still open.

| | |
|---|---|
| kernel release | `6.8.0-ltram` |
| git tag | `step2-zone-2026-08-18` → `11cb403f48` on branch `ltram-arm64` |
| repo | `ba8:/local/home/hushim/ltram-policy-bench/linux` |
| base | mainline v6.8 (`e8f897f4af`) — same commit as the vanilla baseline |
| toolchain | `aarch64-linux-gnu-gcc-11` |
| pairs with | bitstream `169_phy200` |

## What it changes

Injects the window at `0x14000000000` (256 MiB, 65,536 pages) as node 1 with a dedicated
`ZONE_LTRAM`, and guarantees nothing is allocated from it.

**Residency is enforced by construction.** `build_zonerefs_node()` never puts ZONE_LTRAM
in a zonelist; the allocator reaches zones only through zonelists, so a zone in none of
them is unreachable — including from code written later, which a gfp-mask test would not
cover. A `WARN_ONCE` in `get_page_from_freelist()` catches anything arriving anyway.

**Three paths would have put kernel state on flash by default**, and each fails silently
because a store to this window is discarded with no fault:

| path | why it would land on NOR | fix |
|---|---|---|
| `mm/memblock.c` | boot allocators pass `NUMA_NO_NODE`; memblock searches **top-down** and the window is the **highest-PFN** range, so per-CPU areas, swiotlb, the qspinlock hash and CPU-entry page tables all default there | redirect `NUMA_NO_NODE` → node 0 |
| `mm/sparse.c` | section usemap and the ~4 MiB `struct page` memmap follow the node they describe | force to node 0 |
| `drivers/base/arch_numa.c` | the node's own `pg_data_t` follows the node | force to node 0 |

Node 1's metadata living on node 0 looks wrong and is deliberate.

## Verified at build time

**`CONFIG_LTRAM=n` is inert.** Zero ltram symbols; the only difference from the vanilla
baseline is four initcall names whose encoded source line numbers moved because comments
shifted. That is what makes the A/B measurement meaningful — one config symbol, not two
kernels.

That check earned its keep twice:
- `IS_ENABLED(CONFIG_LTRAM)` still *compiles* its branch, so naming `ZONE_LTRAM` inside
  one failed to build with the option off. Preprocessor guards are required wherever the
  enum value appears.
- Adding a zone widens `ZONES_SHIFT` and **overflows `GFP_ZONE_TABLE`** — 2^4 x 3 = 48
  fits in 64 bits, 2^5 x 3 = 96 does not. The compiler reports this only as a shift-count
  warning from inside a macro, which is easy to miss and wrong at runtime. `ZONE_LTRAM` is
  excluded from the table exactly as `ZONE_DEVICE` already is; nothing is lost, since no
  gfp mask can select a zone that is in no zonelist.

**`CONFIG_LTRAM=y` builds** with 7 ltram symbols and a clean `6.8.0-ltram` release string.

## Hardware gates — still to run

1. `numactl -H` shows node 1 at **256 MB**
2. `/proc/zoneinfo` shows `Node 1, zone LtRAM` with `present 65536` and `managed 0`
3. **boot leak** `present - managed - free` must be **<= 3 pages**. The x86 line measured
   204 before the `NUMA_NO_NODE` redirect and 3 after; the residual three are zone-init
   bookkeeping. Anything higher means a boot allocator still reaches flash.
4. `dmesg | grep ltram` — the declaration line, and **no** stray-allocation warnings
5. read-back: `nortest census` before rebooting lays down the address-stamp pattern; after
   boot the same offsets read through the node must match `PAT(off) ^ seed`

## Deploying

```bash
scp Image      hushim@enzian-gateway.inf.ethz.ch:/srv/tftp/userkernels/hushim/vmlinuz
scp initrd.img hushim@enzian-gateway.inf.ethz.ch:/srv/tftp/userkernels/hushim/initrd.img
emg release zuestoll08
emg acquire -n ltram -g 2025-07-28 -k hushim/vmlinuz -i hushim/initrd.img zuestoll08
```
Modules for `6.8.0-ltram` are **already installed on z08** and the initrd was generated
there, so only the two files above need publishing.

**Falling back:** `baselines/vanilla68-2026-08-18/` boots the validated kernel. Its module
tree was removed from z08 to make room, but that does not stop it booting — the NIC and
iSCSI drivers are `=y`, so it reaches its iSCSI root with no modules at all.
