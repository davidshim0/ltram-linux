// SPDX-License-Identifier: GPL-2.0
/*
 * ltram: out-of-tree module that exposes /dev/ltram and handles the
 * LtRAM <-> DRAM repatriation on write.
 *
 * Builds against kmohr's zone_ltram kernel branch, which provides
 * ZONE_LTRAM, __GFP_LTRAM, and ltram_migrate_to/from helpers.
 *
 * EE392C project, branch 250522_Implementation.
 */

#include <linux/module.h>
#include <linux/mm.h>
#include <linux/numa.h>
#include <linux/pagemap.h>
#include <linux/atomic.h>
#include <linux/ltram.h>

/* Global counters; exposed via debugfs later. */
static struct {
	atomic_long_t nr_repat;
	atomic_long_t nr_repat_fail;
	atomic_long_t nr_warn_wrong_zone;
} ltram_stats;

/*
 * page_mkwrite: a process wrote a write-protected page in our VMA.
 *
 * If the page is in ZONE_LTRAM, we migrate it back to DRAM using
 * Katherine's ltram_migrate_from(). That helper wraps migrate_pages
 * with the right target, which means:
 *   - rmap is walked and every PTE that mapped the source folio is
 *     swapped to the new DRAM folio, writable.
 *   - TLBs on every CPU are shot down by the migration framework.
 *   - The source folio's reference count is dropped and the page
 *     returns to ZONE_LTRAM's free list.
 *
 * On x86 there is no extra cache flush needed (cache-coherent). On a
 * VIVT arch, migrate_pages handles flush_dcache_folio internally.
 *
 * Return VM_FAULT_NOPAGE so wp_page_shared skips finish_mkwrite_fault;
 * the kernel will retry the user's store against the new writable PTE.
 */
vm_fault_t ltram_page_mkwrite(struct vm_fault *vmf)
{
	struct folio *folio = page_folio(vmf->page);
	int err;

	/* Confirm the source page lives in ZONE_LTRAM. If not, this VMA
	 * was set up wrong; fall through to normal WP semantics. */
	if (folio_zonenum(folio) != ZONE_LTRAM) {
		atomic_long_inc(&ltram_stats.nr_warn_wrong_zone);
		pr_warn_ratelimited("ltram: page_mkwrite on non-LtRAM folio (zone=%d)\n",
				    folio_zonenum(folio));
		return 0;
	}

	/* Hand it to the kernel-side migration helper. */
	err = ltram_migrate_from(folio);
	if (err) {
		atomic_long_inc(&ltram_stats.nr_repat_fail);
		/* -EBUSY: could not isolate from LRU; retry the fault.
		 * -EFAULT: migration itself failed; also retry. */
		return VM_FAULT_RETRY;
	}

	atomic_long_inc(&ltram_stats.nr_repat);
	return VM_FAULT_NOPAGE;
}
EXPORT_SYMBOL_GPL(ltram_page_mkwrite);

/*
 * Next steps: misc_register("/dev/ltram"), ltram_mmap that installs
 * our vm_ops, ltram_fault that calls folio_alloc(GFP_LTRAM) and
 * returns the page read-only, debugfs for the counters, per-page
 * write counter array.
 */
