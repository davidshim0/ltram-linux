// SPDX-License-Identifier: GPL-2.0-only
/*
 * LtRAM core — declare the coherent NOR window as a NUMA node and a memory
 * zone, and account for anything that reaches it when it should not.
 *
 * The zone is kept out of every zonelist (see build_zonerefs_node), so the page
 * allocator cannot reach it at all. Pages arrive by migration only. This file
 * owns the range, the boot-time declaration, and the counters that make a
 * violation visible instead of silent.
 */
#include <linux/ltram.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/nodemask.h>
#include <linux/printk.h>
#include <linux/debugfs.h>
#include <linux/atomic.h>

unsigned long ltram_start_pfn __read_mostly;
unsigned long ltram_end_pfn   __read_mostly;

/* Free-running, never reset. A delta is what a test reads; an absolute value
 * is what a boot-time audit reads. */
static atomic64_t ltram_stray_allocs;

/*
 * Report a page that reached LtRAM through a path that should not exist.
 * Rate-limited stack traces plus an unbounded count: the count is the honest
 * number (a flooded log is unreadable), the traces name the caller.
 */
void ltram_note_stray_alloc(unsigned long pfn, const char *where)
{
	atomic64_inc(&ltram_stray_allocs);
	pr_warn_ratelimited("ltram: stray page pfn %lu from %s (total %lld)\n",
			    pfn, where, atomic64_read(&ltram_stray_allocs));
}

/*
 * Declare the window to memblock as node LTRAM_NUMA_NODE.
 *
 * Called at the TOP of bootmem_init(), before max_pfn is derived from
 * memblock_end_of_DRAM() so the window is included, and before
 * arch_numa_init() so the node exists when NUMA is set up. On this platform
 * the window is not in any firmware table -- unlike x86 where the node comes
 * from SRAT -- so it has to be injected here or it does not exist at all.
 */
void __init ltram_declare_node(void)
{
	int rc;

	rc = memblock_add_node(LTRAM_PHYS_BASE, LTRAM_PHYS_SIZE,
			       LTRAM_NUMA_NODE, MEMBLOCK_NONE);
	if (rc) {
		pr_err("ltram: memblock_add_node failed (%d) -- LtRAM disabled\n", rc);
		return;
	}

	ltram_start_pfn = PFN_UP(LTRAM_PHYS_BASE);
	ltram_end_pfn   = PFN_DOWN(LTRAM_PHYS_BASE + LTRAM_PHYS_SIZE);

	pr_info("ltram: node %d = 0x%llx + %llu MiB, pfn %lu..%lu (%lu pages)\n",
		LTRAM_NUMA_NODE, LTRAM_PHYS_BASE, LTRAM_PHYS_SIZE >> 20,
		ltram_start_pfn, ltram_end_pfn, ltram_end_pfn - ltram_start_pfn);
}

static int __init ltram_debugfs_init(void)
{
	struct dentry *d;

	if (!ltram_end_pfn)
		return 0;

	d = debugfs_create_dir("ltram", NULL);
	debugfs_create_u64("stray_allocs", 0444, d,
			   (u64 *)&ltram_stray_allocs.counter);
	debugfs_create_ulong("start_pfn", 0444, d, &ltram_start_pfn);
	debugfs_create_ulong("end_pfn",   0444, d, &ltram_end_pfn);
	return 0;
}
late_initcall(ltram_debugfs_init);
