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
#include <linux/workqueue.h>
#include <linux/delay.h>

/* ---- tunables -------------------------------------------------------------
 * Defaults are deliberately conservative: the wear budget is 41.5 erases/s
 * sustained (65,536 sectors x 100,000 erases over five years), and a promotion
 * that turns out wrong costs two erases. A policy that promotes faster than it
 * can be wrong is a device-killer, so the batch cap matters more than the
 * interval.
 */
static unsigned int scan_interval_ms = 1000;
static unsigned int promote_batch = 32;	/* pages per pass; 32/s is under the budget */
/*
 * PTEs a single pass may examine before it stops and saves its place. Bounds
 * the work per pass independently of promote_batch, so a region where nothing
 * is eligible is swept quickly instead of being re-walked from the start.
 *
 * This, with scan_interval_ms, is what sets how long a page must sit
 * write-protected before it is judged read-mostly: a page is sighted once per
 * SWEEP, so the sweep time IS the observation window. At 8192 a 50,176-page
 * region sweeps in ~6 s. There is no separate streak counter -- see the note
 * above should_promote().
 */
static unsigned int scan_ptes_per_pass = 8192;
/*
 * Background erase. Hysteresis, not a single threshold: erasing starts when
 * free falls below the low mark and continues until it reaches the high one,
 * so the engine is not started and stopped once per allocation.
 *
 * The high mark is really "how big a burst can be absorbed without waiting for
 * an erase", and it is paid for up front -- at ~16.4 ms a sector, filling 8192
 * of them takes ~2.2 minutes of erasing.
 */
static unsigned int erase_low_water  = 2048;
static unsigned int erase_high_water = 8192;
static unsigned int erase_poll_ms    = 50;
/*
 * Erases per worker invocation. One per poll would cap the engine at
 * 1000/erase_poll_ms per second -- 20/s at a 50 ms poll -- when the device
 * sustains ~61/s. Each erase blocks ~16.4 ms, so 16 of them is ~260 ms in one
 * work item, which is why this runs on the unbound queue and re-arms with no
 * delay while there is still a backlog.
 */
static unsigned int erase_batch = 16;
/*
 * One sample cannot tell a quiet device from the gap between two reads, so the
 * device must look idle this many times in a row before a ~16.4 ms erase is
 * committed. Reads never reach the driver -- they are plain loads through the
 * cacheable window -- so the FPGA status word is the only visibility there is.
 */
static unsigned int idle_samples   = 3;
static unsigned int idle_sample_us = 1000;
module_param(scan_interval_ms, uint, 0644);
module_param(promote_batch, uint, 0644);
module_param(scan_ptes_per_pass, uint, 0644);
module_param(erase_low_water, uint, 0644);
module_param(erase_high_water, uint, 0644);
module_param(erase_poll_ms, uint, 0644);
module_param(erase_batch, uint, 0644);
module_param(idle_samples, uint, 0644);
module_param(idle_sample_us, uint, 0644);

/* ---- the flash page allocator --------------------------------------------- */
static unsigned long *ltram_free_bitmap;	/* 1 = free */
static unsigned long ltram_nr_pages;
static DEFINE_SPINLOCK(ltram_alloc_lock);
static atomic64_t stat_moved_to_dram;
static atomic64_t stat_freed_via_backstop, stat_freed_via_hook;
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

/* VALID -> FREE, for a sector that was never programmed. */
static void lt_to_free(unsigned long idx)
{
	if (!lt_ready)
		return;
	__clear_bit(idx, lt_bm[LT_VALID]);
	list_add_tail(&lt_node[idx], &lt_free_by_ec[lt_bucket_of(lt_ec[idx])]);
	lt_free_count++;
}

/*
 * VALID -> DIRTY. The sector holds data we wrote, so it cannot be handed out
 * again until it has been erased. The erase worker drains this.
 */
static void lt_to_dirty(unsigned long idx)
{
	if (!lt_ready)
		return;
	__clear_bit(idx, lt_bm[LT_VALID]);
	__set_bit(idx, lt_bm[LT_DIRTY]);
}

/* DIRTY -> ERASING. At most one at a time: the device erases one sector. */
static void lt_dirty_to_erasing(unsigned long idx)
{
	__clear_bit(idx, lt_bm[LT_DIRTY]);
	lt_erasing = idx;
}

/* ERASING -> FREE. The sector is blank and allocatable again. */
static void lt_erasing_to_free(unsigned long idx)
{
	lt_erasing = LT_ERASING_IDLE;
	list_add_tail(&lt_node[idx], &lt_free_by_ec[lt_bucket_of(lt_ec[idx])]);
	lt_free_count++;
	__set_bit(idx, ltram_free_bitmap);
	atomic64_dec(&ltram_pages_in_use);
}

/* ERASING -> DIRTY. The erase failed; put it back and try again later. */
static void lt_erasing_to_dirty(unsigned long idx)
{
	lt_erasing = LT_ERASING_IDLE;
	__set_bit(idx, lt_bm[LT_DIRTY]);
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

/*
 * @was_programmed: did this sector actually receive data?
 *
 * A page allocated as a migration destination whose migration then FAILED was
 * never written, so its sector is still blank and needs no erase -- it goes
 * straight back to FREE. Only a page that really held data goes to DIRTY. Get
 * this wrong in the generous direction and every failed migration costs a
 * 16.4 ms erase for nothing; get it wrong the other way and a sector is handed
 * out still holding the last tenant's data.
 */
static void ltram_free_page_back(struct page *p, bool was_programmed)
{
	unsigned long idx = page_to_pfn(p) - ltram_start_pfn;
	unsigned long flags;

	/* free_pages_prepare() can reach here from softirq and with interrupts
	 * already off, so this lock cannot be the plain variety. */
	spin_lock_irqsave(&ltram_alloc_lock, flags);
	if (!ltram_free_bitmap || idx >= ltram_nr_pages ||
	    test_bit(idx, ltram_free_bitmap))
		goto out;			/* not ours, or already free */

	if (was_programmed && lt_ready) {
		/* Stays out of ltram_free_bitmap: it is not allocatable until
		 * the erase worker has blanked it. */
		lt_to_dirty(idx);
	} else {
		__set_bit(idx, ltram_free_bitmap);
		atomic64_dec(&ltram_pages_in_use);
		lt_to_free(idx);
	}
out:
	spin_unlock_irqrestore(&ltram_alloc_lock, flags);
}

/* ---- the background erase engine ------------------------------------------
 *
 * THIS IS THE POINT OF THE WHOLE STATE MACHINE. ft_ltram_write_page() erases
 * synchronously before every program: ~16.4 ms of erase against ~1.2 ms of DMA,
 * so 93% of a promotion's cost is the erase, paid on the critical path. Pages
 * arriving pre-erased from a Free pool cost the DMA alone.
 *
 * It removes the erase from the LATENCY path, not from the THROUGHPUT budget:
 * sustained promotion is still capped near 61 pages/s by erase time. Ours is a
 * burst workload -- promote a working set, then it sits -- so we get the win.
 *
 * A workqueue, not a timer: the erase sleeps ~16.4 ms inside the backend
 * (waiting on the device's erase counter, not on a fixed delay), and a timer
 * callback runs in softirq where sleeping is forbidden.
 */
static void lt_erase_work_fn(struct work_struct *w);
static DECLARE_DELAYED_WORK(lt_erase_work, lt_erase_work_fn);
static atomic64_t stat_erases_done, stat_erases_failed, stat_erase_deferred;

/*
 * The device must look idle idle_samples times in a row. A single sample can
 * land in the gap between two reads and report quiet when the workload is
 * mid-stream -- and reads never reach the driver at all, being plain loads
 * through the cacheable window, so the FPGA's own status word is the only
 * visibility there is.
 */
static bool lt_device_quiet(void)
{
	unsigned int i;

	for (i = 0; i < idle_samples; i++) {
		if (!ltram_device_idle())
			return false;
		if (i + 1 < idle_samples)
			usleep_range(idle_sample_us, idle_sample_us * 2);
	}
	return true;
}

static void lt_erase_work_fn(struct work_struct *w)
{
	unsigned long flags, idx;
	bool have_op = ltram_have_erase_op();
	unsigned int done = 0;
	bool backlog = false;
	int rc;

	if (!lt_ready)
		goto again;

	while (done < erase_batch) {
		idx = ULONG_MAX;
		rc = 0;

		/*
		 * Hysteresis: start below the low mark, keep going until the
		 * high one. A single threshold would start and stop the engine
		 * once per allocation at the boundary.
		 */
		spin_lock_irqsave(&ltram_alloc_lock, flags);
		if (lt_erasing == LT_ERASING_IDLE &&
		    lt_free_count < erase_high_water) {
			unsigned long d = find_first_bit(lt_bm[LT_DIRTY],
							 ltram_nr_pages);

			if (d < ltram_nr_pages) {
				lt_dirty_to_erasing(d);
				idx = d;
			}
		}
		spin_unlock_irqrestore(&ltram_alloc_lock, flags);

		if (idx == ULONG_MAX)
			break;			/* nothing to do, or at the high mark */

		if (have_op) {
			if (!lt_device_quiet()) {
				/* Put it back rather than erase into a read
				 * stream; try again on the next tick. */
				spin_lock_irqsave(&ltram_alloc_lock, flags);
				lt_erasing_to_dirty(idx);
				spin_unlock_irqrestore(&ltram_alloc_lock, flags);
				atomic64_inc(&stat_erase_deferred);
				break;
			}
			rc = ltram_erase_page(ltram_start_pfn + idx);
		}
		/*
		 * No erase op: the sector still has to be recycled, and that is
		 * safe because write_page() erases inline before every program.
		 * One wasted erase per reuse -- the price of doing this in two
		 * steps rather than one flag day.
		 */

		spin_lock_irqsave(&ltram_alloc_lock, flags);
		if (rc) {
			lt_erasing_to_dirty(idx);
			atomic64_inc(&stat_erases_failed);
		} else {
			lt_erasing_to_free(idx);
			if (have_op)
				atomic64_inc(&stat_erases_done);
		}
		spin_unlock_irqrestore(&ltram_alloc_lock, flags);

		if (rc)
			break;
		done++;
		backlog = true;
		cond_resched();
	}

again:
	/* Still draining -> come straight back. Idle -> tick slowly. */
	queue_delayed_work(system_unbound_wq, &lt_erase_work,
			   backlog ? 0 : msecs_to_jiffies(erase_poll_ms));
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
	ltram_free_page_back(&dst->page, false);	/* never programmed */
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
	atomic64_inc(&stat_freed_via_hook);
	ltram_free_page_back(&folio->page, true);
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
	atomic64_inc(&stat_freed_via_backstop);
	ltram_free_page_back(page, true);
}

void ltram_note_demotion(void)
{
	atomic64_inc(&stat_moved_to_dram);
}

/* ---- per-page observation -------------------------------------------------
 * Keyed by pfn. Small and sparse: only pages of the target that have been seen
 * clean at least once appear here.
 */
/*
 * NO PER-PAGE OBSERVATION STATE.
 *
 * A page used to need N consecutive clean sightings, which meant a counter per
 * pfn in an xarray. That counter was never erased -- one kzalloc per distinct
 * pfn ever scanned, retained for the life of the kernel and growing with every
 * process targeted -- and it cost an xa_load on every PTE of every pass.
 *
 * It is not needed. "Not writable when we looked" is the whole test. How much
 * confidence that carries is a function of how long the page had been
 * write-protected before we looked, and that is already controlled by
 * scan_interval_ms and scan_ptes_per_pass, which together set how long a sweep
 * takes to come back around. One knob instead of two, and no metadata.
 *
 * Note the minimum is still TWO sightings: the first finds the page writable
 * and arms it, the second confirms nothing trapped. That is inherent -- a page
 * cannot be known read-only until it has been protected and left alone.
 */

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
	bool (*should_promote)(struct folio *f, bool written);
	void (*pass_end)(void);
};

static bool clean_run_should_promote(struct folio *f, bool written)
{
	return !written;
}

static const struct ltram_policy policy_clean_run = {
	.name           = "clean-run",
	.should_promote = clean_run_should_promote,
};

static const struct ltram_policy *policy = &policy_clean_run;

/* ---- counters ------------------------------------------------------------- */
static atomic64_t stat_ptes_examined, stat_moved_to_ltram, stat_not_moved;

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
 *                    clean. If this stays high while stat_write_protected also climbs, the
 *                    re-arm is firing and not clearing anything, which is a
 *                    DIFFERENT bug from the hypothesis and is not fixed by it.
 *   (there is no clean-streak counter: one clean sighting is the test)  The
 *                    policy is working and just needs more passes.
 *   stat_chosen     chosen and isolated -- became a candidate.
 *   stat_lru_refused chosen but folio_isolate_lru() refused. Invisible before.
 *   stat_write_protected          the write-protect actually executed.
 */
static atomic64_t stat_skipped_file_backed;
static atomic64_t stat_was_written,
		  stat_chosen, stat_lru_refused, stat_write_protected, stat_dirty_but_readonly;

/* ---- targeting ------------------------------------------------------------ */
static pid_t target_pid;
static DEFINE_MUTEX(target_lock);

/* ---- the scan ------------------------------------------------------------- */
struct scan_ctx {
	struct list_head candidates;
	unsigned int nr;		/* candidates picked this pass */
	unsigned int examined;		/* PTEs looked at this pass */
	unsigned long next_addr;	/* where to resume next pass */
	bool stopped;			/* hit a limit rather than the end */
};

/*
 * Where the next pass resumes.
 *
 * Before this existed the walk restarted at address 0 EVERY pass and stopped
 * once the batch filled, so anything above that point was never examined at
 * all -- a second large region above the first would have starved for ever,
 * and the only reason it did not is that mmap happened to place the region we
 * cared about lowest. It also meant the same low PTEs were re-examined every
 * second, which is why ptes_examined reached 13.2 million over an address
 * space holding 50,176 pages.
 */
static unsigned long scan_cursor;
static atomic64_t stat_sweeps;		/* times the cursor wrapped */

static int scan_pte_range(pmd_t *pmd, unsigned long addr, unsigned long end,
			  struct mm_walk *walk)
{
	struct scan_ctx *ctx = walk->private;
	pte_t *pte, *start_pte;
	spinlock_t *ptl;

	start_pte = pte = pte_offset_map_lock(walk->mm, pmd, addr, &ptl);
	if (!pte)
		return 0;

	for (; addr < end && ctx->nr < promote_batch &&
	       ctx->examined < scan_ptes_per_pass; addr += PAGE_SIZE, pte++) {
		ctx->examined++;
		pte_t p = ptep_get(pte);
		struct folio *folio;
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
			atomic64_inc(&stat_skipped_file_backed);
			continue;
		}
		if (!folio_try_get(folio))
			continue;

		atomic64_inc(&stat_ptes_examined);

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
			atomic64_inc(&stat_dirty_but_readonly);

		if (policy->should_promote(folio, written)) {
			if (!folio_isolate_lru(folio)) {
				atomic64_inc(&stat_lru_refused);
				goto rearm;
			}
			atomic64_inc(&stat_chosen);
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
			atomic64_inc(&stat_was_written);

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
			atomic64_inc(&stat_write_protected);
		}
		folio_put(folio);
	}
	pte_unmap_unlock(start_pte, ptl);
	cond_resched();

	/*
	 * Save the place, and stop the whole walk if a budget ran out. A
	 * non-zero return terminates walk_page_range(), which is what keeps the
	 * cursor meaningful: without it the walk carries on into the next PMD
	 * with both budgets already spent, examines nothing there, and the
	 * cursor ends up past pages it never looked at.
	 */
	ctx->next_addr = addr;
	if (addr < end) {
		ctx->stopped = true;
		return 1;
	}
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

	ctx.next_addr = scan_cursor;
	mmap_read_lock(mm);
	walk_page_range(mm, scan_cursor, TASK_SIZE, &scan_ops, &ctx);
	mmap_read_unlock(mm);

	/*
	 * Stopped on a budget -> resume exactly there. Ran off the end -> the
	 * sweep is done, wrap to 0. A pass that fills promote_batch advances the
	 * cursor by only that many pages, which is correct: it still makes
	 * forward progress, where the old code threw the position away and
	 * re-walked the same pages a second later.
	 */
	if (ctx.stopped) {
		scan_cursor = ctx.next_addr;
	} else {
		scan_cursor = 0;
		atomic64_inc(&stat_sweeps);
	}
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
	atomic64_add(moved, &stat_moved_to_ltram);
	atomic64_add(ctx.nr - moved, &stat_not_moved);
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
	/*
	 * Reset the cursor only when a DIFFERENT target arrives -- a new process
	 * is a new address space, so a carried-over position means nothing.
	 * Detaching (pid 0) deliberately leaves it alone: resetting there wiped
	 * the cursor before it could be read after a run, which made the whole
	 * mechanism unobservable.
	 */
	if (pid && pid != target_pid)
		scan_cursor = 0;
	target_pid = pid;
	mutex_unlock(&target_lock);
	pr_info("ltram: target pid %d\n", pid);
	return n;
}

static ssize_t stats_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return sysfs_emit(buf,
		"policy               %s\n"
		"target_pid           %d\n"
		"ptes_examined        %lld\n"
		"moved_to_ltram       %lld\n"
		"not_moved_this_pass  %lld\n"
		"moved_to_dram        %lld\n"
		"pages_in_use         %lld\n"
		"pages_total          %lu\n"
		"was_written          %lld\n"
		"dirty_but_readonly   %lld\n"
		"chosen               %lld\n"
		"lru_refused          %lld\n"
		"write_protected      %lld\n"
		"freed_via_backstop   %lld\n"
		"freed_via_hook       %lld\n"
		"skipped_file_backed  %lld\n"
		"sweeps               %lld\n"
		"scan_cursor          0x%lx\n"
		"erases_done          %lld\n"
		"erases_failed        %lld\n"
		"erase_deferred       %lld\n",
		policy->name, target_pid,
		atomic64_read(&stat_ptes_examined), atomic64_read(&stat_moved_to_ltram),
		atomic64_read(&stat_not_moved), atomic64_read(&stat_moved_to_dram),
		atomic64_read(&ltram_pages_in_use), ltram_nr_pages,
		atomic64_read(&stat_was_written),
		atomic64_read(&stat_dirty_but_readonly), atomic64_read(&stat_chosen),
		atomic64_read(&stat_lru_refused), atomic64_read(&stat_write_protected),
		atomic64_read(&stat_freed_via_backstop), atomic64_read(&stat_freed_via_hook),
		atomic64_read(&stat_skipped_file_backed),
		atomic64_read(&stat_sweeps), scan_cursor,
		atomic64_read(&stat_erases_done), atomic64_read(&stat_erases_failed),
		atomic64_read(&stat_erase_deferred));
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

	seq_printf(m, "pages                %lu\n", ltram_nr_pages);
	seq_printf(m, "free                 %lu   (bucket lists)\n", lt_free_count);
	for (st = 0; st < LT_NR_BM; st++)
		seq_printf(m, "%-20s %lu\n", lt_bm_name[st], counts[st]);
	seq_printf(m, "erasing              %s\n",
		   lt_erasing == LT_ERASING_IDLE ? "idle" : "yes");
	seq_printf(m, "free_from_bucketlist %lu   (recount, must equal free)\n",
		   bucket_total);
	/*
	 * total_allocs is the load-bearing number for judging spread: it is the
	 * sum of every erase count, i.e. how many times ANY page was handed out.
	 * Compare it against touched -- allocations concentrated on a handful of
	 * pages give a large total against a tiny touched, which is exactly the
	 * signature lowest-first allocation produces.
	 */
	seq_printf(m, "total_allocs         %llu   (sum of all erase counts)\n", ec_sum);
	seq_printf(m, "pages_ever_used      %lu   of %lu, ever allocated (%lu%%)\n",
		   touched, ltram_nr_pages,
		   ltram_nr_pages ? touched * 100 / ltram_nr_pages : 0);
	seq_printf(m, "erase_count          min %u  max %u  spread %u\n",
		   ec_min == U32_MAX ? 0 : ec_min, ec_max,
		   ec_max - (ec_min == U32_MAX ? 0 : ec_min));

	seq_puts(m, "ec_histogram         log2; bucket k is 2^(k-1)..2^k-1\n");
	if (lt_lg[0])
		seq_printf(m, "  %14s %u\n", "never", lt_lg[0]);
	for (b = 1; b < LT_LG_BUCKETS; b++)
		if (lt_lg[b])
			seq_printf(m, "  [%6u..%6u] %u\n",
				   1u << (b - 1), (1u << b) - 1, lt_lg[b]);

	if (ec_max) {
		int t;

		seq_puts(m, "most_worn            page index : erase count\n");
		for (t = 0; t < LT_TOPN && lt_top[t].ec; t++)
			seq_printf(m, "  %10lu : %u\n", lt_top[t].idx, lt_top[t].ec);
	}

	seq_puts(m, "free_buckets         non-empty only\n");
	for (b = 0; b < LT_EC_BUCKETS; b++)
		if (lt_blen[b])
			seq_printf(m, "  [%6u..%6u] %u free\n",
				   b * LT_EC_GRAIN, (b + 1) * LT_EC_GRAIN - 1, lt_blen[b]);

	seq_printf(m, "invariant            %s\n", err ? "FAIL" : "ok");
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
	queue_delayed_work(system_unbound_wq, &lt_erase_work,
			   msecs_to_jiffies(erase_poll_ms));
	pr_info("ltram: page states armed, %lu pages, erase worker running\n",
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
