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
#include <linux/io.h>
#include <linux/moduleparam.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/highmem.h>

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
/*
 * "ltram=off" on the kernel command line skips the declaration entirely: no node,
 * no zone, no memblock change. The resulting boot is equivalent to CONFIG_LTRAM=n,
 * which makes every LtRAM kernel here recoverable without a rebuild -- if a change
 * to this path breaks boot, the previous image still boots with one word added to
 * the command line.
 */
static bool ltram_disabled __initdata;

static int __init ltram_setup(char *str)
{
	if (str && !strcmp(str, "off"))
		ltram_disabled = true;
	return 1;
}
__setup("ltram=", ltram_setup);

void __init ltram_declare_node(void)
{
	int rc;

	if (ltram_disabled) {
		pr_info("ltram: disabled on the command line (ltram=off)\n");
		return;
	}

	/*
	 * The pfn bounds must be live BEFORE the range enters memblock. Anything
	 * keyed on them -- and the fault-path predicate pfn_is_ltram() -- would
	 * otherwise see 0 for the window between the add and the assignment.
	 */
	ltram_start_pfn = PFN_UP(LTRAM_PHYS_BASE);
	ltram_end_pfn   = PFN_DOWN(LTRAM_PHYS_BASE + LTRAM_PHYS_SIZE);

	rc = memblock_add_node(LTRAM_PHYS_BASE, LTRAM_PHYS_SIZE,
			       LTRAM_NUMA_NODE, MEMBLOCK_NONE);
	if (rc) {
		pr_err("ltram: memblock_add_node failed (%d) -- LtRAM disabled\n", rc);
		ltram_start_pfn = ltram_end_pfn = 0;
		return;
	}

	/*
	 * RESERVE it, rather than filtering it out of memblock iteration.
	 *
	 * This is the whole fix for "the window has no kernel address". The two
	 * things we need pull in opposite directions through one function:
	 *
	 *   for_each_free_mem_range()  -- allocation -- must NEVER return this
	 *   for_each_mem_range()       -- what map_mem() walks to build the
	 *                                 linear map -- MUST see it
	 *
	 * Both run through should_skip_region(), so the range filter that used to
	 * live there did both jobs: it kept boot allocations off flash AND kept
	 * map_mem() from ever mapping the window. Every kernel mapping API then
	 * broke, because memblock_add_node() had already flipped
	 * pfn_is_map_memory() to true: ioremap_cache() started returning
	 * __phys_to_virt() of an address with no page-table entry, ioremap()
	 * started returning NULL, and memremap(MEMREMAP_WB) followed
	 * ioremap_cache(). The first touch was a level-0 translation fault.
	 *
	 * A reservation separates them exactly. Reserved memory is subtracted
	 * from the allocation iterator and skipped by memblock_free_all(), so
	 * nothing allocates here and the pages are never released to buddy --
	 * ZONE_LTRAM keeps managed == 0, which is what makes residency
	 * enforceable. But reserved memory is still MEMORY: for_each_mem_range()
	 * walks it and map_mem() maps it, so the window has a kernel virtual
	 * address and page_address() works on its struct pages -- which step 5
	 * requires for the migration destination.
	 */
	rc = memblock_reserve(LTRAM_PHYS_BASE, LTRAM_PHYS_SIZE);
	if (rc) {
		pr_err("ltram: memblock_reserve failed (%d) -- LtRAM disabled\n", rc);
		ltram_start_pfn = ltram_end_pfn = 0;
		return;
	}

	pr_info("ltram: node %d = 0x%llx + %llu MiB, pfn %lu..%lu (%lu pages), reserved\n",
		LTRAM_NUMA_NODE, LTRAM_PHYS_BASE, LTRAM_PHYS_SIZE >> 20,
		ltram_start_pfn, ltram_end_pfn, ltram_end_pfn - ltram_start_pfn);
}


/*
 * ---- the write backend ---------------------------------------------------
 *
 * The NOR driver is a module and can be unloaded while a migration is in
 * flight. A bare pointer plus a NULL check is not enough: the check can pass,
 * the module can go away, and the call then lands in freed text. So every call
 * holds a module reference for its duration, and unregister waits.
 */
static const struct ltram_flash_ops *ltram_ops;
static DEFINE_SPINLOCK(ltram_ops_lock);

static atomic64_t ltram_writes_ok;
static atomic64_t ltram_writes_failed;

int ltram_register_flash_ops(const struct ltram_flash_ops *ops)
{
	if (!ops || !ops->write_page)
		return -EINVAL;

	spin_lock(&ltram_ops_lock);
	if (ltram_ops) {
		spin_unlock(&ltram_ops_lock);
		return -EBUSY;
	}
	ltram_ops = ops;
	spin_unlock(&ltram_ops_lock);

	pr_info("ltram: flash backend registered by %s\n",
		ops->owner ? module_name(ops->owner) : "builtin");
	return 0;
}
EXPORT_SYMBOL_GPL(ltram_register_flash_ops);

void ltram_unregister_flash_ops(const struct ltram_flash_ops *ops)
{
	spin_lock(&ltram_ops_lock);
	if (ltram_ops == ops)
		ltram_ops = NULL;
	spin_unlock(&ltram_ops_lock);
	pr_info("ltram: flash backend unregistered\n");
}
EXPORT_SYMBOL_GPL(ltram_unregister_flash_ops);

bool ltram_have_flash_ops(void)
{
	bool have;

	spin_lock(&ltram_ops_lock);
	have = ltram_ops != NULL;
	spin_unlock(&ltram_ops_lock);
	return have;
}

/*
 * Program one page of flash. Refuses rather than pretending: with no backend
 * registered this returns -ENODEV, so a caller cannot mistake "nothing wrote
 * it" for success -- which is the whole failure mode this subsystem exists to
 * eliminate.
 */
/*
 * Erase one sector. Same module-pinning dance as ltram_write_page(): take the
 * pointer under the lock and pin the provider before dropping it, so the
 * backend cannot unload between the check and the call.
 */
int ltram_erase_page(unsigned long pfn)
{
	const struct ltram_flash_ops *ops;
	struct module *owner;
	int rc;

	if (WARN_ON_ONCE(!pfn_is_ltram(pfn)))
		return -EINVAL;

	spin_lock(&ltram_ops_lock);
	ops = ltram_ops;
	owner = ops ? ops->owner : NULL;
	if (ops && owner && !try_module_get(owner))
		ops = NULL;
	spin_unlock(&ltram_ops_lock);

	if (!ops || !ops->erase_page) {
		if (owner)
			module_put(owner);
		return -ENODEV;
	}

	rc = ops->erase_page(pfn);

	if (owner)
		module_put(owner);
	return rc;
}
EXPORT_SYMBOL_GPL(ltram_erase_page);

bool ltram_have_erase_op(void)
{
	bool have;

	spin_lock(&ltram_ops_lock);
	have = ltram_ops && ltram_ops->erase_page;
	spin_unlock(&ltram_ops_lock);
	return have;
}
EXPORT_SYMBOL_GPL(ltram_have_erase_op);

/*
 * A single instantaneous sample. Absent op means "assume idle" -- correct only
 * when nothing else drives the device, which is true for our own test setups
 * and would not be in production.
 */
bool ltram_device_idle(void)
{
	const struct ltram_flash_ops *ops;
	bool idle = true;

	spin_lock(&ltram_ops_lock);
	ops = ltram_ops;
	if (ops && ops->device_idle)
		idle = ops->device_idle();
	spin_unlock(&ltram_ops_lock);
	return idle;
}
EXPORT_SYMBOL_GPL(ltram_device_idle);

int ltram_write_page(unsigned long dst_pfn, unsigned long src_pfn)
{
	const struct ltram_flash_ops *ops;
	struct module *owner;
	int rc;

	if (WARN_ON_ONCE(!pfn_is_ltram(dst_pfn)))
		return -EINVAL;

	spin_lock(&ltram_ops_lock);
	ops = ltram_ops;
	owner = ops ? ops->owner : NULL;
	/* pin the provider before dropping the lock, so it cannot unload
	 * between here and the call */
	if (ops && owner && !try_module_get(owner))
		ops = NULL;
	spin_unlock(&ltram_ops_lock);

	if (!ops) {
		atomic64_inc(&ltram_writes_failed);
		return -ENODEV;
	}

	rc = ops->write_page(dst_pfn, src_pfn);
	if (!rc)
		ltram_record_promotion(dst_pfn, src_pfn);

	if (owner)
		module_put(owner);

	if (rc)
		atomic64_inc(&ltram_writes_failed);
	else
		atomic64_inc(&ltram_writes_ok);
	return rc;
}
EXPORT_SYMBOL_GPL(ltram_write_page);


/*
 * ---- migration into flash -------------------------------------------------
 */
bool folio_is_ltram(const struct folio *folio)
{
	return folio && pfn_is_ltram(folio_pfn((struct folio *)folio));
}
EXPORT_SYMBOL_GPL(folio_is_ltram);

/*
 * Program every page of @dst from @src through the driver.
 *
 * Called from migrate_folio_extra() BEFORE folio_migrate_mapping(), which is
 * the whole point: at that moment try_to_migrate() has already unmapped @src,
 * so its contents are stable, and a failure here aborts the migration with
 * nothing published. Hooked after the mapping move there would be no way to
 * fail safely -- the mapping would point at flash that never received the data,
 * which is exactly the silent corruption this exists to prevent.
 */
int ltram_copy_to_flash(struct folio *dst, struct folio *src)
{
	long nr = folio_nr_pages(dst);
	long i;

	if (WARN_ON_ONCE(folio_nr_pages(src) != nr))
		return -EINVAL;

	for (i = 0; i < nr; i++) {
		/*
		 * The PFN, not a mapping. The backend hands this straight to the
		 * DMA engine as the source address, so nothing here dereferences
		 * it and the kmap that used to wrap this loop is gone. Deriving a
		 * physical address from kmap_local_folio() would have worked on
		 * arm64, where it degenerates to the linear map, and been wrong
		 * anywhere with highmem.
		 */
		int rc = ltram_write_page(folio_pfn(dst) + i, folio_pfn(src) + i);

		if (rc) {
			pr_warn_ratelimited("ltram: flash write failed at page %ld/%ld of folio pfn %lu (%d) -- migration aborted\n",
					    i, nr, folio_pfn(dst), rc);
			return rc;
		}
	}
	return 0;
}
EXPORT_SYMBOL_GPL(ltram_copy_to_flash);

/*
 * ---- boot-time read self-test -------------------------------------------
 *
 * Proves the window is mapped, coherent and readable from kernel context --
 * which is a different claim from "the node appears in /proc". It reads only:
 * the CPU physically cannot store here, so the write half of the self-test has
 * to wait until the NOR driver registers a DMA backend.
 *
 * The harness pattern is SELF-CHECKING, which is what makes this possible with
 * no userspace and no reference file: every 32-bit word equals its own byte
 * offset within the device xored with a seed, so the expected value at any
 * address is computable here.
 *
 * Sampling: a stride that is coprime with both the 4 KiB sector and the 128 B
 * cache line, so the walk crosses sector and line boundaries instead of
 * sitting in one comfortable region. The known read-side fault on this
 * hardware only appears on the FIRST word of a line, and a test that never
 * lands there will not see it.
 */
static unsigned int ltram_selftest_words = 1024;
core_param(ltram_selftest_words, ltram_selftest_words, uint, 0444);
static unsigned int ltram_selftest_seed;
core_param(ltram_selftest_seed, ltram_selftest_seed, uint, 0444);

static int __init ltram_selftest(void)
{
	void __iomem *base;
	unsigned int i, erased = 0, zero = 0, matched = 0;
	u64 stride = 4099;	/* coprime with 4096 and 128 */
	u32 sum = 0;

	if (!ltram_end_pfn || !ltram_selftest_words)
		return 0;

	/*
	 * Late enough that the FPGA is programmed and the ECI link is up.
	 * Reading before that is an external abort -- a dead boot with nothing
	 * after it on the console -- which is why this is a late initcall and
	 * not part of the declaration path.
	 */
	base = memremap(LTRAM_PHYS_BASE, LTRAM_PHYS_SIZE, MEMREMAP_WB);
	if (!base) {
		pr_err("ltram: self-test could not map the window\n");
		return 0;
	}

	for (i = 0; i < ltram_selftest_words; i++) {
		u64 off = ((u64)i * stride * 4) % (LTRAM_PHYS_SIZE - 4);
		u32 got;

		off &= ~3ULL;
		got = readl(base + off);
		sum += got;

		if (got == 0xFFFFFFFFu)
			erased++;
		else if (got == 0)
			zero++;

		/*
		 * The harness writes word = (byte offset) ^ seed, but the seed is
		 * PER SECTOR, so a single seed cannot describe the whole device.
		 * Verification is therefore opt-in: pass ltram_selftest_seed= after
		 * filling a region with one known seed. Without it this reports a
		 * fingerprint rather than asserting a pattern the device may not
		 * hold -- a test that cries wolf on healthy hardware is worse than
		 * no test, because it trains you to ignore it.
		 */
		if (ltram_selftest_seed && got == ((u32)off ^ ltram_selftest_seed))
			matched++;

		if (i < 4)
			pr_info("ltram: self-test sample +0x%08llx = %08x\n", off, got);
	}
	memunmap(base);

	pr_info("ltram: self-test read %u words, stride %llu: erased(ff) %u, zero %u, checksum %08x\n",
		ltram_selftest_words, stride, erased, zero, sum);

	if (ltram_selftest_seed)
		pr_info("ltram: self-test seed 0x%08x: %u/%u words match offset^seed%s\n",
			ltram_selftest_seed, matched, ltram_selftest_words,
			matched == ltram_selftest_words ? "" : "  <-- MISMATCH");

	/*
	 * The load-bearing result is not any particular value: it is that we got
	 * here at all. Reaching this line means the window is mapped, readable
	 * from kernel context, and did not abort -- which is the claim step 3
	 * exists to make. Values are for cross-checking against the harness.
	 */
	pr_info("ltram: self-test completed without abort -- window is readable from kernel context\n");
	return 0;
}
late_initcall(ltram_selftest);

/*
 * echo "<pfn> <u32 pattern>" > /sys/kernel/debug/ltram/write_test
 *
 * Fills a page with the pattern and programs it through the backend. Exists so
 * the write path can be exercised with one page before any migration machinery
 * depends on it -- verify by reading the page back with the cache evicted, and
 * by watching the FPGA status word advance (beats +64, pages +16, erases +1).
 * The counter movement is the honest witness: a write that silently did nothing
 * leaves correct-looking cached data and zero counter delta.
 */
static ssize_t ltram_write_test(struct file *f, const char __user *ubuf,
				size_t len, loff_t *ppos)
{
	unsigned long pfn;
	unsigned int pat;
	char buf[64];
	u32 *page;
	int rc, i;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;
	if (sscanf(buf, "%lu %x", &pfn, &pat) != 2)
		return -EINVAL;

	page = (u32 *)__get_free_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;
	for (i = 0; i < PAGE_SIZE / 4; i++)
		page[i] = pat;

	rc = ltram_write_page(pfn, PFN_DOWN(__pa(page)));
	free_page((unsigned long)page);

	pr_info("ltram: write_test pfn %lu pattern %08x -> %d\n", pfn, pat, rc);
	return rc ? rc : len;
}

static const struct file_operations ltram_write_test_fops = {
	.owner = THIS_MODULE,
	.write = ltram_write_test,
	.llseek = noop_llseek,
};

/* Shared with mm/ltram_policy.c, which hangs the page-state file off it.
 * ltram.o links before ltram_policy.o and both are late_initcall, so this is
 * populated before the policy's initcall runs. */
struct dentry *ltram_debugfs_dir;

static int __init ltram_debugfs_init(void)
{
	struct dentry *d;

	if (!ltram_end_pfn)
		return 0;

	d = debugfs_create_dir("ltram", NULL);
	ltram_debugfs_dir = d;
	debugfs_create_u64("stray_allocs", 0444, d,
			   (u64 *)&ltram_stray_allocs.counter);
	debugfs_create_ulong("start_pfn", 0444, d, &ltram_start_pfn);
	debugfs_create_ulong("end_pfn",   0444, d, &ltram_end_pfn);
	debugfs_create_u64("writes_ok",     0444, d, (u64 *)&ltram_writes_ok.counter);
	debugfs_create_u64("writes_failed", 0444, d, (u64 *)&ltram_writes_failed.counter);
	debugfs_create_file("write_test", 0200, d, NULL, &ltram_write_test_fops);
	return 0;
}
late_initcall(ltram_debugfs_init);
