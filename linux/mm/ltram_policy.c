// SPDX-License-Identifier: GPL-2.0-only
/*
 * LtRAM placement policy: decide which of a target process's pages belong on
 * flash, and move them there.
 *
 * TWO THINGS HERE ARE FORCED BY EARLIER DECISIONS, NOT CHOSEN:
 *
 * 1. The mover has its OWN page allocator. ZONE_LTRAM appears in no zonelist
 *    (that is how residency is enforced), so the buddy allocator cannot hand
 *    out a flash page even if asked. The zone's pages are never released to
 *    buddy -- managed stays 0 -- and this file owns them through a bitmap.
 *    That is also what the FREE/VALID/DIRTY lifecycle in the design needs.
 *
 * 2. Promotion is driven by OBSERVED behaviour, not by VMA flags. The target
 *    workload allocates writable memory, fills it once, and then only reads it;
 *    a policy keyed on VM_WRITE would never see it. So pages are watched across
 *    passes and promoted for staying clean.
 */
#include <linux/ltram.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/migrate.h>
#include <linux/sched/mm.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/xarray.h>
#include <linux/pagewalk.h>
#include <linux/rmap.h>
#include "internal.h"
#include <asm/tlbflush.h>
#include <linux/slab.h>

/* ---- tunables -------------------------------------------------------------
 * Defaults are deliberately conservative: the wear budget is 41.5 erases/s
 * sustained (65,536 sectors x 100,000 erases over five years), and a promotion
 * that turns out wrong costs two erases. A policy that promotes faster than it
 * can be wrong is a device-killer, so the batch cap matters more than the
 * interval.
 */
static unsigned int scan_interval_ms = 1000;
static unsigned int clean_passes_required = 3;
static unsigned int promote_batch = 32;	/* pages per pass; 32/s is under the budget */
module_param(scan_interval_ms, uint, 0644);
module_param(clean_passes_required, uint, 0644);
module_param(promote_batch, uint, 0644);

/* ---- the flash page allocator --------------------------------------------- */
static unsigned long *ltram_free_bitmap;	/* 1 = free */
static unsigned long ltram_nr_pages;
static DEFINE_SPINLOCK(ltram_alloc_lock);
static atomic64_t stat_demoted;
static atomic64_t stat_late_free;
static atomic64_t ltram_pages_in_use;

static struct page *ltram_alloc_page(void)
{
	unsigned long idx, flags;
	struct page *p = NULL;

	spin_lock_irqsave(&ltram_alloc_lock, flags);
	if (ltram_free_bitmap) {
		idx = find_first_bit(ltram_free_bitmap, ltram_nr_pages);
		if (idx < ltram_nr_pages) {
			__clear_bit(idx, ltram_free_bitmap);
			p = pfn_to_page(ltram_start_pfn + idx);
			atomic64_inc(&ltram_pages_in_use);
		}
	}
	spin_unlock_irqrestore(&ltram_alloc_lock, flags);

	if (p) {
		/*
		 * These pages were never given to buddy, so they arrive with no
		 * refcount. Establish one the way the migration core expects.
		 */
		set_page_count(p, 1);
		p->flags &= ~PAGE_FLAGS_CHECK_AT_PREP;
	}
	return p;
}

static void ltram_free_page_back(struct page *p)
{
	unsigned long idx = page_to_pfn(p) - ltram_start_pfn;
	unsigned long flags;

	/* free_pages_prepare() can reach here from softirq and with interrupts
	 * already off, so this lock cannot be the plain variety. */
	spin_lock_irqsave(&ltram_alloc_lock, flags);
	if (ltram_free_bitmap && idx < ltram_nr_pages && !test_bit(idx, ltram_free_bitmap)) {
		__set_bit(idx, ltram_free_bitmap);
		atomic64_dec(&ltram_pages_in_use);
	}
	spin_unlock_irqrestore(&ltram_alloc_lock, flags);
}

/* migrate_pages() callbacks */
static struct folio *ltram_get_new_folio(struct folio *src, unsigned long private)
{
	struct page *p;

	/* Only base pages. A 2 MiB folio is 512 flash sectors, and one write
	 * anywhere in it would demote all 512 -- 256x the wear of a wrong guess
	 * on a base page. Huge pages are excluded by eligibility, and this is
	 * the backstop. */
	if (folio_test_large(src))
		return NULL;

	p = ltram_alloc_page();
	return p ? page_folio(p) : NULL;
}

static void ltram_put_new_folio(struct folio *dst, unsigned long private)
{
	ltram_free_page_back(&dst->page);
}

/*
 * Hand a flash page back to the bitmap. Reached from __folio_put_small() when
 * the last reference to a promoted page goes away -- normally because a write
 * fault demoted it back to DRAM and wp_page_copy() dropped the original.
 *
 * These pages are NOT buddy's. ZONE_LTRAM is in no zonelist and its managed
 * count is zero, so free_unref_page() would be handing the allocator a page it
 * has never owned and does not account for.
 */
void ltram_free_folio(struct folio *folio)
{
	ltram_free_page_back(&folio->page);
}

/*
 * The catch-all. __folio_put_small() sees only single-page puts; release_pages()
 * and free_unref_page_list() -- the batch path that process exit and munmap take
 * -- bypass it entirely. Before this existed those pages hit the backstop in
 * free_pages_prepare(), were correctly kept out of buddy, and then leaked,
 * because nothing gave them back to the bitmap. 16,502 of them went missing in
 * one evening. free_pages_prepare() is the single funnel every free path passes
 * through, so hooking it here covers all of them by construction.
 */
void ltram_free_page(struct page *page)
{
	atomic64_inc(&stat_late_free);
	ltram_free_page_back(page);
}

void ltram_note_demotion(void)
{
	atomic64_inc(&stat_demoted);
}

/* ---- per-page observation -------------------------------------------------
 * Keyed by pfn. Small and sparse: only pages of the target that have been seen
 * clean at least once appear here.
 */
struct ltram_obs { u32 clean_runs; };
static DEFINE_XARRAY(ltram_obs);

/* ---- the policy interface -------------------------------------------------
 * A vtable, not BPF. The policy call is ~30 ns against a 16.4 ms erase, so the
 * dispatch cost is invisible either way; the reason to start here is that the
 * first policy has to be debugged at all, and struct_ops adds a layer between
 * you and that. The interface is deliberately the shape struct_ops needs, so a
 * BPF backend can be added later without touching the scanner.
 */
struct ltram_policy {
	const char *name;
	void (*pass_begin)(void);
	bool (*should_promote)(struct folio *f, struct ltram_obs *o, bool written);
	void (*pass_end)(void);
};

static bool clean_run_should_promote(struct folio *f, struct ltram_obs *o, bool written)
{
	if (written) {
		o->clean_runs = 0;
		return false;
	}
	if (o->clean_runs < U32_MAX)
		o->clean_runs++;
	return o->clean_runs >= clean_passes_required;
}

static const struct ltram_policy policy_clean_run = {
	.name           = "clean-run",
	.should_promote = clean_run_should_promote,
};

static const struct ltram_policy *policy = &policy_clean_run;

/* ---- counters ------------------------------------------------------------- */
static atomic64_t stat_scanned, stat_promoted, stat_promote_failed;

/* ---- why a scanned page was NOT selected ----------------------------------
 * "scanned 5184674, promoted 0, promote_failed 0" says a page was looked at and
 * never chosen, and says NOTHING about which test rejected it. Four different
 * bugs produce that same triple, and one of them is not in should_promote() at
 * all: if should_promote() returns true and folio_isolate_lru() then fails, the
 * page falls through to the re-arm and is counted nowhere.
 *
 * These six separate every path that can end in "not promoted":
 *
 *   rej_dirty_ro     dirty, NOT writable -- the mprotect(PROT_READ) weights.
 *                    Confirms the issue-10 hypothesis if this dominates.
 *   rej_dirty_rw     dirty AND writable -- a page the re-arm should be able to
 *                    clean. If this stays high while rearmed also climbs, the
 *                    re-arm is firing and not clearing anything, which is a
 *                    DIFFERENT bug from the hypothesis and is not fixed by it.
 *   rej_runs_short   seen clean, but clean_runs < clean_passes_required. The
 *                    policy is working and just needs more passes.
 *   sel_isolated     chosen and isolated -- became a candidate.
 *   sel_isolate_fail chosen but folio_isolate_lru() refused. Invisible before.
 *   rearmed          the write-protect actually executed.
 */
static atomic64_t rej_not_anon;
static atomic64_t rej_writable, rej_runs_short,
		  sel_isolated, sel_isolate_fail, rearmed, stale_dirty;

/* ---- targeting ------------------------------------------------------------ */
static pid_t target_pid;
static DEFINE_MUTEX(target_lock);

/* ---- the scan ------------------------------------------------------------- */
struct scan_ctx {
	struct list_head candidates;
	unsigned int nr;
};

static int scan_pte_range(pmd_t *pmd, unsigned long addr, unsigned long end,
			  struct mm_walk *walk)
{
	struct scan_ctx *ctx = walk->private;
	pte_t *pte, *start_pte;
	spinlock_t *ptl;

	start_pte = pte = pte_offset_map_lock(walk->mm, pmd, addr, &ptl);
	if (!pte)
		return 0;

	for (; addr < end && ctx->nr < promote_batch; addr += PAGE_SIZE, pte++) {
		pte_t p = ptep_get(pte);
		struct folio *folio;
		struct ltram_obs *o;
		bool written;

		if (!pte_present(p))
			continue;
		folio = vm_normal_folio(walk->vma, addr, p);
		if (!folio || folio_test_large(folio) || folio_is_ltram(folio))
			continue;
		/* Mobility: a pinned page must never be promoted -- it cannot be
		 * migrated later for wear-levelling or demotion, and a device may
		 * hold its physical address. */
		if (folio_maybe_dma_pinned(folio))
			continue;
		/*
		 * Anonymous private memory only.
		 *
		 * A page-cache folio belongs to the filesystem, not to this mm.
		 * It is shared with every other process mapping that file, the
		 * block layer can DMA into the buffer heads hanging off it, and
		 * moving it goes through __buffer_migrate_folio() rather than the
		 * anon path. None of that is safe against a backing store whose
		 * reads cross a coherent interconnect to an FPGA.
		 *
		 * This was not a rare edge case. The scanner PREFERRED these:
		 * read-only file text is the most read-mostly memory a process
		 * has, and it sits at the low addresses the batch fills from
		 * first. On 2026-08-20 a file-backed folio with buffer heads was
		 * migrated onto flash and the machine took an uncorrectable
		 * SError out of the coherent fabric moments later -- L2C LFB
		 * entry timeout and global sync CCPI timeout, i.e. a request to
		 * the window that never completed.
		 *
		 * KSM folios are excluded for the same reason: they are shared
		 * between unrelated mms and this policy has no right to relocate
		 * them on everyone else's behalf.
		 */
		if (!folio_test_anon(folio) || folio_test_ksm(folio)) {
			atomic64_inc(&rej_not_anon);
			continue;
		}
		if (!folio_try_get(folio))
			continue;

		atomic64_inc(&stat_scanned);

		/*
		 * "Has this page been written since I last armed it?"
		 *
		 * NOT pte_dirty(). That is (PTE_DIRTY | writable-and-not-rdonly),
		 * and PTE_DIRTY is a one-way latch: set by the fault handler on
		 * the first write a page ever takes, preserved by pte_modify()
		 * across mprotect, re-set rather than cleared by pte_wrprotect(),
		 * and cleared only by pte_mkclean() -- which nothing here calls.
		 * It answers "was this page EVER written", which is the question
		 * the swap path needs and not the one this policy asks. Measured
		 * cost of asking it: 13,247,550 of 13,258,954 scans rejected, and
		 * not one weight page ever promoted.
		 *
		 * Write permission is the honest signal on this CPU. With
		 * HAFDBS = 0 nothing but the page-fault handler can restore it
		 * after the scanner takes it away, so !pte_write means exactly
		 * "no write has trapped since we armed this page". The scanner
		 * clears it; only a real trapped write sets it back.
		 *
		 * pte_write() alone is bit 51; the hardware also needs PTE_RDONLY
		 * clear before a store actually lands, so this is conservative --
		 * it can call a page written for one extra pass when a store
		 * would in fact have trapped. One pass, and it keeps this to
		 * generic kernel API rather than an arm64-only macro.
		 */
		written = pte_write(p);

		/*
		 * Pages the OLD rule would have rejected and this one accepts.
		 * Purely observational: it is the A/B for this change, visible in
		 * the same run rather than across two boots.
		 */
		if (pte_dirty(p) && !written)
			atomic64_inc(&stale_dirty);

		o = xa_load(&ltram_obs, folio_pfn(folio));
		if (!o) {
			o = kzalloc(sizeof(*o), GFP_ATOMIC);
			if (!o) { folio_put(folio); continue; }
			if (xa_err(xa_store(&ltram_obs, folio_pfn(folio), o, GFP_ATOMIC))) {
				kfree(o); folio_put(folio); continue;
			}
		}

		if (policy->should_promote(folio, o, written)) {
			if (!folio_isolate_lru(folio)) {
				atomic64_inc(&sel_isolate_fail);
				goto rearm;
			}
			atomic64_inc(&sel_isolated);
			list_add(&folio->lru, &ctx->candidates);
			ctx->nr++;
			/*
			 * DROP OUR REFERENCE. folio_isolate_lru() took its own,
			 * and that is the one the candidate list owns from here.
			 * Keeping the folio_try_get() reference as well leaves the
			 * folio at +2 where folio_migrate_mapping() expects +1, so
			 * every migration returns -EAGAIN and fails BEFORE reaching
			 * the flash hook -- silently, because the failure looks like
			 * ordinary migration contention.
			 *
			 * Measured: 448 promote_failed, 0 promoted, and the FPGA
			 * status word did not move at all.
			 */
			folio_put(folio);
			continue;
		}

		if (written)
			atomic64_inc(&rej_writable);
		else
			atomic64_inc(&rej_runs_short);
rearm:
		/*
		 * Re-arm write observation. There is NO hardware dirty-bit
		 * management on this CPU (measured: ID_AA64MMFR1_EL1 HAFDBS = 0),
		 * so the dirty bit is set by the fault handler -- which means the
		 * only way to observe the NEXT write is to write-protect now and
		 * let it fault.
		 *
		 * Clearing the young/accessed bit instead, which is the obvious
		 * thing to reach for, observes ACCESS and not WRITE -- and this
		 * policy is about writes. A read-only workload would look busy and
		 * never be promoted.
		 *
		 * The cost is a fault per write, so it scales with write traffic
		 * rather than with pages watched. That is the standard price of
		 * software dirty tracking and the reason the scan interval is a
		 * tunable.
		 */
		if (written) {
			ptep_set_wrprotect(walk->mm, addr, pte);
			flush_tlb_page(walk->vma, addr);
			atomic64_inc(&rearmed);
		}
		folio_put(folio);
	}
	pte_unmap_unlock(start_pte, ptl);
	cond_resched();
	return 0;
}

static const struct mm_walk_ops scan_ops = {
	.pmd_entry = scan_pte_range,
	.walk_lock = PGWALK_RDLOCK,
};

static void ltram_scan_once(void)
{
	struct scan_ctx ctx = { .nr = 0 };
	struct task_struct *task;
	struct mm_struct *mm = NULL;
	unsigned int moved;
	pid_t pid;

	mutex_lock(&target_lock);
	pid = target_pid;
	mutex_unlock(&target_lock);
	if (!pid)
		return;

	INIT_LIST_HEAD(&ctx.candidates);

	rcu_read_lock();
	task = pid_task(find_vpid(pid), PIDTYPE_PID);
	if (task)
		mm = get_task_mm(task);
	rcu_read_unlock();
	if (!mm)
		return;

	if (policy->pass_begin)
		policy->pass_begin();

	mmap_read_lock(mm);
	walk_page_range(mm, 0, TASK_SIZE, &scan_ops, &ctx);
	mmap_read_unlock(mm);
	mmput(mm);

	if (policy->pass_end)
		policy->pass_end();

	if (!ctx.nr)
		return;

	/*
	 * migrate_pages() with our own allocator. The flash write happens inside
	 * migrate_folio_extra() (step 5), before the mapping moves, so a failure
	 * here leaves the pages in DRAM intact.
	 */
	moved = 0;
	/* The last argument is ret_succeeded -- the number that MOVED, not the
	 * number that failed. Getting this backwards produces a counter that
	 * reports success while nothing moved, which is the exact failure this
	 * subsystem exists to make impossible. */
	migrate_pages(&ctx.candidates, ltram_get_new_folio, ltram_put_new_folio,
		      0, MIGRATE_SYNC, MR_NUMA_MISPLACED, &moved);
	atomic64_add(moved, &stat_promoted);
	atomic64_add(ctx.nr - moved, &stat_promote_failed);
	putback_movable_pages(&ctx.candidates);
}

static int ltram_scan_thread(void *unused)
{
	while (!kthread_should_stop()) {
		ltram_scan_once();
		msleep_interruptible(scan_interval_ms);
	}
	return 0;
}

static struct task_struct *scan_task;

/* ---- sysfs ---------------------------------------------------------------- */
static ssize_t target_pid_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%d\n", target_pid);
}

static ssize_t target_pid_store(struct kobject *k, struct kobj_attribute *a,
				const char *buf, size_t n)
{
	pid_t pid;

	if (kstrtoint(buf, 10, &pid))
		return -EINVAL;

	mutex_lock(&target_lock);
	target_pid = pid;
	mutex_unlock(&target_lock);
	pr_info("ltram: target pid %d\n", pid);
	return n;
}

static ssize_t stats_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return sysfs_emit(buf,
		"policy            %s\n"
		"target_pid        %d\n"
		"scanned           %lld\n"
		"promoted          %lld\n"
		"promote_failed    %lld\n"
		"demoted           %lld\n"
		"pages_in_use      %lld\n"
		"pages_total       %lu\n"
		"rej_writable      %lld\n"
		"rej_runs_short    %lld\n"
		"stale_dirty       %lld\n"
		"sel_isolated      %lld\n"
		"sel_isolate_fail  %lld\n"
		"rearmed           %lld\n"
		"late_free         %lld\n"
		"rej_not_anon      %lld\n",
		policy->name, target_pid,
		atomic64_read(&stat_scanned), atomic64_read(&stat_promoted),
		atomic64_read(&stat_promote_failed), atomic64_read(&stat_demoted),
		atomic64_read(&ltram_pages_in_use), ltram_nr_pages,
		atomic64_read(&rej_writable), atomic64_read(&rej_runs_short),
		atomic64_read(&stale_dirty), atomic64_read(&sel_isolated),
		atomic64_read(&sel_isolate_fail), atomic64_read(&rearmed),
		atomic64_read(&stat_late_free), atomic64_read(&rej_not_anon));
}

static struct kobj_attribute target_pid_attr = __ATTR_RW(target_pid);
static struct kobj_attribute stats_attr = __ATTR_RO(stats);
static struct attribute *ltram_attrs[] = {
	&target_pid_attr.attr, &stats_attr.attr, NULL,
};
ATTRIBUTE_GROUPS(ltram);
static struct kobject *ltram_kobj;

static int __init ltram_policy_init(void)
{
	if (!ltram_end_pfn)
		return 0;

	ltram_nr_pages = ltram_end_pfn - ltram_start_pfn;
	ltram_free_bitmap = bitmap_zalloc(ltram_nr_pages, GFP_KERNEL);
	if (!ltram_free_bitmap)
		return -ENOMEM;
	bitmap_fill(ltram_free_bitmap, ltram_nr_pages);

	ltram_kobj = kobject_create_and_add("ltram", kernel_kobj);
	if (ltram_kobj && sysfs_create_groups(ltram_kobj, ltram_groups))
		pr_warn("ltram: sysfs registration failed\n");

	scan_task = kthread_run(ltram_scan_thread, NULL, "ltram_scand");
	if (IS_ERR(scan_task)) {
		pr_err("ltram: could not start scanner\n");
		scan_task = NULL;
	}

	pr_info("ltram: policy '%s' ready, %lu pages, idle until a target pid is set\n",
		policy->name, ltram_nr_pages);
	return 0;
}
late_initcall(ltram_policy_init);
