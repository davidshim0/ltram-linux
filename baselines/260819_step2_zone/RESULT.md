# Step 2 — node and zone: PASSED ON HARDWARE

**19 August 2026, zuestoll08.** The first LtRAM gate to pass on real hardware.

| | |
|---|---|
| kernel | `6.8.0-ltram` `#4 SMP PREEMPT_DYNAMIC Wed Aug 19 00:26:09 CEST 2026` |
| version | `260818_step2_zone-6-g81121c699`, branch `step2-debug` |
| commit | `81121c699` — "arm64, arch_numa: give ZONE_LTRAM a range, and register node 1 with NUMA" |
| toolchain | `aarch64-linux-gnu-gcc 11.4.0` |
| bitstream | **irrelevant to this gate** — this kernel touches no flash. No self-test, no write backend, no policy. Pure kernel result. |

## What passed

### The zone exists with the right extent, and buddy owns none of it

```
Node 1, zone    LtRAM
  pages free     0
        spanned  65536
        present  65536
        managed  0
        cma      0
  start_pfn:           335544320
```

`managed 0` is the gate. The 256 MiB is present and the kernel knows its extent, but the
page allocator has no claim on any of it. That is what makes residency enforceable, and
it is why `mm/ltram_policy.c` must own these pages through a private bitmap — the buddy
allocator cannot hand out a flash page even when asked.

### The allocator refuses node 1 rather than quietly serving it

```
$ numactl --membind=1 -- true
  page allocation failure: order:0, mode:GFP_HIGHUSER_MOVABLE, nodemask=1
  __alloc_pages -> warn_alloc -> failure
  numactl: execution of `true': Argument list too long
  exit=1
```

**This is the sharpest result here**, because it is the only one where a wrong answer
would have looked like success. Had `membind=1` succeeded, the process would have been
handed flash pages and every store it made would have been silently discarded.

### Nothing leaked onto flash during boot

```
/sys/kernel/debug/ltram/stray_allocs   0
/sys/kernel/debug/ltram/start_pfn      335544320
/sys/kernel/debug/ltram/end_pfn        335609856
```

Zero, against a gate of "at most 3". The three forcing fixes are visible in the boot log:

```
NUMA: NODE_DATA(1) on node 0                       <- pgdat in DRAM, not on flash
pcpu-alloc: [0] 00 ... [0] 47                      <- all 48 per-CPU areas on node 0
software IO TLB: mapped [mem 0xf9e00000-0xfde00000] <- swiotlb in DRAM
```

Left alone, all three land on the highest-PFN node, which is flash, and every store into
them vanishes.

### The node is real to userspace

```
$ numactl --hardware
available: 2 nodes (0-1)
node 0 size: 128628 MB    node 0 free: 127811 MB
node 1 size: 0 MB         node 1 free: 0 MB
node distances:  0: 10 20 / 1: 20 10

Node 1 MemTotal: 0 kB
```

And the system is healthy: iSCSI root mounted, 48 CPUs, USB enumerated, `free -g` shows
125 GB with node 1 contributing nothing.

## Zone ranges as declared

```
DMA      [mem 0x0000000001400000-0x00000000ffffffff]
Normal   [mem 0x0000000100000000-0x0000013fffffffff]
LtRAM    [mem 0x0000014000000000-0x000001400fffffff]
node 1:  [mem 0x0000014000000000-0x000001400fffffff]
```

`0x14000000000 >> 12` = 335544320. Span = 65536 pages = exactly 256 MiB.

## What this does NOT show

- **Nothing about flash.** This kernel never reads or writes the window. Whether the data
  path works is step 3 onward.
- `Fallback order for Node 1: 1 0` appears in the boot log. That is the *node* order,
  printed regardless; `build_zonelists` skips zones with no managed pages, and `managed 0`
  confirms no LtRAM zone entered a zonelist.

## Carried forward

The failed allocation dumped a full `warn_alloc` backtrace. Correct behaviour, but noisy —
anything reaching node 1 through the buddy allocator will spam the log. The policy
allocator uses its own bitmap and will not hit this path, but a deliberate buddy
allocation there would want `__GFP_NOWARN`.

## Two measurement mistakes worth not repeating

- `grep -A12 'Node 1, zone' /proc/zoneinfo` **cuts off before `managed`** — the per-node
  stats block inserts about 45 lines between the zone header and its counters.
- `awk '/zone +LtRAM/{...}'` matches **node 0's** empty LtRAM zone, which appears first in
  the file, and reports `spanned 0 / present 0 / managed 0`. Anchor on `Node 1,`:

```bash
sed -n '/Node 1, zone *LtRAM/,/^Node /p' /proc/zoneinfo
```

## Files

| file | what |
|---|---|
| `zoneinfo-step2.txt` | full `/proc/zoneinfo` |
| `ltram-zone-step2.txt` | the LtRAM zone block alone |
| `dmesg-step2.txt` | full boot log |
| `numactl-step2.txt` | `numactl --hardware` |
