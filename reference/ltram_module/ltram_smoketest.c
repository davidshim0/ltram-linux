// SPDX-License-Identifier: GPL-2.0
/*
 * ltram_smoketest: in-kernel verification that ZONE_LTRAM allocations work
 * end to end.
 *
 * Loadable module. `insmod ltram_smoketest.ko` triggers the test suite once
 * during init; results print to dmesg. `rmmod` to unload. Pass counter is
 * exposed at /sys/module/ltram_smoketest/parameters/last_pass_count.
 *
 * EE392C, branch 250522_Implementation.
 */

#include <linux/module.h>
#include <linux/mm.h>
#include <linux/numa.h>
#include <linux/highmem.h>
#include <linux/ltram.h>

#define TAG "ltram_smoketest"

/*
 * GFP_LTRAM alone uses node 1's *fallback* zonelist, which lists node 0's
 * zones after ZONE_LTRAM. If the ZONE_LTRAM attempt does not immediately
 * succeed (e.g. migratetype mismatch at boot), the allocator silently
 * falls back to node 0 ZONE_NORMAL and trips the VM_BUG_ON in
 * get_page_from_freelist. Adding __GFP_THISNODE selects node 1's
 * no-fallback zonelist (ZONE_LTRAM only), so we either get an LtRAM page
 * or NULL, never a node-0 page. This matches ltram_migrate_from().
 */
#define LTRAM_GFP (GFP_LTRAM | __GFP_THISNODE)

/* Tunable bulk counts so we exercise the buddy allocator a bit. */
static int bulk_count = 64;
module_param(bulk_count, int, 0444);
MODULE_PARM_DESC(bulk_count, "Number of order-0 folios to allocate in bulk test (default 64)");

/* Result counters, readable from sysfs after init runs. */
static int last_pass_count;
static int last_fail_count;
module_param(last_pass_count, int, 0444);
module_param(last_fail_count, int, 0444);

#define EXPECT(cond, fmt, ...) do {                                            \
    if (!(cond)) {                                                             \
        pr_err(TAG ":   FAIL: " fmt "\n", ##__VA_ARGS__);                      \
        return -EINVAL;                                                        \
    }                                                                          \
} while (0)

/*
 * Test 1: single-page allocation.
 * Verify it lands on node 1, in ZONE_LTRAM.
 */
static int test_single_page(void)
{
    struct folio *folio;

    pr_info(TAG ": TEST 1 - single-page allocation from GFP_LTRAM\n");
    folio = folio_alloc(LTRAM_GFP, 0);
    EXPECT(folio, "folio_alloc returned NULL");

    pr_info(TAG ":   got folio: nid=%d, zone=%d, order=%u\n",
            folio_nid(folio), folio_zonenum(folio), folio_order(folio));
    EXPECT(folio_nid(folio) == 1, "expected nid=1, got nid=%d", folio_nid(folio));
    EXPECT(folio_zonenum(folio) == ZONE_LTRAM,
           "expected ZONE_LTRAM(%d), got zone=%d", ZONE_LTRAM, folio_zonenum(folio));

    folio_put(folio);
    pr_info(TAG ":   PASS\n");
    return 0;
}

/*
 * Test 2: bulk allocation.
 * Allocate many order-0 folios in a row to exercise free-list churn.
 */
static int test_bulk_allocation(void)
{
    struct folio **folios;
    int i, allocated = 0;
    int errors = 0;

    pr_info(TAG ": TEST 2 - bulk %d-page allocation\n", bulk_count);
    folios = kmalloc_array(bulk_count, sizeof(*folios), GFP_KERNEL);
    EXPECT(folios, "kmalloc_array failed for tracking array");

    for (i = 0; i < bulk_count; i++) {
        folios[i] = folio_alloc(LTRAM_GFP, 0);
        if (folios[i]) {
            allocated++;
            if (folio_nid(folios[i]) != 1)
                errors++;
            if (folio_zonenum(folios[i]) != ZONE_LTRAM)
                errors++;
        }
    }
    pr_info(TAG ":   allocated %d/%d folios, %d misplaced\n",
            allocated, bulk_count, errors);

    /* Free everything regardless of outcome */
    for (i = 0; i < bulk_count; i++) {
        if (folios[i])
            folio_put(folios[i]);
    }
    kfree(folios);

    EXPECT(allocated == bulk_count, "expected %d allocations, got %d",
           bulk_count, allocated);
    EXPECT(errors == 0, "%d folios on wrong node/zone", errors);

    pr_info(TAG ":   PASS\n");
    return 0;
}

/*
 * Test 3: write/read the allocated page.
 * Confirms the LtRAM page is actually backed by usable memory.
 */
static int test_write_then_read(void)
{
    struct folio *folio;
    u32 *kva;
    u32 pattern = 0xCAFEBABEu;

    pr_info(TAG ": TEST 3 - write/read pattern check\n");
    folio = folio_alloc(LTRAM_GFP, 0);
    EXPECT(folio, "folio_alloc failed");

    kva = folio_address(folio);  /* linear-map kernel VA */
    EXPECT(kva, "folio_address returned NULL");

    *kva = pattern;
    barrier();  /* prevent compiler reordering */

    EXPECT(*kva == pattern, "wrote 0x%08x, read back 0x%08x", pattern, *kva);

    pr_info(TAG ":   wrote 0x%08x at %p, read 0x%08x: PASS\n",
            pattern, kva, *kva);
    folio_put(folio);
    return 0;
}

/*
 * Test 4: GFP_KERNEL (no __GFP_LTRAM) MUST NOT land on LtRAM.
 * Verifies Katherine's zone-protection logic in get_page_from_freelist.
 */
static int test_kernel_alloc_avoids_ltram(void)
{
    struct folio *folio;

    pr_info(TAG ": TEST 4 - GFP_KERNEL allocation should NOT land on LtRAM\n");
    folio = folio_alloc(GFP_KERNEL, 0);
    EXPECT(folio, "GFP_KERNEL alloc failed");

    pr_info(TAG ":   got folio: nid=%d, zone=%d\n",
            folio_nid(folio), folio_zonenum(folio));
    EXPECT(folio_zonenum(folio) != ZONE_LTRAM,
           "GFP_KERNEL leaked onto ZONE_LTRAM");
    EXPECT(folio_nid(folio) == 0,
           "GFP_KERNEL allocation landed on node %d, expected 0", folio_nid(folio));

    folio_put(folio);
    pr_info(TAG ":   PASS\n");
    return 0;
}

/*
 * Test 5: GFP_HIGHUSER (the userspace default) must also avoid LtRAM.
 */
static int test_highuser_alloc_avoids_ltram(void)
{
    struct folio *folio;

    pr_info(TAG ": TEST 5 - GFP_HIGHUSER allocation should NOT land on LtRAM\n");
    folio = folio_alloc(GFP_HIGHUSER, 0);
    EXPECT(folio, "GFP_HIGHUSER alloc failed");

    EXPECT(folio_zonenum(folio) != ZONE_LTRAM,
           "GFP_HIGHUSER leaked onto ZONE_LTRAM");

    pr_info(TAG ":   GFP_HIGHUSER landed on nid=%d zone=%d: PASS\n",
            folio_nid(folio), folio_zonenum(folio));
    folio_put(folio);
    return 0;
}

/*
 * Test 6: alloc, write, free, alloc again, verify the page is reusable.
 * Catches lifecycle bugs where freed LtRAM pages do not return to the
 * free list cleanly.
 */
static int test_alloc_free_realloc(void)
{
    struct folio *folio1, *folio2;
    void *kva;

    pr_info(TAG ": TEST 6 - alloc -> write -> free -> alloc cycle\n");
    folio1 = folio_alloc(LTRAM_GFP, 0);
    EXPECT(folio1, "first alloc failed");

    kva = folio_address(folio1);
    memset(kva, 0xAB, PAGE_SIZE);
    pr_info(TAG ":   first folio at nid=%d, wrote 0xAB pattern\n",
            folio_nid(folio1));
    folio_put(folio1);

    folio2 = folio_alloc(LTRAM_GFP, 0);
    EXPECT(folio2, "second alloc failed");
    EXPECT(folio_nid(folio2) == 1, "second alloc not on LtRAM");

    pr_info(TAG ":   second alloc succeeded on nid=%d: PASS\n",
            folio_nid(folio2));
    folio_put(folio2);
    return 0;
}

static int __init ltram_smoketest_init(void)
{
    int passes = 0, fails = 0;

    pr_info(TAG ": ============= SMOKE TEST START =============\n");
    pr_info(TAG ": bulk_count parameter = %d\n", bulk_count);

    if (test_single_page() == 0)               passes++; else fails++;
    if (test_bulk_allocation() == 0)           passes++; else fails++;
    if (test_write_then_read() == 0)           passes++; else fails++;
    if (test_kernel_alloc_avoids_ltram() == 0) passes++; else fails++;
    if (test_highuser_alloc_avoids_ltram() == 0) passes++; else fails++;
    if (test_alloc_free_realloc() == 0)        passes++; else fails++;

    last_pass_count = passes;
    last_fail_count = fails;

    pr_info(TAG ": ============= SMOKE TEST DONE  =============\n");
    pr_info(TAG ": Results: %d passed, %d failed\n", passes, fails);
    if (fails == 0)
        pr_info(TAG ": ALL TESTS PASSED\n");
    else
        pr_err(TAG ":  %d TEST(S) FAILED\n", fails);

    /*
     * Return 0 so the module stays loaded and `rmmod` works cleanly.
     * Test results are visible in dmesg and via sysfs.
     */
    return 0;
}

static void __exit ltram_smoketest_exit(void)
{
    pr_info(TAG ": unloaded\n");
}

module_init(ltram_smoketest_init);
module_exit(ltram_smoketest_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("EE392C LtRAM project");
MODULE_DESCRIPTION("Smoke test: verifies ZONE_LTRAM allocation works end-to-end");
