# Enzian: verify what Linux sees of the FPGA side

These commands check whether the kernel on the ThunderX2 actually believes
there is a second NUMA node, how big it is, where its memory lives, and
which kernel threads are managing it. Run them on the Enzian board after
boot (with whatever bitstream you intend to characterize loaded).

The point of running these in order is to catch the failure modes early:
node 1 declared in DT but with zero pages, or node 1 with pages but no
kswapd, or memory hotplug paths needed because the static DT did not
include the region.


## 1. Does Linux see two NUMA nodes at all?

```
numactl --hardware
```

Expected for a working LtRAM setup:
- "available: 2 nodes (0-1)"
- node 0 cpus: a list of every ThunderX2 core
- node 1 cpus: empty (LtRAM is a CPU-less node)
- node 0 size: ThunderX2 DRAM in MB
- node 1 size: FPGA-side memory in MB
- node distances: a 2x2 matrix; 10 on the diagonal, your declared distance
  off-diagonal

Failure signal: if it says "available: 1 nodes (0)" then either the DT
did not declare node 1, or the kernel was built without NUMA support, or
the LtRAM `memory@...` node was rejected during parsing.


## 2. What is the actual physical address layout?

```
cat /proc/iomem | head -50
```

Look for the physical range claimed for the FPGA side. Compare to what
your bitstream places at that range. If iomem shows the range as "System
RAM" then the kernel believes it is plain memory and node 1's pgdat will
own it. If it shows as a device MMIO region, the kernel will not put
pages on the free lists and LtRAM allocations will fail.

To see per-PFN bookkeeping per node:

```
cat /proc/pagetypeinfo
```


## 3. Are zones set up correctly on node 1?

```
cat /proc/zoneinfo
```

Look for "Node 1, zone ..." blocks. Each should have:
- a nonzero `present` count (the pages physically exist)
- a nonzero `managed` count (the pages were handed to the buddy allocator)
- watermarks min / low / high that are nonzero

Critical failure signal: `present` nonzero but `managed` zero means the
memory was discovered but never made available to the allocator. Usually
fixed by passing the region to memblock_add() and free_area_init() at the
right point in boot. Talk to the Enzian team if you see this.


## 4. Does kswapd1 exist?

```
ps -ef | grep kswapd
```

Expected: kswapd0 and kswapd1, both as kernel threads (PPID 2, name in
square brackets).

Where is kswapd1 running:

```
ps -eo pid,comm,psr | grep kswapd1
```

The `psr` column is the CPU it last ran on. For a CPU-less node 1, this
will be a node-0 CPU. That is expected.


## 5. What is in the DT today?

If you have the running DT exposed (kernel built with CONFIG_OF):

```
ls /sys/firmware/devicetree/base/
```

To find memory nodes:

```
find /sys/firmware/devicetree/base/ -name 'memory@*' -type d
```

For each one, check its numa-node-id and reg:

```
hexdump -C /sys/firmware/devicetree/base/memory@<addr>/reg
hexdump -C /sys/firmware/devicetree/base/memory@<addr>/numa-node-id
```

The `reg` is encoded as a pair of (base, size) cells. The `numa-node-id`
is a single 4-byte big-endian integer; for node 1 you should see
`00 00 00 01`.

To find the distance-map:

```
ls /sys/firmware/devicetree/base/distance-map/ 2>/dev/null || \
  ls /sys/firmware/devicetree/base/ | grep -i distance
hexdump -C /sys/firmware/devicetree/base/distance-map/distance-matrix
```

Each triple in the matrix is (from-node, to-node, distance).


## 6. Are the LRU lists wired up for node 1?

```
grep -A 30 'Node 1' /proc/vmstat
```

Or, more focused:

```
cat /sys/devices/system/node/node1/vmstat
```

Look for nonzero `nr_active_anon`, `nr_inactive_anon`, `nr_active_file`,
`nr_inactive_file`. If they are all zero on a system that is actively
allocating LtRAM pages, the LRU plumbing is not engaging on that node.


## 7. Sanity check: can we actually place a page on node 1?

```
numactl --membind=1 --cpunodebind=0 head -c 1048576 /dev/urandom > /tmp/lt
free
cat /sys/devices/system/node/node1/meminfo | head -10
```

`MemFree` on node 1 should drop by roughly 1 MB. If it does not, the
allocator is not actually putting the pages there.


## 8. If the static DT does not declare LtRAM and you need it at runtime

The hotplug path, assuming kernel has `CONFIG_MEMORY_HOTPLUG=y`:

```
ls /sys/devices/system/memory/
```

You should see `memoryN` directories per memory block, plus a `probe`
file you can echo a physical address into. Document which path Enzian
uses and whether their kernel build enables hotplug.


## 9. Confirm DBM (Dirty Bit Modifier) availability

ThunderX-1 is ARMv8.0, which the Enzian paper confirms. DBM was added
in ARMv8.1. So we expect **DBM to be absent** on Enzian. This check
confirms that expectation against the running silicon, in case any
later board revision changed it.

What the kernel exposes:

```
cat /proc/cpuinfo | head -40
```

Look at the `Features` line. On an ARMv8.0 chip you will NOT see
features that imply v8.1+ such as `atomics` (LSE), `cpuid`, or `lrcpc`.
DBM itself is not a userspace-visible HWCAP, so it does not appear in
the Features line directly. Use the kernel's own report:

```
dmesg | grep -iE 'dbm|dirty bit|HW.AFDBM|cpu features'
```

On a DBM-capable system the kernel logs something like
`CPU features: detected: Hardware dirty bit management`. On
ThunderX-1, that line will be absent.

The kernel's compile-time support is separate from the chip's runtime
support. Check the build:

```
zcat /proc/config.gz 2>/dev/null | grep -E 'AFDBM|HW_DBM' || \
  grep -E 'AFDBM|HW_DBM' /boot/config-$(uname -r) 2>/dev/null
```

`CONFIG_ARM64_HW_AFDBM=y` means the kernel was compiled to use DBM if
the hardware has it. With ThunderX-1, the kernel will fall back to
software-emulated dirty bits regardless of this config flag.

Quick functional probe of cost: trigger a clean-to-dirty transition
and measure. On a DBM-equipped chip this is nanoseconds; on ThunderX-1
it is a page fault.

```
perf stat -e page-faults,minor-faults dd if=/dev/zero of=/tmp/x \
  bs=4096 count=10000 oflag=direct 2>/dev/null
```

If minor-faults scales roughly with count, the system is faulting on
clean-to-dirty transitions (no DBM). If it stays near zero, DBM is
active.

This confirms the design implication: any LRW sampling that depends on
the hardware dirty bit will be expensive on Enzian without FPGA
assistance.


## 10. Open questions to bring to the Enzian team

1. How is FPGA-side memory size communicated to Linux: static DTS,
   U-Boot patches the DTB at boot, or memory hotplug?
2. If U-Boot patches, what register on the FPGA does it read, and where
   in the boot scripts does the patching happen?
3. Are there existing Enzian projects with FPGA-side memory visible to
   Linux that we can use as a precedent?
4. Is the production kernel build configured with CONFIG_NUMA,
   CONFIG_MEMORY_HOTPLUG, CONFIG_MIGRATION, and CONFIG_NUMA_BALANCING?
5. What is the canonical place in the Enzian kernel tree for the base
   DTS we should extend?
