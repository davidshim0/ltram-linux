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
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/list.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/bitops.h>

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


/* ===========================================================================
 * Page-state tracking -- STEP 1b: the bucket lists now DRIVE allocation.
 *
 * 1a added these structures as a shadow that changed no decision, so the
 * bookkeeping could be proven before anything depended on it. It was, on
 * 2026-08-25: the invariant held across 1,472 allocate/free cycles.
 *
 * 1b hands the decision over. ltram_alloc_page() now pops the head of the
 * lowest non-empty bucket instead of calling find_first_bit(), so the
 * least-worn page is chosen and pages rotate rather than piling on index 0.
 * The old bitmap is still maintained purely as a cross-check.
 *
 * The design this builds toward is FREE / VALID / DIRTY / ERASING, with the
 * erase moved off the write path into a background worker. Today every
 * promotion pays ~16.4 ms of sector erase before a ~1.2 ms DMA; with pages
 * arriving pre-erased the erase leaves the latency path entirely. In 1a only
 * FREE and VALID are ever occupied, because a freed page still goes straight
 * back to FREE exactly as it does now.
 *
 * HOW EACH STATE IS REPRESENTED, and why they differ:
 *
 *   VALID    bitmap.  No ordering needed, and bitmap_weight() DERIVES the
 *   DIRTY    bitmap.  count from the data so it cannot drift away from it.
 *                     Disjointness is one bitwise AND. 8 KiB each.
 *
 *   ERASING  a single u32 index. The hardware erases one sector at a time --
 *            serialised under the backend's mutex -- so a queue would be a
 *            container that never holds more than one thing.
 *
 *   FREE     the bucket lists, and NOTHING ELSE. There is deliberately no
 *            free bitmap: free is exactly ~(VALID | DIRTY) minus whatever is
 *            being erased, so it is DERIVED on demand. A maintained free
 *            bitmap would be a third description of a set already described
 *            twice, i.e. one more thing that can disagree.
 *
 * The free list is bucketed by erase count at 1000 granularity, FIFO within a
 * bucket. FIFO is not a compromise: while every page shares an erase count
 * they all sit in bucket 0, and pop-head / return-tail IS round-robin, which
 * is the wear spreading we want. Exact per-erase-count buckets behave
 * identically there and cost 100,000 list heads instead of 101.
 *
 * RETIRED is deliberately absent. It only becomes meaningful with the parking
 * policy -- park a permanently read-only page in a nearly-worn sector so it is
 * never erased again -- which is a later experiment. Adding the state now would
 * mean a bitmap that is always empty and a line in the report for a feature
 * that does not exist.
 * ===========================================================================
 */
#define LT_EC_GRAIN	1000u
#define LT_EC_BUCKETS	101u			/* 0 .. 100,000 erases */
#define LT_ERASING_IDLE	0xFFFFFFFFu

enum lt_bmstate { LT_VALID = 0, LT_DIRTY, LT_NR_BM };
static const char * const lt_bm_name[LT_NR_BM] = { "valid", "dirty" };

static unsigned long	*lt_bm[LT_NR_BM];	/* VALID and DIRTY only */
static unsigned long	*lt_scratch;		/* derivations and ANDs */
static u32		*lt_ec;			/* erase count, per page */
static struct list_head	*lt_node;		/* per-page link into a bucket */
static struct list_head	 lt_free_by_ec[LT_EC_BUCKETS];
static unsigned long	 lt_free_count;		/* O(1) for the watermark */
static u32		 lt_erasing = LT_ERASING_IDLE;
static bool		 lt_ready;

static inline unsigned int lt_bucket_of(u32 ec)
{
	unsigned int b = ec / LT_EC_GRAIN;

	return b < LT_EC_BUCKETS ? b : LT_EC_BUCKETS - 1;
}

/*
 * FREE -> VALID. The erase count is incremented HERE, which is an upper bound:
 * in the current design every allocated page is erased by ft_ltram_write_page()
 * before it is programmed, so allocation and erase are one-to-one -- except
 * when a migration fails after allocating, which over-counts by one. Wrong in
 * the conservative direction, and replaced by an erase-completion hook in 1c.
 *
 * Caller holds ltram_alloc_lock.
 */
static void lt_to_valid(unsigned long idx)
{
	if (!lt_ready)
		return;
	list_del_init(&lt_node[idx]);		/* off its free bucket */
	lt_free_count--;
	__set_bit(idx, lt_bm[LT_VALID]);
	if (lt_ec[idx] < U32_MAX)
		lt_ec[idx]++;
}

/* VALID -> FREE. 1c inserts DIRTY and the erase between these two halves. */
static void lt_to_free(unsigned long idx)
{
	if (!lt_ready)
		return;
	__clear_bit(idx, lt_bm[LT_VALID]);
	list_add_tail(&lt_node[idx], &lt_free_by_ec[lt_bucket_of(lt_ec[idx])]);
	lt_free_count++;
}

/*
 * Pop the least-worn free page: the head of the lowest non-empty bucket.
 *
 * Scanning all 101 buckets is cheaper than the find_first_bit() it replaces --
 * 101 list_empty() loads against a 1,024-word bitmap scan -- so no cursor or
 * hint is needed. A hint would also be wrong: erase counts only rise, but a
 * page that sat VALID at a low count returns to a bucket BELOW wherever the
 * hint had drifted, so the scan has to start at zero anyway.
 *
 * Head, not tail: FIFO within a bucket. While every page shares an erase count
 * they all sit in bucket 0, and pop-head with return-to-tail is round-robin --
 * which is the wear spreading the old allocator lacked.
 *
 * Returns ltram_nr_pages when the window is full. Caller holds ltram_alloc_lock.
 */
static unsigned long lt_pop_free(void)
{
	unsigned int b;

	for (b = 0; b < LT_EC_BUCKETS; b++)
		if (!list_empty(&lt_free_by_ec[b]))
			return lt_free_by_ec[b].next - lt_node;
	return ltram_nr_pages;
}

static struct page *ltram_alloc_page(void)
{
	unsigned long idx, flags;
	struct page *p = NULL;

	spin_lock_irqsave(&ltram_alloc_lock, flags);
	/*
	 * STEP 1b: the bucket lists now DECIDE which page is handed out. The old
	 * ltram_free_bitmap is still maintained in step, purely so the assertion
	 * in lt_check_fast() -- derived FREE must equal it bit for bit -- keeps
	 * cross-checking two independently updated structures. It is cheap, and
	 * it is the strongest check this subsystem has. Retire it only once the
	 * bucket lists have been trusted for a while.
	 */
	if (lt_ready) {
		idx = lt_pop_free();
		if (idx < ltram_nr_pages) {
			__clear_bit(idx, ltram_free_bitmap);
			p = pfn_to_page(ltram_start_pfn + idx);
			atomic64_inc(&ltram_pages_in_use);
			lt_to_valid(idx);	/* removes it from its bucket */
		}
	} else if (ltram_free_bitmap) {
		/* Tracking failed to allocate at init. Fall back to the old
		 * lowest-index allocator rather than refusing to work at all. */
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
		lt_to_free(idx);	/* 1a shadow */
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


/* ---- invariant checking and the debugfs view ------------------------------
 *
 * The redundancy is the point. lt_free_count and the bucket lists are two
 * independently maintained descriptions of the same set, and the derived free
 * set is a third that cannot drift because it is computed, not stored.
 * Asserting they agree turns a silent page loss into a loud one -- which is
 * exactly what was missing when 16,502 pages went astray.
 */
#define LT_ERR_OVERLAP		(1u << 0)
#define LT_ERR_COUNT		(1u << 1)
#define LT_ERR_FREE_MISMATCH	(1u << 2)
#define LT_ERR_BUCKET_COUNT	(1u << 3)
#define LT_ERR_BUCKET_MEMBER	(1u << 4)

/*
 * Bitwise only: O(n/64) with popcount, a few microseconds. This is the subset
 * a hot path could afford. Caller holds ltram_alloc_lock.
 */
static unsigned int lt_check_fast(void)
{
	unsigned long total;
	unsigned int err = 0;

	if (!lt_ready)
		return 0;

	bitmap_and(lt_scratch, lt_bm[LT_VALID], lt_bm[LT_DIRTY], ltram_nr_pages);
	if (!bitmap_empty(lt_scratch, ltram_nr_pages))
		err |= LT_ERR_OVERLAP;

	total = bitmap_weight(lt_bm[LT_VALID], ltram_nr_pages) +
		bitmap_weight(lt_bm[LT_DIRTY], ltram_nr_pages) +
		lt_free_count +
		(lt_erasing != LT_ERASING_IDLE ? 1 : 0);
	if (total != ltram_nr_pages)
		err |= LT_ERR_COUNT;

	/*
	 * Derive FREE rather than store it: ~(VALID | DIRTY), less whatever is
	 * being erased. In 1a nothing is ever DIRTY or ERASING, so this must be
	 * bit-for-bit the live allocator's bitmap -- which is the whole point of
	 * the shadow step.
	 */
	if (ltram_free_bitmap) {
		bitmap_or(lt_scratch, lt_bm[LT_VALID], lt_bm[LT_DIRTY], ltram_nr_pages);
		bitmap_complement(lt_scratch, lt_scratch, ltram_nr_pages);
		if (lt_erasing != LT_ERASING_IDLE)
			__clear_bit(lt_erasing, lt_scratch);
		if (!bitmap_equal(lt_scratch, ltram_free_bitmap, ltram_nr_pages))
			err |= LT_ERR_FREE_MISMATCH;
	}
	return err;
}

/*
 * Report scratch. Static rather than on the stack: together these are ~1 KB,
 * which is most of a kernel frame, and every access is already serialised by
 * ltram_alloc_lock.
 *
 * LT_LG_BUCKETS is a log2 histogram -- bucket 0 is "never allocated", bucket k
 * is 2^(k-1) .. 2^k-1. A linear histogram cannot answer the question this file
 * exists for: at 1000-erase granularity, "65,535 pages at zero" and "32 pages
 * at 47" look identical, and both are what the low bucket held on 2026-08-25.
 */
#define LT_LG_BUCKETS	33
#define LT_TOPN		16
static unsigned int lt_lg[LT_LG_BUCKETS];
static unsigned int lt_blen[LT_EC_BUCKETS];
static struct { unsigned long idx; u32 ec; } lt_top[LT_TOPN];

static int lt_state_show(struct seq_file *m, void *v)
{
	unsigned long flags, i, counts[LT_NR_BM] = {0}, bucket_total = 0;
	unsigned long touched = 0;
	u32 ec_min = U32_MAX, ec_max = 0;
	u64 ec_sum = 0;
	unsigned int err, st, b;

	if (!lt_ready) {
		seq_puts(m, "page-state tracking not initialised\n");
		return 0;
	}

	/*
	 * The whole check runs under the allocator lock with interrupts off:
	 * ~65,536 iterations, a few hundred microseconds. Too long for a hot
	 * path, fine for a file a human reads. It is held throughout because a
	 * bucket list walked while another CPU splices it is a crash, and a
	 * half-consistent answer would be worse than useless.
	 */
	spin_lock_irqsave(&ltram_alloc_lock, flags);

	err = lt_check_fast();
	for (st = 0; st < LT_NR_BM; st++)
		counts[st] = bitmap_weight(lt_bm[st], ltram_nr_pages);

	memset(lt_lg, 0, sizeof(lt_lg));
	memset(lt_blen, 0, sizeof(lt_blen));
	memset(lt_top, 0, sizeof(lt_top));

	for (i = 0; i < ltram_nr_pages; i++) {
		u32 ec = lt_ec[i];
		unsigned int lg = ec ? min_t(unsigned int, fls(ec), LT_LG_BUCKETS - 1) : 0;
		int t;

		lt_lg[lg]++;
		ec_sum += ec;
		if (ec)
			touched++;
		if (ec < ec_min) ec_min = ec;
		if (ec > ec_max) ec_max = ec;

		/* Keep the LT_TOPN worst, sorted descending. Insertion sort over
		 * 16 entries: the compare rejects almost everything, so this is
		 * one branch per page in the common case. */
		if (ec > lt_top[LT_TOPN - 1].ec) {
			for (t = LT_TOPN - 1; t > 0 && lt_top[t - 1].ec < ec; t--)
				lt_top[t] = lt_top[t - 1];
			lt_top[t].idx = i;
			lt_top[t].ec  = ec;
		}
	}

	for (b = 0; b < LT_EC_BUCKETS; b++) {
		struct list_head *e;

		list_for_each(e, &lt_free_by_ec[b]) {
			unsigned long idx = e - lt_node;

			bucket_total++;
			lt_blen[b]++;
			if (idx >= ltram_nr_pages ||
			    test_bit(idx, lt_bm[LT_VALID]) ||
			    test_bit(idx, lt_bm[LT_DIRTY]) ||
			    lt_bucket_of(lt_ec[idx]) != b)
				err |= LT_ERR_BUCKET_MEMBER;
		}
	}
	if (bucket_total != lt_free_count)
		err |= LT_ERR_BUCKET_COUNT;

	spin_unlock_irqrestore(&ltram_alloc_lock, flags);

	seq_printf(m, "pages            %lu\n", ltram_nr_pages);
	seq_printf(m, "free             %lu   (bucket lists)\n", lt_free_count);
	for (st = 0; st < LT_NR_BM; st++)
		seq_printf(m, "%-16s %lu\n", lt_bm_name[st], counts[st]);
	seq_printf(m, "erasing          %s\n",
		   lt_erasing == LT_ERASING_IDLE ? "idle" : "yes");
	seq_printf(m, "free_walked      %lu   (independent count, must equal free)\n",
		   bucket_total);
	/*
	 * total_allocs is the load-bearing number for judging spread: it is the
	 * sum of every erase count, i.e. how many times ANY page was handed out.
	 * Compare it against touched -- allocations concentrated on a handful of
	 * pages give a large total against a tiny touched, which is exactly the
	 * signature lowest-first allocation produces.
	 */
	seq_printf(m, "total_allocs     %llu   (sum of all erase counts)\n", ec_sum);
	seq_printf(m, "touched          %lu   of %lu pages ever allocated (%lu%%)\n",
		   touched, ltram_nr_pages,
		   ltram_nr_pages ? touched * 100 / ltram_nr_pages : 0);
	seq_printf(m, "erase_count      min %u  max %u  spread %u\n",
		   ec_min == U32_MAX ? 0 : ec_min, ec_max,
		   ec_max - (ec_min == U32_MAX ? 0 : ec_min));

	seq_puts(m, "ec_histogram     log2; bucket k is 2^(k-1)..2^k-1\n");
	if (lt_lg[0])
		seq_printf(m, "  %14s %u\n", "never", lt_lg[0]);
	for (b = 1; b < LT_LG_BUCKETS; b++)
		if (lt_lg[b])
			seq_printf(m, "  [%6u..%6u] %u\n",
				   1u << (b - 1), (1u << b) - 1, lt_lg[b]);

	if (ec_max) {
		int t;

		seq_puts(m, "most_worn        page index : erase count\n");
		for (t = 0; t < LT_TOPN && lt_top[t].ec; t++)
			seq_printf(m, "  %10lu : %u\n", lt_top[t].idx, lt_top[t].ec);
	}

	seq_puts(m, "free_buckets     non-empty only\n");
	for (b = 0; b < LT_EC_BUCKETS; b++)
		if (lt_blen[b])
			seq_printf(m, "  [%6u..%6u] %u free\n",
				   b * LT_EC_GRAIN, (b + 1) * LT_EC_GRAIN - 1, lt_blen[b]);

	seq_printf(m, "invariant        %s\n", err ? "FAIL" : "ok");
	if (err & LT_ERR_OVERLAP)
		seq_puts(m, "  FAIL: a page is both VALID and DIRTY\n");
	if (err & LT_ERR_COUNT)
		seq_puts(m, "  FAIL: valid + dirty + free + erasing != page count\n");
	if (err & LT_ERR_FREE_MISMATCH)
		seq_puts(m, "  FAIL: derived FREE disagrees with the live allocator bitmap\n");
	if (err & LT_ERR_BUCKET_COUNT)
		seq_puts(m, "  FAIL: bucket lengths do not sum to lt_free_count\n");
	if (err & LT_ERR_BUCKET_MEMBER)
		seq_puts(m, "  FAIL: a bucketed page is VALID/DIRTY, or is in the wrong bucket\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lt_state);

/* Sized by the window, allocated once, never resized. ~1.3 MB for 256 MiB. */
static int __init lt_tracking_init(void)
{
	unsigned long i;
	unsigned int st, b;

	for (st = 0; st < LT_NR_BM; st++) {
		lt_bm[st] = bitmap_zalloc(ltram_nr_pages, GFP_KERNEL);
		if (!lt_bm[st])
			goto nomem;
	}
	lt_scratch = bitmap_zalloc(ltram_nr_pages, GFP_KERNEL);
	lt_ec   = kvcalloc(ltram_nr_pages, sizeof(*lt_ec), GFP_KERNEL);
	lt_node = kvmalloc_array(ltram_nr_pages, sizeof(*lt_node), GFP_KERNEL);
	if (!lt_scratch || !lt_ec || !lt_node)
		goto nomem;

	for (b = 0; b < LT_EC_BUCKETS; b++)
		INIT_LIST_HEAD(&lt_free_by_ec[b]);

	/*
	 * Every page starts FREE -- on bucket 0, since every erase count starts
	 * at 0 -- matching bitmap_fill() on the live allocator. Those counts are
	 * NOT the device's history and are not meant to be: this chip took heavy
	 * unknown wear during FPGA bring-up. Read them as "erases since counting
	 * began".
	 */
	for (i = 0; i < ltram_nr_pages; i++) {
		INIT_LIST_HEAD(&lt_node[i]);
		list_add_tail(&lt_node[i], &lt_free_by_ec[0]);
	}
	lt_free_count = ltram_nr_pages;
	lt_ready = true;

	if (ltram_debugfs_dir)
		debugfs_create_file("pagestate", 0444, ltram_debugfs_dir,
				    NULL, &lt_state_fops);
	pr_info("ltram: page-state tracking armed (shadow only), %lu pages\n",
		ltram_nr_pages);
	return 0;

nomem:
	for (st = 0; st < LT_NR_BM; st++) {
		bitmap_free(lt_bm[st]);
		lt_bm[st] = NULL;
	}
	bitmap_free(lt_scratch); lt_scratch = NULL;
	kvfree(lt_ec);   lt_ec = NULL;
	kvfree(lt_node); lt_node = NULL;
	pr_warn("ltram: page-state tracking disabled (out of memory)\n");
	return -ENOMEM;
}

static int __init ltram_policy_init(void)
{
	if (!ltram_end_pfn)
		return 0;

	ltram_nr_pages = ltram_end_pfn - ltram_start_pfn;
	ltram_free_bitmap = bitmap_zalloc(ltram_nr_pages, GFP_KERNEL);
	if (!ltram_free_bitmap)
		return -ENOMEM;
	bitmap_fill(ltram_free_bitmap, ltram_nr_pages);

	/* Shadow tracking. Failure here is not fatal: it costs the new
	 * bookkeeping and its assertions, not the ability to allocate. */
	lt_tracking_init();

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
