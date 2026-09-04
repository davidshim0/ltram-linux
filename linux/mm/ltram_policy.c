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
#include <linux/ktime.h>
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
static unsigned int promote_batch = 1;	/* ONE: the interval sets the rate */
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
 * THE WEAR BUDGET.
 *
 * Endurance, not throughput, is what limits this device. A program takes
 * ~1.2 ms, so the hardware would sustain ~830 promotions a second all day; the
 * datasheet's 100,000 erases per sector is what actually runs out. Spread the
 * array's whole budget evenly across the intended service life and the
 * sustainable rate falls out of one division:
 *
 *	rate = erases_left / seconds_left
 *
 * 65,536 sectors x 100,000 cycles over five years is 41.5/s -- one promotion
 * every 24 ms. Self-correcting with no controller and nothing to tune:
 * overspend and erases_left falls, so the rate falls; sit idle and
 * seconds_left falls while erases_left does not, so the rate rises. Budget a
 * tick does not spend is not banked anywhere, it simply stays in erases_left
 * and pays for a faster rate later.
 *
 * wear_epoch is when the service life started, in seconds since the Unix
 * epoch, and it is writable so the rate can be moved without a rebuild.
 * Setting it in the PAST leaves less time and promotes faster; setting it in
 * the FUTURE leaves more and promotes slower. For ~1.5 ms, about the fastest
 * the device can physically program, the life left has to be ~114 days --
 * so move the epoch back by roughly wear_days - 114.
 *
 * Default: 2026-08-29 00:00:00 UTC, when counting began on this board.
 *
 * erases_used is a LOWER BOUND -- an unclean shutdown loses whatever accrued
 * since the last save, and the wear this chip took during FPGA bring-up was
 * never counted at all. So the budget is optimistic by an unknown amount.
 */
static unsigned long wear_epoch    = 1787961600UL;
static unsigned int  wear_days     = 1826;	/* five years */
static unsigned int  wear_cycles   = 100000;	/* datasheet, per sector */
static unsigned int  wear_governor = 1;
/*
 * How long to wait when there is no clean sector to promote INTO. Scanning
 * then can only find candidates nothing can be allocated for, and nothing is
 * likely to free a sector in the next millisecond either.
 */
static unsigned int  scan_stall_ms = 1000;
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
/*
 * How long to wait before looking again when there is NOTHING to erase. Only
 * the idle case: with a backlog the worker re-arms at zero delay and the device
 * sets the pace.
 *
 * TODO: make this event driven and stop polling. Nothing tells the worker that
 * free has crossed the low mark; it finds out on its next tick, so this value
 * is purely how stale that answer may be. Having lt_to_data() call
 * queue_delayed_work(..., 0) when lt_clean_count drops below erase_low_water
 * would make the wait exact and remove idle wakeups entirely. queue_delayed_work()
 * does not sleep, so it is safe under ltram_alloc_lock. The cost is a branch on
 * the allocation path and the allocator having to know the erase engine exists.
 */
static unsigned int erase_poll_ms    = 30;
/*
 * The idle tick, used only when the engine is OFF: free is above the high mark
 * and no erase is wanted. That is the steady state of a machine that has
 * finished a workload -- tonight it sat at free 60633, dirty 4903, engine off,
 * and would have woken 33 times a second forever to decide it had nothing to
 * do. Once a second is plenty.
 *
 * It is safe because of what the low mark buys. At the sustained promotion rate
 * the wear budget allows, ~41/s, draining 2048 free pages takes about 50
 * seconds, so starting the engine up to a second late costs 2% of the buffer.
 * That stops being true if promote_batch is raised for an experiment: at 512/s
 * the same buffer lasts 4 seconds. It does not matter there either, but for a
 * different reason -- the device only erases ~61/s, so at those rates the pool
 * exhausts no matter how promptly the engine starts.
 */
static unsigned int erase_idle_ms    = 1000;
/*
 * Erases per worker invocation. ONE, because batching buys nothing and costs
 * the thing that matters.
 *
 * It buys nothing because the delay, not the batch, sets the rate: an erase
 * blocks ~16.4 ms inside the worker, so one erase at a 0 ms re-arm and sixteen
 * back to back both run at the device's ~61/s.
 *
 * It costs read latency. NOR cannot serve a read while a sector is erasing, so
 * sixteen erases back to back is a quarter of a second during which every read
 * that misses cache waits, up to 16.4 ms each. One erase per 30 ms leaves a
 * 13.6 ms window after each one for reads to get through, and 33/s still
 * matches the demand: every dirty sector exists because a page was promoted, so
 * the erase rate is pinned to the promotion rate, which is 32/s at the default
 * promote_batch and is capped at 41.5/s by the wear budget regardless. The
 * spare capacity up to 61/s was never needed.
 *
 * The idle gate cannot substitute for this spacing. It samples the device over
 * 3 ms and then commits to a 16.4 ms operation, so it can say the device was
 * quiet a moment ago and cannot say nothing will arrive during the erase.
 * Spacing is the only mechanism that actually limits interference.
 */
static unsigned int erase_batch = 1;
/*
 * One sample cannot tell a quiet device from the gap between two reads, so the
 * device must look idle this many times in a row before a ~16.4 ms erase is
 * committed. Reads never reach the driver -- they are plain loads through the
 * cacheable window -- so the FPGA status word is the only visibility there is.
 */
static unsigned int idle_samples   = 3;
static unsigned int idle_sample_us = 1000;
/*
 * Census mode. Walk and write-protect exactly as always, count what WOULD have
 * been promoted, and promote nothing. The point is to measure how much of a
 * process is read-mostly using the real scanner, without the placement system
 * altering the thing being measured -- pages moving to flash change access
 * latency, and a pool that fills changes the walk. Off by default: with
 * scan_only = 0 every path below is the shipped one.
 */
static bool scan_only;
module_param(scan_interval_ms, uint, 0644);
module_param(promote_batch, uint, 0644);
module_param(scan_ptes_per_pass, uint, 0644);

module_param(wear_epoch, ulong, 0644);
module_param(wear_days, uint, 0644);
module_param(wear_cycles, uint, 0644);
module_param(wear_governor, uint, 0644);
module_param(scan_stall_ms, uint, 0644);
module_param(erase_low_water, uint, 0644);
module_param(erase_high_water, uint, 0644);
module_param(erase_poll_ms, uint, 0644);
module_param(erase_idle_ms, uint, 0644);
module_param(erase_batch, uint, 0644);
module_param(idle_samples, uint, 0644);
module_param(idle_sample_us, uint, 0644);
module_param(scan_only, bool, 0644);

/* ---- the flash page allocator --------------------------------------------- */
static unsigned long *ltram_clean_bitmap;	/* 1 = free */
static unsigned long ltram_nr_pages;
static DEFINE_SPINLOCK(ltram_alloc_lock);
static atomic64_t stat_moved_to_dram;
static atomic64_t stat_freed_via_backstop, stat_freed_via_hook;
/*
 * Destinations handed to migrate_pages(), and destinations it handed back.
 *
 * A flash page allocated as a migration destination is released either by
 * migrate_pages() calling put_new_folio on failure, or by the folio's last
 * reference going away after a successful migration. If dst_allocated exceeds
 * dst_released + moved_to_ltram, a failure path returned WITHOUT releasing the
 * destination, and that page is stranded: refcount 1, no mapping, no LRU, never
 * freed by anything.
 *
 * Eight such pages were found on 2026-08-26, at consecutive pfns immediately
 * after the last correctly-released one -- the signature of a batch in flight.
 * This pair turns that from an inference into an equation.
 */
static atomic64_t stat_dst_allocated, stat_dst_released;
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
#define LT_EC_BUCKETS	101u
/*
 * Bucket width, in erases. 101 buckets at the default 1,000 covers the whole
 * 100,000-cycle endurance of a sector.
 *
 * WRITABLE, because at the default the bucket machinery is unreachable. Real
 * counts sit around 20-35, so every sector is in bucket 0 and stays there
 * until one reaches 1,000 -- roughly 65 million erases away. Inside a bucket
 * the free list is FIFO, so what actually runs today is round-robin, and
 * lt_bucket_of() plus the per-bucket lists are dead code that cannot be
 * exercised without burning through the chip.
 *
 * Set it to 10 and the same counts spread across three or four buckets at
 * once, so "least worn first" becomes observable in seconds instead of years.
 * Changing it moves every free page to a different bucket, so the setter
 * rebuilds the lists -- writing it without that would leave lt_pop_clean()
 * handing out by the old order and trip LT_ERR_BUCKET_MEMBER.
 */
static unsigned int ec_grain = 1000;
#define LT_ERASING_IDLE	0xFFFFFFFFu

enum lt_bmstate { LT_DATA = 0, LT_DIRTY, LT_NR_BM };
static const char * const lt_bm_name[LT_NR_BM] = { "data", "dirty" };

static unsigned long	*lt_bm[LT_NR_BM];	/* VALID and DIRTY only */
static unsigned long	*lt_scratch;		/* derivations and ANDs */
/*
 * Has this sector been PROGRAMMED since its last erase?
 *
 * Not the same question as DATA or DIRTY, which is why it needs its own bit.
 * A destination is allocated (DATA, blank), then written, then the migration
 * either completes or fails. On failure migrate_pages() hands the page back
 * through put_new_folio, and until 2026-08-28 that path assumed the sector was
 * never touched and returned it straight to CLEAN.
 *
 * It is touched whenever ltram_copy_to_flash() ran before the mapping move
 * failed, which is 24% of migrations in a sweep. Those sectors went back into
 * the clean pool holding data, and with inline_erase=0 the next promotion
 * programmed on top of them. verify_erased caught it; nothing else would have.
 */
static u32		*lt_ec;			/* erase count, per page */
static struct list_head	*lt_node;		/* per-page link into a bucket */
static struct list_head	 lt_clean_by_ec[LT_EC_BUCKETS];
static unsigned long	 lt_clean_count;		/* O(1) for the watermark */
/*
 * Erases since counting began: the sum of lt_ec[], maintained incrementally
 * because 65,536 adds is too much to repeat on every scan tick just to divide
 * by it. It must equal that sum exactly -- selftest A3 asserts it, and the
 * whole wear budget is computed from it.
 *
 * Counted at ALLOCATION, in lt_to_data(), which is where lt_ec[] itself is
 * counted. That is an upper bound on erases: every allocated sector is erased
 * before it is programmed, so the two are one-to-one except when a migration
 * fails after allocating, which over-counts by one. Wrong in the conservative
 * direction, and identical to what the persisted blob holds.
 */
static atomic64_t	 lt_ec_total;
static u32		 lt_erasing = LT_ERASING_IDLE;
static bool		 lt_ready;

static inline unsigned int lt_bucket_of(u32 ec)
{
	unsigned int b = ec / (ec_grain ? ec_grain : 1);

	return b < LT_EC_BUCKETS ? b : LT_EC_BUCKETS - 1;
}

static void lt_rebuild_buckets(void);
static int lt_set_ec_grain(const char *val, const struct kernel_param *kp)
{
	unsigned int old = ec_grain;
	int rc = param_set_uint(val, kp);

	if (rc)
		return rc;
	if (!ec_grain) {
		ec_grain = old;
		return -EINVAL;
	}
	if (ec_grain != old && lt_ready)
		lt_rebuild_buckets();	/* every page changes bucket */
	return 0;
}
static const struct kernel_param_ops lt_ec_grain_ops = {
	.set = lt_set_ec_grain,
	.get = param_get_uint,
};
module_param_cb(ec_grain, &lt_ec_grain_ops, &ec_grain, 0644);

/*
 * FREE -> VALID. The erase count is incremented HERE, which is an upper bound:
 * in the current design every allocated page is erased by ft_ltram_write_page()
 * before it is programmed, so allocation and erase are one-to-one -- except
 * when a migration fails after allocating, which over-counts by one. Wrong in
 * the conservative direction, and replaced by an erase-completion hook in 1c.
 *
 * Caller holds ltram_alloc_lock.
 */
static void lt_to_data(unsigned long idx)
{
	if (!lt_ready)
		return;
	list_del_init(&lt_node[idx]);		/* off its free bucket */
	lt_clean_count--;
	__set_bit(idx, lt_bm[LT_DATA]);
	if (lt_ec[idx] < U32_MAX)
		lt_ec[idx]++;
	/*
	 * And the running total, HERE, with the per-sector count it must equal.
	 *
	 * This lived in lt_erasing_to_clean() at first, which counts a different
	 * event: erase COMPLETIONS rather than allocations. The two differ by
	 * however many sectors are allocated but not yet erased, so the total
	 * drifted below the array by exactly the current data+dirty -- measured
	 * 4,219 against a pool that had 57,344 dirty at boot and 61,563 later.
	 *
	 * It also made restore incoherent: lt_rebuild_buckets() seeds the total
	 * from sum(lt_ec[]), the allocation basis, after which it accrued on the
	 * erase basis and drifted again from the moment it was made correct.
	 *
	 * The wear budget divides by this, and undercounting overstates
	 * erases_left, which lets the governor promote FASTER than the budget
	 * allows -- the unsafe direction.
	 */
	atomic64_inc(&lt_ec_total);
}


/*
 * VALID -> DIRTY. The sector holds data we wrote, so it cannot be handed out
 * again until it has been erased. The erase worker drains this.
 */
static void lt_to_dirty(unsigned long idx)
{
	if (!lt_ready)
		return;
	__clear_bit(idx, lt_bm[LT_DATA]);
	__set_bit(idx, lt_bm[LT_DIRTY]);
}

/*
 * Mark which DIRTY page has an erase in flight. NOT a state transition: the
 * page stays in the DIRTY bitmap throughout.
 *
 * Erasing is something happening TO a dirty page, not a place a page can be.
 * Treating it as a fourth state cost a fourth term in the count invariant and
 * an exception in the derived CLEAN set, and bought nothing: the worker already
 * refuses to select a second page while this marker is set, so nothing ever
 * depended on the page having left DIRTY.
 */
static void lt_mark_erasing(unsigned long idx)
{
	lt_erasing = idx;
}

/* The erase retired. Now the sector is blank and allocatable. */
static void lt_erasing_to_clean(unsigned long idx)
{
	lt_erasing = LT_ERASING_IDLE;
	__clear_bit(idx, lt_bm[LT_DIRTY]);
	list_add_tail(&lt_node[idx], &lt_clean_by_ec[lt_bucket_of(lt_ec[idx])]);
	lt_clean_count++;
	__set_bit(idx, ltram_clean_bitmap);
	atomic64_dec(&ltram_pages_in_use);
}

/* Erase failed, or the idle gate refused. The page never left DIRTY, so just
 * release the marker and let the worker try again on a later tick. */
static void lt_erasing_clear(unsigned long idx)
{
	lt_erasing = LT_ERASING_IDLE;
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
static unsigned long lt_pop_clean(void)
{
	unsigned int b;

	for (b = 0; b < LT_EC_BUCKETS; b++)
		if (!list_empty(&lt_clean_by_ec[b]))
			return lt_clean_by_ec[b].next - lt_node;
	return ltram_nr_pages;
}

static struct page *ltram_alloc_page(void)
{
	unsigned long idx, flags;
	struct page *p = NULL;

	spin_lock_irqsave(&ltram_alloc_lock, flags);
	/*
	 * STEP 1b: the bucket lists now DECIDE which page is handed out. The old
	 * ltram_clean_bitmap is still maintained in step, purely so the assertion
	 * in lt_check_fast() -- derived FREE must equal it bit for bit -- keeps
	 * cross-checking two independently updated structures. It is cheap, and
	 * it is the strongest check this subsystem has. Retire it only once the
	 * bucket lists have been trusted for a while.
	 */
	if (lt_ready) {
		idx = lt_pop_clean();
		if (idx < ltram_nr_pages) {
			__clear_bit(idx, ltram_clean_bitmap);
			p = pfn_to_page(ltram_start_pfn + idx);
			atomic64_inc(&ltram_pages_in_use);
			lt_to_data(idx);	/* removes it from its bucket */
		}
	} else if (ltram_clean_bitmap) {
		/* Tracking failed to allocate at init. Fall back to the old
		 * lowest-index allocator rather than refusing to work at all. */
		idx = find_first_bit(ltram_clean_bitmap, ltram_nr_pages);
		if (idx < ltram_nr_pages) {
			__clear_bit(idx, ltram_clean_bitmap);
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
static void ltram_free_page_back(struct page *p)
{
	unsigned long idx = page_to_pfn(p) - ltram_start_pfn;
	unsigned long flags;

	/* free_pages_prepare() can reach here from softirq and with interrupts
	 * already off, so this lock cannot be the plain variety. */
	spin_lock_irqsave(&ltram_alloc_lock, flags);
	if (!ltram_clean_bitmap || idx >= ltram_nr_pages ||
	    test_bit(idx, ltram_clean_bitmap))
		goto out;			/* not ours, or already free */

	/*
	 * DIRTY, always. Every path that releases an LtRAM page -- the write
	 * fault, the exit backstop, and a failed migration -- is releasing a
	 * sector that either holds data or cannot be proven not to. It stays
	 * out of ltram_clean_bitmap so it is not allocatable until the erase
	 * worker has blanked it.
	 *
	 * There is deliberately no "return it clean" branch any more. It
	 * existed for sectors that were allocated but never programmed, and the
	 * only way to identify those was a bit that a FAILED write never set.
	 */
	if (lt_ready)
		lt_to_dirty(idx);
out:
	spin_unlock_irqrestore(&ltram_alloc_lock, flags);
}

/*
 * Which process the scanner is watching, or 0 for none. Declared here rather
 * than with the rest of the targeting code because the erase worker reads it:
 * "is a workload running" is what decides whether erases may run flat out or
 * must leave gaps for reads.
 */
static pid_t target_pid;

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

/*
 * Hysteresis state. Latched, not recomputed from a single threshold: the
 * engine turns on below the low mark and stays on until free reaches the
 * high one. Between the two marks it keeps whatever state it had.
 * Guarded by ltram_alloc_lock.
 */
static bool lt_erase_engine_on;
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
	unsigned int delay_ms;
	int rc;

	if (!lt_ready)
		goto again;

	while (done < erase_batch) {
		idx = ULONG_MAX;
		rc = 0;

		spin_lock_irqsave(&ltram_alloc_lock, flags);

		/*
		 * Hysteresis: start below the low mark, keep going until the
		 * high one. A single threshold would start and stop the engine
		 * once per allocation at the boundary. Checking "off" first
		 * means a nonsensical low > high degrades to that single
		 * threshold rather than latching on forever.
		 */
		if (lt_clean_count >= erase_high_water)
			lt_erase_engine_on = false;
		else if (lt_clean_count < erase_low_water)
			lt_erase_engine_on = true;

		if (lt_erase_engine_on && lt_erasing == LT_ERASING_IDLE) {
			unsigned long d = find_first_bit(lt_bm[LT_DIRTY],
							 ltram_nr_pages);

			if (d < ltram_nr_pages) {
				lt_mark_erasing(d);
				idx = d;
			}
		}
		spin_unlock_irqrestore(&ltram_alloc_lock, flags);

		if (idx == ULONG_MAX)
			break;			/* nothing dirty, or engine off */

		/*
		 * No erase op means no way to blank a sector. Recycling anyway
		 * used to be safe, because write_page() erased inline before
		 * every program; with inline_erase=0 it is exactly the bug
		 * put_new_folio exists to prevent, one level up. Put it back and
		 * say so once: a pool that stops recovering is a visible
		 * problem, and a pool that recycles unerased sectors is not.
		 */
		if (!have_op) {
			static bool said;

			spin_lock_irqsave(&ltram_alloc_lock, flags);
			lt_erasing_clear(idx);
			spin_unlock_irqrestore(&ltram_alloc_lock, flags);
			if (!said) {
				said = true;
				pr_warn("ltram: no erase op registered -- dirty sectors cannot be recycled\n");
			}
			break;
		}

		{
			if (!lt_device_quiet()) {
				/* Put it back rather than erase into a read
				 * stream; try again on the next tick. */
				spin_lock_irqsave(&ltram_alloc_lock, flags);
				lt_erasing_clear(idx);
				spin_unlock_irqrestore(&ltram_alloc_lock, flags);
				atomic64_inc(&stat_erase_deferred);
				break;
			}
			rc = ltram_erase_page(ltram_start_pfn + idx);
		}

		spin_lock_irqsave(&ltram_alloc_lock, flags);
		if (rc) {
			lt_erasing_clear(idx);
			atomic64_inc(&stat_erases_failed);
		} else {
			lt_erasing_to_clean(idx);
			atomic64_inc(&stat_erases_done);
		}
		spin_unlock_irqrestore(&ltram_alloc_lock, flags);

		if (rc)
			break;
		done++;
		cond_resched();
	}

again:
	/*
	 * How fast to come back, decided by who is waiting on the device.
	 *
	 *   engine off              1000 ms  clean is above the high mark. Just
	 *                                    checking whether it has dropped.
	 *   a workload is attached    30 ms  POLITE. Leave a 13.6 ms window after
	 *                                    each erase for reads to get through.
	 *                                    33/s still matches promotion demand,
	 *                                    since every dirty sector exists
	 *                                    because a page was promoted.
	 *   nothing attached           0 ms  no reads to disturb. Drain at the
	 *                                    device's own ~61/s, which keeps a
	 *                                    full 65,536-sector wipe at ~18
	 *                                    minutes rather than 33.
	 *
	 * target_pid alone decides this, not the idle gate. The gate samples the
	 * DEVICE over 3 ms, and a compute-bound workload between two reads looks
	 * exactly like an idle machine for that long. target_pid is unambiguous:
	 * either something is attached or nothing is.
	 *
	 * One accepted cost. With nothing attached and the gate refusing anyway,
	 * because something outside the policy is using the device, this re-arms
	 * at 0 ms and spins on the gate at roughly 300 checks a second. Each is
	 * one readq, and it stops the moment the device frees up.
	 *
	 * Read without the lock. It cannot tear, and being one tick stale only
	 * picks a different polling interval.
	 */
	if (!lt_erase_engine_on)
		delay_ms = erase_idle_ms;
	else if (READ_ONCE(target_pid))
		delay_ms = erase_poll_ms;
	else
		delay_ms = 0;

	queue_delayed_work(system_unbound_wq, &lt_erase_work,
			   msecs_to_jiffies(delay_ms));
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
	if (p)
		atomic64_inc(&stat_dst_allocated);
	return p ? page_folio(p) : NULL;
}

/*
 * migrate_pages() handing back a destination it could not use.
 *
 * "Failed" does NOT mean "untouched". ltram_copy_to_flash() runs before
 * folio_migrate_mapping(), on purpose, so that a flash error aborts with
 * nothing published -- which means a mapping failure arrives here on a sector
 * that has already been programmed.
 */
static void ltram_put_new_folio(struct folio *dst, unsigned long private)
{
	/*
	 * ALWAYS dirty. The sector needs an erase, and asking whether it was
	 * actually programmed cannot be answered safely.
	 *
	 * migrate_pages() reaches here in four ways. Before the copy: lock
	 * contention, the source turning unmigratable, an early -EAGAIN --
	 * sector allocated, never programmed, still blank. After a successful
	 * copy: folio_migrate_mapping() returns -EAGAIN on an unexpected
	 * refcount -- sector programmed. Retry exhaustion resolves to one of
	 * those two.
	 *
	 * And the fourth: ltram_copy_to_flash() itself failed. That is the one
	 * that made the old test unsafe. ltram_write_page() only calls
	 * ltram_record_promotion() when rc == 0, so a failed write leaves
	 * lt_written CLEAR -- while the backend may have programmed the sector
	 * before failing, and a multi-page folio may have written earlier pages
	 * before erroring on a later one. The bit said "blank", the sector held
	 * data, and it went back to the clean pool to be programmed again
	 * without an erase: the bitwise AND of old and new, silently.
	 *
	 * So do not ask. A blank sector sent for erase costs one erase out of a
	 * budget of 6.55e9; a programmed sector called blank corrupts data.
	 * Wrong in the only direction that is survivable.
	 *
	 * Nothing is owed to the application either way. folio_migrate_mapping()
	 * never ran, so the PTE still points at the DRAM source and the process
	 * carries on using it, unaware.
	 */
	atomic64_inc(&stat_dst_released);
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
	atomic64_inc(&stat_freed_via_hook);
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
	atomic64_inc(&stat_freed_via_backstop);
	ltram_free_page_back(page);
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
		  stat_chosen, stat_lru_refused, stat_write_protected, stat_dirty_but_readonly,
		  stat_would_promote;

/* ---- targeting ------------------------------------------------------------ */
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
			/*
			 * Census mode stops here. Jumping to rearm rather than
			 * continuing is deliberate: nothing was isolated, so the
			 * reference this iteration took must be dropped by the
			 * folio_put at the bottom of the loop -- the not-chosen
			 * path's bookkeeping, not the candidate list's. A page
			 * that reaches here has written == false, so the re-arm
			 * itself is a no-op and the PTE stays as it was.
			 */
			if (scan_only) {
				atomic64_inc(&stat_would_promote);
				goto rearm;
			}
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

#define LT_WEAR_STOP	UINT_MAX

/*
 * Milliseconds between promotions, from erases_left / seconds_left. See the
 * wear_epoch comment above. LT_WEAR_STOP means the budget is gone.
 */
static unsigned int lt_wear_interval_ms(void)
{
	u64 total, used, left, ms;
	s64 secs_left;

	if (!wear_governor)
		return scan_interval_ms;

	total = (u64)ltram_nr_pages * (u64)wear_cycles;
	used  = (u64)atomic64_read(&lt_ec_total);
	if (used >= total)
		return LT_WEAR_STOP;

	secs_left = (s64)wear_epoch + (s64)wear_days * 86400 -
		    ktime_get_real_seconds();
	if (secs_left <= 0)
		return LT_WEAR_STOP;

	left = total - used;
	ms = div64_u64(1000ULL * (u64)secs_left, left);
	/*
	 * The device cannot program faster than ~1.2 ms whatever this says, and
	 * a zero would turn this thread into a spin.
	 */
	if (ms < 1)
		ms = 1;
	if (ms > 60000)
		ms = 60000;
	return (unsigned int)ms;
}

static int ltram_scan_thread(void *unused)
{
	while (!kthread_should_stop()) {
		unsigned int ms = lt_wear_interval_ms();

		/*
		 * Exhausted: stop promoting. Deliberately not a trickle. If the
		 * budget was spent correctly we never arrive here, so arriving
		 * means the arithmetic was wrong -- and a trickle would hide
		 * that rather than surface it.
		 */
		if (ms == LT_WEAR_STOP) {
			pr_warn_once("ltram: wear budget exhausted (%lld of %llu erases) -- promotion stopped\n",
				     atomic64_read(&lt_ec_total),
				     (u64)ltram_nr_pages * (u64)wear_cycles);
			msleep_interruptible(scan_stall_ms);
			continue;
		}

		/*
		 * THE INTERVAL IS A GAP, NOT A PERIOD. Sleep it, and on waking
		 * promote if
		 * there is somewhere to put a page and a candidate inside
		 * scan_ptes_per_pass. No token bucket and no deadline: a tick
		 * that finds nothing just sleeps again, and the budget it did
		 * not spend stays in cycles_left and buys a faster rate later.
		 *
		 * The real period is this sleep PLUS the work: a scan of up to
		 * scan_ptes_per_pass, then one migration -- a page copy, a ~1.2 ms
		 * flash program, and the TLB work. Measured at ~3 ms, and it fits
		 * both arms of the selftest with one constant:
		 *
		 *	5 ms sleep + 3 ms work = 8 ms -> 125/s   (measured 125.0)
		 *	1 ms sleep + 3 ms work = 4 ms -> 250/s   (measured 249.9)
		 *
		 * So the governor DELIVERS LESS than it budgets: 24 ms asks for
		 * 41.5/s and gets ~37/s, about 11% under. Deliberately left
		 * alone. Subtracting a measured work estimate would tighten the
		 * rate towards the ceiling, and the error is already in the only
		 * safe direction -- spending less wear than allowed, so the array
		 * outlives its target rather than falling short of it.
		 */
		msleep_interruptible(ms);
		if (kthread_should_stop())
			break;

		/*
		 * Nowhere to put a page. Scanning could only turn up candidates
		 * that cannot be allocated for, and a sector is unlikely to come
		 * free in the next millisecond.
		 */
		if (!scan_only && !READ_ONCE(lt_clean_count)) {
			msleep_interruptible(scan_stall_ms);
			continue;
		}

		ltram_scan_once();
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
		"would_promote        %lld\n"
		"lru_refused          %lld\n"
		"write_protected      %lld\n"
		"freed_via_backstop   %lld\n"
		"freed_via_hook       %lld\n"
		"dst_allocated        %lld\n"
		"dst_released         %lld\n"
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
		atomic64_read(&stat_would_promote),
		atomic64_read(&stat_lru_refused), atomic64_read(&stat_write_protected),
		atomic64_read(&stat_freed_via_backstop), atomic64_read(&stat_freed_via_hook),
		atomic64_read(&stat_dst_allocated), atomic64_read(&stat_dst_released),
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
 * The redundancy is the point. lt_clean_count and the bucket lists are two
 * independently maintained descriptions of the same set, and the derived free
 * set is a third that cannot drift because it is computed, not stored.
 * Asserting they agree turns a silent page loss into a loud one -- which is
 * exactly what was missing when 16,502 pages went astray.
 */
#define LT_ERR_OVERLAP		(1u << 0)
#define LT_ERR_COUNT		(1u << 1)
#define LT_ERR_CLEAN_MISMATCH	(1u << 2)
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

	bitmap_and(lt_scratch, lt_bm[LT_DATA], lt_bm[LT_DIRTY], ltram_nr_pages);
	if (!bitmap_empty(lt_scratch, ltram_nr_pages))
		err |= LT_ERR_OVERLAP;

	total = bitmap_weight(lt_bm[LT_DATA], ltram_nr_pages) +
		bitmap_weight(lt_bm[LT_DIRTY], ltram_nr_pages) +
		lt_clean_count;
	if (total != ltram_nr_pages)
		err |= LT_ERR_COUNT;

	/*
	 * Derive FREE rather than store it: ~(VALID | DIRTY), less whatever is
	 * being erased. In 1a nothing is ever DIRTY or ERASING, so this must be
	 * bit-for-bit the live allocator's bitmap -- which is the whole point of
	 * the shadow step.
	 */
	if (ltram_clean_bitmap) {
		bitmap_or(lt_scratch, lt_bm[LT_DATA], lt_bm[LT_DIRTY], ltram_nr_pages);
		bitmap_complement(lt_scratch, lt_scratch, ltram_nr_pages);
		if (!bitmap_equal(lt_scratch, ltram_clean_bitmap, ltram_nr_pages))
			err |= LT_ERR_CLEAN_MISMATCH;
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
	u32 erasing_snap;
	bool engine_on;
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

		list_for_each(e, &lt_clean_by_ec[b]) {
			unsigned long idx = e - lt_node;

			bucket_total++;
			lt_blen[b]++;
			if (idx >= ltram_nr_pages ||
			    test_bit(idx, lt_bm[LT_DATA]) ||
			    test_bit(idx, lt_bm[LT_DIRTY]) ||
			    lt_bucket_of(lt_ec[idx]) != b)
				err |= LT_ERR_BUCKET_MEMBER;
		}
	}
	if (bucket_total != lt_clean_count)
		err |= LT_ERR_BUCKET_COUNT;

	erasing_snap = lt_erasing;
	engine_on    = lt_erase_engine_on;

	spin_unlock_irqrestore(&ltram_alloc_lock, flags);

	seq_printf(m, "pages                %lu\n", ltram_nr_pages);
	seq_printf(m, "clean                %lu   (bucket lists)\n", lt_clean_count);
	for (st = 0; st < LT_NR_BM; st++)
		seq_printf(m, "%-20s %lu\n", lt_bm_name[st], counts[st]);
	/* Not a state: which DIRTY page, if any, has an erase in flight. */
	if (erasing_snap == LT_ERASING_IDLE)
		seq_puts(m, "erasing              none\n");
	else
		seq_printf(m, "erasing              page %u (still counted in dirty)\n",
			   erasing_snap);
	/*
	 * The engine is off whenever free is above the high mark -- so
	 * "erases_done 0" after a small workload is the design working, not a
	 * stalled worker. This line is the difference between the two.
	 */
	seq_printf(m, "erase_engine         %s   (on below %u free, off at %u)\n",
		   engine_on ? "ON" : "off", erase_low_water, erase_high_water);
	seq_printf(m, "clean_from_bucketlist %lu  (recount, must equal clean)\n",
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

	/*
	 * When only a handful of pages are VALID, dump them. Twice now a story
	 * about why they are stuck has been constructed from counter arithmetic
	 * alone, and twice it has been wrong -- refcount 2 -> 1 across a COW is
	 * ordinary, because LRU membership holds one of them. The state of the
	 * folio itself is the only thing that settles it:
	 *
	 *   refs 1, map 0, lru        -- unmapped and waiting for reclaim that
	 *                                never runs on an idle 125 GB machine
	 *   map > 0                   -- something still maps it; not stuck at all
	 *   refs > 1, map 0, no lru   -- a reference taken and never dropped
	 *
	 * Capped, so a mid-run read with thousands VALID does not print a novel.
	 */
	if (counts[LT_DATA] > 0 && counts[LT_DATA] <= 64) {
		unsigned long b;

		seq_puts(m, "data_detail          pfn : state (nothing should hold DATA with no owner)\n");
		for (b = find_first_bit(lt_bm[LT_DATA], ltram_nr_pages);
		     b < ltram_nr_pages;
		     b = find_next_bit(lt_bm[LT_DATA], ltram_nr_pages, b + 1)) {
			struct folio *f = page_folio(pfn_to_page(ltram_start_pfn + b));

			seq_printf(m, "  %10lu : refs %d  map %d  %s%s%s%s\n",
				   ltram_start_pfn + b,
				   folio_ref_count(f), folio_mapcount(f),
				   folio_test_lru(f)         ? "lru "        : "",
				   folio_test_anon(f)        ? "anon "       : "",
				   folio_test_swapbacked(f)  ? "swapbacked " : "",
				   folio_test_locked(f)      ? "locked "     : "");
		}
	}

	seq_puts(m, "clean_buckets        non-empty only\n");
	for (b = 0; b < LT_EC_BUCKETS; b++)
		if (lt_blen[b])
			seq_printf(m, "  [%6u..%6u] %u clean\n",
				   b * ec_grain, (b + 1) * ec_grain - 1, lt_blen[b]);

	seq_printf(m, "invariant            %s\n", err ? "FAIL" : "ok");
	if (err & LT_ERR_OVERLAP)
		seq_puts(m, "  FAIL: a page is both DATA and DIRTY\n");
	if (err & LT_ERR_COUNT)
		seq_puts(m, "  FAIL: data + dirty + clean != page count\n");
	if (err & LT_ERR_CLEAN_MISMATCH)
		seq_puts(m, "  FAIL: derived CLEAN disagrees with the live allocator bitmap\n");
	if (err & LT_ERR_BUCKET_COUNT)
		seq_puts(m, "  FAIL: bucket lengths do not sum to clean\n");
	if (err & LT_ERR_BUCKET_MEMBER)
		seq_puts(m, "  FAIL: a bucketed page holds DATA or is DIRTY, or is in the wrong bucket\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lt_state);

/*
 * Promotion provenance: for every sector currently holding live data, where
 * that data came from.
 *
 * /proc/pid/pagemap answers the forward question, virtual to physical, for a
 * process that is still alive. This is the reverse, and it outlives the
 * process: given a sector, what was put there, from which pfn, for which
 * target, and in what order.
 *
 * The order is the part that earns its keep. On 2026-08-27 a single promotion
 * segfaulted the workload and identifying WHICH sector took an evening of
 * reasoning. Sector 0 had been programmed directly by test=45, outside the
 * state machine, so the kernel still believed it blank and handed it out first.
 * A line saying "sector 0, seq 1" would have made that immediate.
 *
 * Overwritten in place, so it is a snapshot of what is live rather than a
 * history. 12 bytes a sector, 768 KB for a 256 MiB window.
 */
struct lt_prov {
	u32 src_pfn;		/* the DRAM page it came from */
	u32 seq;		/* promotion order, monotonic since boot */
	pid_t pid;		/* which target it was promoted for */
};
static struct lt_prov *lt_prov;
static atomic_t lt_prov_seq;

void ltram_record_promotion(unsigned long dst_pfn, unsigned long src_pfn)
{
	unsigned long idx = dst_pfn - ltram_start_pfn;

	if (!lt_prov || idx >= ltram_nr_pages)
		return;
	/*
	 * No lock. Three independent u32 stores, so a concurrent reader can see
	 * a row half from one promotion and half from the next. That is a debug
	 * view of a moving target; taking ltram_alloc_lock on the write path for
	 * it would be paying a real cost for a cosmetic one.
	 */
	/*
	 * No "was it programmed" bit any more. It was read in exactly one place
	 * -- ltram_put_new_folio() -- which now sends every released sector to
	 * DIRTY regardless, because a FAILED write never set the bit while the
	 * sector might still hold data. Setting it here also took
	 * ltram_alloc_lock on the promotion path for a purely advisory answer.
	 */
	lt_prov[idx].src_pfn = (u32)src_pfn;
	lt_prov[idx].seq     = (u32)atomic_inc_return(&lt_prov_seq);
	lt_prov[idx].pid     = target_pid;
}

/*
 * Iterated rather than dumped: there can be tens of thousands of live sectors,
 * and a single show() would make seq_file grow a multi-megabyte buffer. The
 * VALID bitmap is walked WITHOUT the allocator lock, so a row may appear or
 * vanish mid-read. Again: a debug view of a moving target.
 */
static void *lt_prov_start(struct seq_file *m, loff_t *pos)
{
	unsigned long b;

	if (!lt_ready || !lt_prov)
		return NULL;
	if (*pos == 0)
		return SEQ_START_TOKEN;
	b = find_next_bit(lt_bm[LT_DATA], ltram_nr_pages, *pos - 1);
	return b < ltram_nr_pages ? (*pos = b + 1, &lt_prov[b]) : NULL;
}

static void *lt_prov_next(struct seq_file *m, void *v, loff_t *pos)
{
	unsigned long from = (v == SEQ_START_TOKEN) ? 0 : *pos;
	unsigned long b = find_next_bit(lt_bm[LT_DATA], ltram_nr_pages, from);

	return b < ltram_nr_pages ? (*pos = b + 1, &lt_prov[b]) : NULL;
}

static void lt_prov_stop(struct seq_file *m, void *v) { }

static int lt_prov_show(struct seq_file *m, void *v)
{
	struct lt_prov *e = v;
	unsigned long idx;

	if (v == SEQ_START_TOKEN) {
		seq_puts(m, "# live sectors only. sector = index into the window; src_pfn = the\n"
			    "# DRAM page it was copied from; seq = promotion order since boot.\n"
			    "#sector      pfn      src_pfn   erases      seq     pid\n");
		return 0;
	}
	idx = e - lt_prov;
	seq_printf(m, "%8lu %10lu %12u %8u %8u %7d\n",
		   idx, ltram_start_pfn + idx, e->src_pfn,
		   lt_ec ? lt_ec[idx] : 0, e->seq, e->pid);
	return 0;
}

static const struct seq_operations lt_prov_sops = {
	.start = lt_prov_start,
	.next  = lt_prov_next,
	.stop  = lt_prov_stop,
	.show  = lt_prov_show,
};

static int lt_prov_open(struct inode *i, struct file *f)
{
	return seq_open(f, &lt_prov_sops);
}

static const struct file_operations lt_prov_fops = {
	.owner   = THIS_MODULE,
	.open    = lt_prov_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release,
};

/*
 * Erase-count persistence.
 *
 * The counts survive nothing on their own. lt_ec is allocated zeroed at every
 * boot while the flash keeps its wear, and the boot scan cannot help: it
 * recovers which sectors are BLANK by reading them, but no sector records how
 * many times it has been erased. That history has to be handed back to us.
 *
 * A plain binary blob, header then one u32 per page, 256 KB for a 256 MiB
 * window. A userspace unit reads it before shutdown and writes it back after
 * boot. Unclean shutdown loses whatever accumulated since the last save, which
 * makes every count a LOWER BOUND rather than exact. That is the right
 * direction to be wrong in: undercounting sends a write to a sector we thought
 * was fresher than it is, which costs wear. Overcounting would make us avoid
 * good sectors and crowd the ones we believe are fresh, which is the failure
 * this whole mechanism exists to prevent.
 *
 * The counts are "erases since counting began", not the device's history. This
 * chip took heavy unknown wear during FPGA bring-up, before any of this
 * existed, and nothing can recover that.
 */
#define LT_EC_MAGIC	0x4C544543u		/* "LTEC" */
#define LT_EC_VERSION	1u

struct lt_ec_hdr {
	u32 magic;
	u32 version;
	u32 nr_pages;
	u32 reserved;
};

/*
 * No lock on either side. Each count is a naturally aligned u32, so no single
 * value can be torn; what a reader gets is a snapshot that may disagree with
 * itself by a few erases across pages. For a wear estimate that is noise, and
 * it is worth far more than holding ltram_alloc_lock with interrupts off while
 * copying 256 KB.
 */
/*
 * Restoring counts changes which bucket every free page belongs in, so the
 * lists have to be rebuilt from scratch. FREE is derived here the same way
 * lt_check_fast() derives it, as ~(VALID | DIRTY) less whatever is erasing,
 * because a rebuild that disagreed with the checker would trip the very
 * assertion it is meant to keep true.
 *
 * 65,536 iterations under the lock with interrupts off, roughly a hundred
 * microseconds, once, at restore time.
 */
static void lt_rebuild_buckets(void)
{
	unsigned long flags, i;
	unsigned int b;

	spin_lock_irqsave(&ltram_alloc_lock, flags);
	for (b = 0; b < LT_EC_BUCKETS; b++)
		INIT_LIST_HEAD(&lt_clean_by_ec[b]);
	lt_clean_count = 0;

	for (i = 0; i < ltram_nr_pages; i++) {
		if (test_bit(i, lt_bm[LT_DATA]) || test_bit(i, lt_bm[LT_DIRTY]))
			continue;
		INIT_LIST_HEAD(&lt_node[i]);
		list_add_tail(&lt_node[i], &lt_clean_by_ec[lt_bucket_of(lt_ec[i])]);
		lt_clean_count++;
	}
	spin_unlock_irqrestore(&ltram_alloc_lock, flags);

	/* The restored array IS the new ground truth for the wear budget. */
	{
		u64 sum = 0;

		for (i = 0; i < ltram_nr_pages; i++)
			sum += lt_ec[i];
		atomic64_set(&lt_ec_total, (s64)sum);
	}
	pr_info("ltram: erase counts restored, %lu free pages rebucketed, %lld erases counted\n",
		lt_clean_count, atomic64_read(&lt_ec_total));
}

static ssize_t lt_ec_read(struct file *f, char __user *ubuf, size_t len,
			  loff_t *ppos)
{
	struct lt_ec_hdr h = { LT_EC_MAGIC, LT_EC_VERSION,
			       (u32)ltram_nr_pages, 0 };
	size_t body = ltram_nr_pages * sizeof(*lt_ec);
	loff_t off;

	if (!lt_ready || !lt_ec)
		return -ENODEV;

	if (*ppos < (loff_t)sizeof(h))
		return simple_read_from_buffer(ubuf, len, ppos, &h, sizeof(h));

	off = *ppos - sizeof(h);
	if (off >= (loff_t)body)
		return 0;
	if (len > body - off)
		len = body - off;
	if (copy_to_user(ubuf, (char *)lt_ec + off, len))
		return -EFAULT;
	*ppos += len;
	return len;
}

static ssize_t lt_ec_write(struct file *f, const char __user *ubuf, size_t len,
			   loff_t *ppos)
{
	size_t body = ltram_nr_pages * sizeof(*lt_ec);
	loff_t off;

	if (!lt_ready || !lt_ec)
		return -ENODEV;

	/*
	 * The header is checked on the first write and gates everything after
	 * it. A blob from a different window size would otherwise be pasted
	 * over the live array and quietly corrupt every bucket assignment.
	 */
	if (*ppos < (loff_t)sizeof(struct lt_ec_hdr)) {
		struct lt_ec_hdr h;

		if (*ppos != 0 || len < sizeof(h))
			return -EINVAL;
		if (copy_from_user(&h, ubuf, sizeof(h)))
			return -EFAULT;
		if (h.magic != LT_EC_MAGIC || h.version != LT_EC_VERSION ||
		    h.nr_pages != ltram_nr_pages) {
			pr_warn("ltram: erase-count blob rejected: magic %08x ver %u pages %u; this kernel wants %08x %u %lu\n",
				h.magic, h.version, h.nr_pages,
				LT_EC_MAGIC, LT_EC_VERSION, ltram_nr_pages);
			return -EINVAL;
		}
		*ppos = sizeof(h);
		return sizeof(h);
	}

	off = *ppos - sizeof(struct lt_ec_hdr);
	if (off >= (loff_t)body)
		return -ENOSPC;
	if (len > body - off)
		len = body - off;
	if (copy_from_user((char *)lt_ec + off, ubuf, len))
		return -EFAULT;
	*ppos += len;

	/*
	 * Restoring counts moves pages between buckets, so the free lists have
	 * to be rebuilt or lt_pop_clean() keeps handing out by the old order and
	 * lt_check_fast() reports LT_ERR_BUCKET_MEMBER. Done once, when the
	 * last byte lands.
	 */
	if (off + len == body)
		lt_rebuild_buckets();
	return len;
}

static int lt_wear_show(struct seq_file *m, void *v)
{
	u64 total = (u64)ltram_nr_pages * (u64)wear_cycles;
	u64 used  = (u64)atomic64_read(&lt_ec_total);
	s64 secs  = (s64)wear_epoch + (s64)wear_days * 86400 -
		    ktime_get_real_seconds();
	unsigned int ms = lt_wear_interval_ms();

	seq_printf(m, "governor         %s\n", wear_governor ? "on" : "off");
	seq_printf(m, "epoch            %lu\n", wear_epoch);
	seq_printf(m, "service_days     %u\n", wear_days);
	seq_printf(m, "cycles_per_sect  %u\n", wear_cycles);
	seq_printf(m, "basis            allocations (upper bound on erases)\n");
	seq_printf(m, "cycles_total     %llu\n", total);
	seq_printf(m, "cycles_used      %llu\n", used);
	seq_printf(m, "cycles_left      %llu\n", total > used ? total - used : 0);
	seq_printf(m, "seconds_left     %lld\n", secs);
	if (ms == LT_WEAR_STOP)
		seq_puts(m, "interval_ms      STOPPED\n");
	else
		seq_printf(m, "interval_ms      %u\nrate_milli_hz    %llu\n",
			   ms, ms ? div64_u64(1000000ULL, ms) : 0ULL);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lt_wear);

static const struct file_operations lt_ec_fops = {
	.owner	= THIS_MODULE,
	.read	= lt_ec_read,
	.write	= lt_ec_write,
	.llseek	= default_llseek,
};

/*
 * Read the array and sort it into FREE and DIRTY, rather than assuming either.
 *
 * The page state lives in RAM, so a reboot loses it while the flash keeps every
 * bit. Seeding "all FREE" therefore told a lie that grew teeth the moment
 * inline_erase=0 became possible: FREE means "write_page() may program this
 * without erasing first", and NOR programs by clearing bits, so promising that
 * about a sector still holding last boot's data yields the AND of old and new,
 * silently and with no error anywhere.
 *
 * Seeding "all DIRTY" is safe but expensive in the one currency that does not
 * come back: 65,536 erases and ~18 minutes to blank an array that is usually
 * mostly blank already. Reading it costs 256 MB through the window at the
 * ~124 MB/s this device gives, so about two seconds -- and it reports the truth
 * instead of either assumption.
 *
 * A sector counts as blank only if EVERY byte is 0xFF. Sampling the first few
 * words is how you conclude a partially-programmed sector is erased.
 *
 * Returns the number of blank sectors, or -1 if the scan could not run.
 */
static unsigned int scan_pool = 1;
module_param(scan_pool, uint, 0444);

static long __init lt_scan_pool(void)
{
	unsigned long i, blank = 0;
	u64 t0;

	if (!scan_pool)
		return -1;

	/*
	 * Read through page_address(), NOT a private memremap(). The window is
	 * reserved-but-still-memory, so map_mem() gave it a linear mapping, and
	 * that is the mapping migration writes destinations through. Scanning a
	 * second mapping of the same physical sectors would be answering a
	 * slightly different question than the one that matters.
	 *
	 * Late enough for the same reason the step-3 self-test is: reading
	 * before the FPGA is programmed and the ECI link is up is an external
	 * abort with nothing after it on the console.
	 */
	t0 = ktime_get_ns();
	for (i = 0; i < ltram_nr_pages; i++) {
		const u64 *p = page_address(pfn_to_page(ltram_start_pfn + i));
		unsigned int w;

		for (w = 0; w < PAGE_SIZE / sizeof(u64); w++)
			if (READ_ONCE(p[w]) != ~0ULL)
				break;

		if (w == PAGE_SIZE / sizeof(u64)) {
			list_add_tail(&lt_node[i], &lt_clean_by_ec[0]);
			lt_clean_count++;
			blank++;
		} else {
			/*
			 * Holds something: not allocatable until erased.
			 *
			 * Both of the next two lines matter. ltram_policy_init()
			 * has already bitmap_fill()ed the live allocator bitmap,
			 * so a sector left set there while the shadow calls it
			 * DIRTY makes the two structures lt_check_fast() exists
			 * to cross-check disagree from the very first boot. And
			 * a sector that cannot be handed out is occupying the
			 * window exactly as much as a VALID one, so it counts as
			 * in use -- otherwise the erase that eventually frees it
			 * decrements a count it never incremented, and
			 * pages_in_use goes negative.
			 */
			set_bit(i, lt_bm[LT_DIRTY]);
			__clear_bit(i, ltram_clean_bitmap);
			atomic64_inc(&ltram_pages_in_use);
		}

		/* 256 MB of reads in an initcall: do not hold the CPU for all of it. */
		if (!(i & 0xFFF))
			cond_resched();
	}

	pr_info("ltram: pool scan %lu blank, %lu dirty, of %lu sectors in %llu ms\n",
		blank, ltram_nr_pages - blank, ltram_nr_pages,
		(ktime_get_ns() - t0) / NSEC_PER_MSEC);
	return blank;
}

/* Sized by the window, allocated once, never resized. ~1.3 MB for 256 MiB. */
static int __init lt_tracking_init(void)
{
	unsigned long i;
	unsigned int st, b;
	long blank;

	for (st = 0; st < LT_NR_BM; st++) {
		lt_bm[st] = bitmap_zalloc(ltram_nr_pages, GFP_KERNEL);
		if (!lt_bm[st])
			goto nomem;
	}
	lt_scratch = bitmap_zalloc(ltram_nr_pages, GFP_KERNEL);
	lt_ec   = kvcalloc(ltram_nr_pages, sizeof(*lt_ec), GFP_KERNEL);
	lt_node = kvmalloc_array(ltram_nr_pages, sizeof(*lt_node), GFP_KERNEL);
	lt_prov = kvcalloc(ltram_nr_pages, sizeof(*lt_prov), GFP_KERNEL);
	if (!lt_scratch || !lt_ec || !lt_node || !lt_prov)
		goto nomem;

	for (b = 0; b < LT_EC_BUCKETS; b++)
		INIT_LIST_HEAD(&lt_clean_by_ec[b]);

	/*
	 * Erase counts all start at 0. They are NOT the device's history and are
	 * not meant to be: this chip took heavy unknown wear during FPGA
	 * bring-up. Read them as "erases since counting began".
	 */
	for (i = 0; i < ltram_nr_pages; i++)
		INIT_LIST_HEAD(&lt_node[i]);

	lt_clean_count = 0;
	blank = lt_scan_pool();
	if (blank < 0) {
		/*
		 * No scan (unmappable window, or scan_pool=0). Then nothing is
		 * KNOWN blank, and the only safe assumption is that nothing is:
		 * a page declared FREE is a promise to write_page() that it may
		 * program without erasing, and a wrong promise there is silent
		 * data loss. Everything starts DIRTY and the erase worker earns
		 * the pool back.
		 */
		bitmap_fill(lt_bm[LT_DIRTY], ltram_nr_pages);
		bitmap_zero(ltram_clean_bitmap, ltram_nr_pages);
		atomic64_set(&ltram_pages_in_use, ltram_nr_pages);
		pr_warn("ltram: pool not scanned -- assuming all %lu sectors dirty\n",
			ltram_nr_pages);
	}
	lt_ready = true;

	if (ltram_debugfs_dir) {
		debugfs_create_file("pagestate", 0444, ltram_debugfs_dir,
				    NULL, &lt_state_fops);
		/* 0600: it is the device's wear history, and writing it
		 * rewrites the allocator's idea of which sector to use next. */
		debugfs_create_file("erase_counts", 0600, ltram_debugfs_dir,
				    NULL, &lt_ec_fops);
		debugfs_create_file("promotions", 0444, ltram_debugfs_dir,
				    NULL, &lt_prov_fops);
		debugfs_create_file("wear", 0444, ltram_debugfs_dir,
				    NULL, &lt_wear_fops);
	}
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
	kvfree(lt_prov); lt_prov = NULL;
	pr_warn("ltram: page-state tracking disabled (out of memory)\n");
	return -ENOMEM;
}

static int __init ltram_policy_init(void)
{
	if (!ltram_end_pfn)
		return 0;

	ltram_nr_pages = ltram_end_pfn - ltram_start_pfn;
	ltram_clean_bitmap = bitmap_zalloc(ltram_nr_pages, GFP_KERNEL);
	if (!ltram_clean_bitmap)
		return -ENOMEM;
	bitmap_fill(ltram_clean_bitmap, ltram_nr_pages);

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
