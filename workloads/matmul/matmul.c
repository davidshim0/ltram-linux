/*
 * matmul — the LtRAM step-1 workload and baseline instrument.
 *
 * Repeated y = W * x over a large weight matrix. The split is STRUCTURAL, which is
 * what makes it an instrument rather than just a benchmark:
 *
 *     W  weights   written once at init, then ONLY READ      <- the promotion target
 *     y  result    written every iteration                   <- must never be promoted
 *
 * Design notes that exist for the measurement, not the maths:
 *
 *  - W and y are separate mmap regions so their address ranges are exact and can be
 *    resolved through /proc/PID/pagemap later. --print-ranges emits them.
 *  - --protect-weights mprotects W read-only after init. If the algorithm ever writes
 *    W, we get a reported fault naming the address instead of a silent wrong answer.
 *    That converts an assumption into a proof.
 *  - --verify digests the result. A timing number from a wrong computation is worthless,
 *    and this digest is what later steps must reproduce EXACTLY to show the flash path
 *    did not corrupt anything.
 *  - init is seeded, so the digest is comparable across machines and runs.
 *  - --phys prints, at start / after every run / at end, where each region actually
 *    LIVES: first pfn, physical address, and a checksum over every pfn in the region.
 *    The checksum changes if and only if a page moved, so "did anything migrate?"
 *    is one number to compare rather than 50,176. Constant under the vanilla kernel;
 *    expected to change once the policy is promoting. Needs root.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <getopt.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include "sha256.h"

static size_t N = 8192;          /* W is N*N floats: 8192 -> 256 MiB, the window size */
static int    ITERS = 100;
static int    RUNS = 10;
static int    do_verify = 0, do_protect = 0, do_ranges = 0, do_phys = 0, hold_secs = 0;
/* Do not start the clock until the weights are actually IN flash.
 *
 * Promotion happens while the workload runs, and it is not free: at ~1.2 ms a
 * program and 512 pages a scanner tick, the device spends 61% of each second
 * programming, and a program blocks a read. Time a run through that phase and
 * the mean is an average over a transient rather than a latency.
 *
 * That is not hypothetical. The 128 and 256 MiB points came out with a
 * per-pass sd of 17% and 15%, against 0.10% at 64 MiB where promotion finishes
 * early -- and the elevated NOR cost at exactly those two sizes is what sent
 * us looking. Same number in the sweep and in a clean rerun, so it reproduces;
 * it just is not the number we wanted. */
static double wait_res = 0.0;    /* --wait-resident PCT: 0 disables */
static int    wait_max = 900;    /* --wait-timeout SECS */
static int    compute_only = 0;   /* --compute-only: pin every row to row 0 */
static int    do_chase = 0;       /* --chase: dependent-load latency over W */
static volatile uint64_t chase_sink;
static size_t flush_mb = 0;             /* 0 = off; LLC scrub between runs */
static unsigned char *scrub_buf = NULL;
static size_t scrub_bytes = 0;

/*
 * Evict the last-level cache by capacity.
 *
 * This machine has no private L2: 16 MiB is the LLC and it is shared by all 48
 * cores (DESIGN-DECISIONS 0.0). Without this, every run inherits a slice of the
 * previous run's residency and the timings measure a warm cache -- which is
 * fine for a steady-state control and useless for watching a working set move
 * between two tiers with different latencies.
 *
 * Capacity eviction rather than `dc civac`: one dependent-free read per 128 B
 * line over 4x the LLC steamrolls every set, needs no privilege, and does not
 * depend on which cache maintenance instructions EL0 is allowed to issue.
 *
 * CALLED OUTSIDE THE TIMED REGION -- see the run loop. The scrub is not part of
 * the measurement, only of the wall clock.
 */
static void cache_scrub(void)
{
    volatile unsigned long sink = 0;
    size_t i;
    if (!scrub_buf) return;
    for (i = 0; i < scrub_bytes; i += 128) sink += scrub_buf[i];
    (void)sink;

    /*
     * Then DIRTY one word per page, so the scrub buffer can never be mistaken
     * for the thing under study.
     *
     * It is anonymous, and after its memset it is read-mostly -- which makes it
     * a perfect promotion candidate. mmap hands out descending addresses, so it
     * lands BELOW the weights, and the scanner walks from address 0: on
     * 2026-08-20 it promoted all 16,384 pages of the scrub buffer to flash
     * before it ever reached the weights, burning 457 s and 16,384 erases on
     * the instrument rather than the subject.
     *
     * One store per page keeps pte_write() true, so the policy rejects and
     * re-arms it every pass and never selects it. 16,384 stores, immeasurable
     * next to the read sweep above, and still outside the timed region.
     */
    for (i = 0; i < scrub_bytes; i += 4096) scrub_buf[i]++;
}
static uint64_t SEED = 20260818;

static float *W, *x, *y;
static size_t Wbytes, xbytes, ybytes;

/* ---- weight-write detector -------------------------------------------------
 * With --protect-weights, a store into W traps here instead of silently
 * succeeding. Report the address and die loudly: the read-only claim is the
 * premise of the whole experiment, so a violation must not be survivable. */
static void segv(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)uc;
    char *a = (char *)si->si_addr;
    if (a >= (char *)W && a < (char *)W + Wbytes) {
        size_t off = (size_t)(a - (char *)W);
        dprintf(2, "\nFATAL: the algorithm WROTE the weights at W+0x%zx\n"
                   "       the read-only premise is false -- do not trust any promotion result\n", off);
        _exit(42);
    }
    dprintf(2, "\nFATAL: SIGSEGV at %p (outside W)\n", a);
    _exit(43);
}

static double now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void *alloc_region(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    return p;
}

/* deterministic, cheap, and not all-zero (zero pages would be shared and
 * would not behave like real weights under migration) */
static uint64_t rng_s;
static inline float rnd(void)
{
    rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17;
    return (float)((rng_s >> 40) & 0xffff) / 65536.0f - 0.5f;
}

/* ---- physical-address reporting (--phys) -----------------------------------
 * A region is not one physical address. 196 MiB of W is 50,176 pages, each with
 * its own pfn, faulted in on demand and not physically contiguous. So per region
 * this prints an anchor (the first present page), a census, and a checksum over
 * every pfn in order.
 *
 * THE CHECKSUM IS THE TRIPWIRE, THE DIFF IS THE REPORT. pfnsum changes if and
 * only if some page moved; when it does, the full previous pfn vector is still
 * held, so the move is described rather than merely detected: how many pages
 * changed, and for each, WHERE FROM and WHERE TO -- classified DRAM->LtRAM,
 * LtRAM->DRAM, DRAM->DRAM, LtRAM->LtRAM, appeared, vanished. That is the
 * question step 6 asks; a count of promotions cannot answer it.
 *
 * The LtRAM window comes from the same debugfs files ltram-inspect reads, so the
 * two tools cannot drift apart about where the boundary is. On a CONFIG_LTRAM=n
 * kernel those files are absent, every page classifies as DRAM, and the verdict
 * should be "constant" -- that is the control case, not a failure.
 *
 * COST, AND WHY IT CANNOT REACH THE MEASUREMENT. Every report is timed and the
 * total is accumulated in phys_overhead. Reports run OUTSIDE the timed section
 * -- t[r] is taken before the call -- so no scan, diff or print can enter a run
 * time or the mean. The overhead is stated at the end so that exclusion is
 * checkable rather than asserted.
 *
 * Holding the previous pfn vector costs 8 bytes per page: ~400 KB for W at
 * N=7168, anonymous memory inside the target process. Small against 196 MiB,
 * but it is there, and with --phys off none of it is allocated.
 *
 * NEEDS ROOT: an unprivileged pagemap read reports every page present with
 * pfn 0. That is detected and said out loud rather than printed as an address.
 */
#define PM_PRESENT   (1ULL << 63)
#define PM_PFN_MASK  ((1ULL << 55) - 1)
#define PFN_ABSENT   0UL        /* pfn 0 is never a user page on this machine */
#define MOVE_SAMPLES 8          /* individual moves listed per report */
#define HUGE_BYTES   (2UL << 20) /* THP size at the 4 KiB granule */

static int  pm_fd = -1;
static long page_sz;
static unsigned long lt_lo, lt_hi;  /* LtRAM pfn window; 0 = not an LtRAM kernel */
static int  phys_ready, phys_warned, phys_huge_warned, phys_seq;
static double phys_overhead;        /* seconds in --phys; excluded from t[] */

struct phys_stat {
    uint64_t pages, present, in_ltram;
    unsigned long first_pfn, max_pfn;
    uint64_t pfnsum;
};

struct phys_track {
    const char    *name;
    unsigned long *prev;        /* pfn of every page as of the last report */
    uint64_t       npages, last_sum;
    int            seen, changes, first_rep, last_rep;
    uint64_t       tot_to_ltram, tot_to_dram;
};
static struct phys_track ptrack[] = {
    { "weights", NULL, 0, 0, 0, 0, 0, 0, 0, 0 },
    { "input",   NULL, 0, 0, 0, 0, 0, 0, 0, 0 },
    { "result",  NULL, 0, 0, 0, 0, 0, 0, 0, 0 },
};

static unsigned long *pfn_scratch;
static uint64_t       pfn_scratch_n;

static struct phys_track *ptrack_find(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof ptrack / sizeof ptrack[0]; i++)
        if (!strcmp(ptrack[i].name, name))
            return &ptrack[i];
    return NULL;
}

static int pfn_is_ltram(unsigned long pfn)
{
    return lt_hi && pfn >= lt_lo && pfn < lt_hi;
}

static const char *pfn_where(unsigned long pfn)
{
    if (pfn == PFN_ABSENT)
        return "absent";
    return pfn_is_ltram(pfn) ? "LtRAM" : "DRAM";
}

static unsigned long rd_ulong(const char *path)
{
    FILE *f = fopen(path, "r");
    unsigned long v = 0;
    if (!f) return 0;
    if (fscanf(f, "%lu", &v) != 1) v = 0;
    fclose(f);
    return v;
}

static void phys_open(void)
{
    phys_ready = 1;
    page_sz = sysconf(_SC_PAGESIZE);
    pm_fd = open("/proc/self/pagemap", O_RDONLY);
    if (pm_fd < 0) {
        fprintf(stderr, "PHYS: cannot open /proc/self/pagemap (%s) -- run as root\n",
                strerror(errno));
        return;
    }
    lt_lo = rd_ulong("/sys/kernel/debug/ltram/start_pfn");
    lt_hi = rd_ulong("/sys/kernel/debug/ltram/end_pfn");
    if (lt_hi)
        printf("PHYS window  ltram pfn 0x%lx .. 0x%lx  (%lu pages, %lu MiB)\n",
               lt_lo, lt_hi, lt_hi - lt_lo,
               ((lt_hi - lt_lo) * (unsigned long)page_sz) >> 20);
    else
        printf("PHYS window  none -- CONFIG_LTRAM=n or debugfs not mounted;"
               " every page will classify as DRAM\n");
    fflush(stdout);
}

static void phys_scan(const void *base, size_t bytes, struct phys_stat *s,
                      unsigned long *out)
{
    uint64_t i;

    memset(s, 0, sizeof *s);
    s->pages = ((uint64_t)bytes + (uint64_t)page_sz - 1) / (uint64_t)page_sz;
    if (pm_fd < 0)
        return;

    for (i = 0; i < s->pages; i++) {
        uint64_t vaddr = (uint64_t)(uintptr_t)base + i * (uint64_t)page_sz, ent;
        off_t off = (off_t)((vaddr / (uint64_t)page_sz) * sizeof ent);
        unsigned long pfn = PFN_ABSENT;

        if (pread(pm_fd, &ent, sizeof ent, off) == (ssize_t)sizeof ent &&
            (ent & PM_PRESENT))
            pfn = (unsigned long)(ent & PM_PFN_MASK);

        out[i] = pfn;
        if (pfn == PFN_ABSENT)
            continue;
        if (!s->present)
            s->first_pfn = pfn;
        s->present++;
        if (pfn > s->max_pfn)
            s->max_pfn = pfn;
        s->pfnsum = s->pfnsum * 1099511628211ULL + pfn;   /* order-sensitive */
        if (pfn_is_ltram(pfn))
            s->in_ltram++;
    }
    /* Two distinct ways a privilege problem shows up, both of which would
     * otherwise be read as "nothing is here" rather than "you cannot see". */
    if (!phys_warned && s->pages && !s->present) {
        fprintf(stderr,
            "PHYS WARNING: pagemap reports 0 of %llu pages present for a region\n"
            "              that was just written. This is a permission problem,\n"
            "              not an empty region -- run as root.\n",
            (unsigned long long)s->pages);
        phys_warned = 1;
    } else if (!phys_warned && s->present && !s->max_pfn) {
        fprintf(stderr,
            "PHYS WARNING: every pfn read back as 0 -- this is an unprivileged\n"
            "              pagemap read. Run as root; the addresses are not real.\n");
        phys_warned = 1;
    }
}

/* Count 2 MiB blocks backed by one huge page: a block whose 512 pfns are
 * consecutive and start 512-aligned.
 *
 * THIS IS NOT A CURIOSITY. Step 6 refuses large folios outright
 * (folio_test_large in mm/ltram_policy.c), so a THP-backed weight region is
 * ENTIRELY INELIGIBLE for promotion and the policy moves nothing -- producing
 * exactly the signature of a broken policy, from a cause that has nothing to do
 * with the policy. Detect it here rather than discover it as a zero.
 */
static uint64_t phys_huge_blocks(const void *base, const unsigned long *pfn,
                                 uint64_t pages, uint64_t *blocks_total)
{
    uint64_t ppb = HUGE_BYTES / (uint64_t)page_sz;      /* 512 at 4 KiB */
    uint64_t va  = (uint64_t)(uintptr_t)base;
    uint64_t alg = (va + HUGE_BYTES - 1) & ~(HUGE_BYTES - 1);
    uint64_t skip, nblk, b, k, huge = 0;

    *blocks_total = 0;
    if (ppb < 2 || page_sz <= 0)
        return 0;                     /* not a granule we model */
    skip = (alg - va) / (uint64_t)page_sz;
    nblk = pages > skip ? (pages - skip) / ppb : 0;
    *blocks_total = nblk;

    for (b = 0; b < nblk; b++) {
        const unsigned long *p = pfn + skip + b * ppb;

        if (p[0] == PFN_ABSENT || p[0] % ppb)
            continue;
        for (k = 1; k < ppb; k++)
            if (p[k] != p[0] + k)
                break;
        if (k == ppb)
            huge++;
    }
    return huge;
}

/* Describe a change, do not merely announce one: what moved, from where, to
 * where. Two passes so the totals print above the examples. */
static void phys_diff(struct phys_track *tk, const unsigned long *cur)
{
    uint64_t i, changed = 0, d2l = 0, l2d = 0, d2d = 0, l2l = 0, gone = 0, born = 0;
    int shown = 0;

    for (i = 0; i < tk->npages; i++) {
        unsigned long o = tk->prev[i], n = cur[i];

        if (o == n)
            continue;
        changed++;
        if (o == PFN_ABSENT)                            born++;
        else if (n == PFN_ABSENT)                       gone++;
        else if (!pfn_is_ltram(o) &&  pfn_is_ltram(n))  d2l++;
        else if ( pfn_is_ltram(o) && !pfn_is_ltram(n))  l2d++;
        else if ( pfn_is_ltram(o))                      l2l++;
        else                                            d2d++;
    }

    printf("PHYS  moved %-8s %llu of %llu pages changed physical address\n",
           tk->name, (unsigned long long)changed, (unsigned long long)tk->npages);
    printf("PHYS        %-8s DRAM->LtRAM %llu   LtRAM->DRAM %llu   "
           "DRAM->DRAM %llu   LtRAM->LtRAM %llu   appeared %llu   vanished %llu\n",
           tk->name, (unsigned long long)d2l, (unsigned long long)l2d,
           (unsigned long long)d2d, (unsigned long long)l2l,
           (unsigned long long)born, (unsigned long long)gone);

    for (i = 0; i < tk->npages && shown < MOVE_SAMPLES; i++) {
        unsigned long o = tk->prev[i], n = cur[i];

        if (o == n)
            continue;
        printf("PHYS        %-8s page %8llu  pfn 0x%09lx -> 0x%09lx   "
               "phys 0x%012llx -> 0x%012llx   %s -> %s\n",
               tk->name, (unsigned long long)i, o, n,
               (unsigned long long)o * (unsigned long long)page_sz,
               (unsigned long long)n * (unsigned long long)page_sz,
               pfn_where(o), pfn_where(n));
        shown++;
    }
    if (changed > (uint64_t)shown)
        printf("PHYS        %-8s ... %llu further move(s) not listed\n",
               tk->name, (unsigned long long)(changed - (uint64_t)shown));

    tk->tot_to_ltram += d2l;
    tk->tot_to_dram  += l2d;
    fflush(stdout);
}

static void phys_one(const char *tag, const char *name, const void *base, size_t bytes)
{
    uint64_t pages = ((uint64_t)bytes + (uint64_t)page_sz - 1) / (uint64_t)page_sz;
    struct phys_track *tk = ptrack_find(name);
    const char *status = "";
    struct phys_stat s;
    char moved[64];

    if (pages > pfn_scratch_n) {
        unsigned long *p = realloc(pfn_scratch, pages * sizeof *p);
        if (!p) {
            fprintf(stderr, "PHYS: out of memory for %llu pages -- disabling --phys\n",
                    (unsigned long long)pages);
            do_phys = 0;
            return;
        }
        pfn_scratch = p;
        pfn_scratch_n = pages;
    }

    phys_scan(base, bytes, &s, pfn_scratch);

    if (tk && !tk->seen)
        status = "  [base]";
    else if (tk && s.pfnsum == tk->last_sum)
        status = "  [same]";
    else if (tk) {
        snprintf(moved, sizeof moved, "  [MOVED was 0x%016llx]",
                 (unsigned long long)tk->last_sum);
        status = moved;
    }

    printf("PHYS %-6s %-8s pages %8llu  present %8llu  first pfn 0x%09lx  phys 0x%012llx",
           tag, name, (unsigned long long)s.pages, (unsigned long long)s.present,
           s.first_pfn, (unsigned long long)s.first_pfn * (unsigned long long)page_sz);
    if (lt_hi)
        printf("  LtRAM %llu (%.1f%%)", (unsigned long long)s.in_ltram,
               s.pages ? 100.0 * (double)s.in_ltram / (double)s.pages : 0.0);
    {
        uint64_t nblk = 0, huge = phys_huge_blocks(base, pfn_scratch, pages, &nblk);

        if (nblk)
            printf("  huge %llu/%llu", (unsigned long long)huge,
                   (unsigned long long)nblk);
        printf("  pfnsum 0x%016llx%s\n", (unsigned long long)s.pfnsum, status);

        if (huge && !phys_huge_warned) {
            fprintf(stderr,
                "PHYS WARNING: %s is huge-page backed (%llu of %llu 2 MiB blocks).\n"
                "              Step 6 skips large folios, so these pages are\n"
                "              INELIGIBLE for promotion and the policy will move\n"
                "              nothing -- which looks exactly like a policy bug.\n"
                "              echo never | sudo tee"
                " /sys/kernel/mm/transparent_hugepage/enabled\n",
                name, (unsigned long long)huge, (unsigned long long)nblk);
            phys_huge_warned = 1;
        }
    }

    if (!tk) {
        fflush(stdout);
        return;
    }

    if (!tk->seen) {
        tk->seen = 1;
        tk->npages = pages;
        tk->prev = malloc(pages * sizeof *tk->prev);
        if (tk->prev)
            memcpy(tk->prev, pfn_scratch, pages * sizeof *tk->prev);
    } else if (s.pfnsum != tk->last_sum) {
        if (!tk->changes)
            tk->first_rep = phys_seq;
        tk->last_rep = phys_seq;
        tk->changes++;
        if (tk->prev && tk->npages == pages) {
            phys_diff(tk, pfn_scratch);
            memcpy(tk->prev, pfn_scratch, pages * sizeof *tk->prev);
        }
    }
    tk->last_sum = s.pfnsum;
    fflush(stdout);
}

static void phys_verdict(void)
{
    size_t i;

    if (!do_phys || !phys_ready)
        return;
    for (i = 0; i < sizeof ptrack / sizeof ptrack[0]; i++) {
        struct phys_track *tk = &ptrack[i];

        if (!tk->seen)
            continue;
        if (!tk->changes)
            printf("PHYS verdict %-8s pfnsum constant across %d reports"
                   " -- no page of this region moved\n", tk->name, phys_seq);
        else
            printf("PHYS verdict %-8s pfnsum changed at %d of %d reports"
                   " (first %d, last %d) -- cumulative DRAM->LtRAM %llu,"
                   " LtRAM->DRAM %llu\n",
                   tk->name, tk->changes, phys_seq, tk->first_rep, tk->last_rep,
                   (unsigned long long)tk->tot_to_ltram,
                   (unsigned long long)tk->tot_to_dram);
    }
    printf("PHYS overhead %.3f s over %d reports"
           " -- EXCLUDED from every run time and from RESULT\n",
           phys_overhead, phys_seq);
    fflush(stdout);
}

/* Timed as a whole, so the cost of --phys is reported rather than assumed.
 * Every caller sits outside the measured region. */
static void phys_report(const char *tag)
{
    double t0;

    if (!do_phys)
        return;
    if (!phys_ready)
        phys_open();
    t0 = now();
    phys_seq++;
    phys_one(tag, "weights", W, Wbytes);
    phys_one(tag, "input",   x, xbytes);
    phys_one(tag, "result",  y, ybytes);
    phys_overhead += now() - t0;
}
/* LtRAM share of the weight region, right now. Same scan phys_one does,
 * without the printing, so --wait-resident can poll it cheaply. */
static double weights_ltram_pct(void)
{
    uint64_t pages = ((uint64_t)Wbytes + (uint64_t)page_sz - 1) / (uint64_t)page_sz;
    struct phys_stat s;

    if (pm_fd < 0)
        return 0.0;
    if (pages > pfn_scratch_n) {
        unsigned long *p = realloc(pfn_scratch, pages * sizeof *p);

        if (!p)
            return 0.0;
        pfn_scratch = p;
        pfn_scratch_n = pages;
    }
    phys_scan(W, Wbytes, &s, pfn_scratch);
    return s.pages ? 100.0 * (double)s.in_ltram / (double)s.pages : 0.0;
}

int main(int argc, char **argv)
{
    static struct option lo[] = {
        {"n",1,0,'n'}, {"iters",1,0,'i'}, {"runs",1,0,'r'},
        {"verify",0,0,'V'}, {"protect-weights",0,0,'P'},
        {"print-ranges",0,0,'R'}, {"phys",0,0,'A'},
        {"hold",1,0,'H'}, {"seed",1,0,'S'}, {"flush",1,0,'F'},
        {"compute-only",0,0,'C'}, {"chase",0,0,'H'+128},
        {"wait-resident",1,0,'W'}, {"wait-timeout",1,0,'W'+128}, {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "n:i:r:VPRAH:S:F:W:", lo, NULL)) != -1) {
        switch (c) {
        case 'n': N = strtoul(optarg, NULL, 0); break;
        case 'i': ITERS = atoi(optarg); break;
        case 'r': RUNS = atoi(optarg); break;
        case 'V': do_verify = 1; break;
        case 'P': do_protect = 1; break;
        case 'R': do_ranges = 1; break;
        case 'A': do_phys = 1; break;
        case 'H': hold_secs = atoi(optarg); break;
        case 'W': wait_res = atof(optarg); break;
        case 'W'+128: wait_max = atoi(optarg); break;
        case 'S': SEED = strtoull(optarg, NULL, 0); break;
        case 'F': flush_mb = strtoul(optarg, NULL, 0); break;
        case 'C': compute_only = 1; break;
        case 'H'+128: do_chase = 1; break;
        default:
            fprintf(stderr,
              "usage: %s [--n DIM] [--iters K] [--runs R] [--verify]\n"
              "          [--protect-weights] [--print-ranges] [--phys]\n"
              "          [--hold SECS] [--seed S] [--flush MB] [--compute-only] [--chase]\n"
              "          [--wait-resident PCT] [--wait-timeout SECS]\n", argv[0]);
            return 2;
        }
    }

    Wbytes = N * N * sizeof(float);
    xbytes = N * sizeof(float);
    ybytes = N * sizeof(float);

    if (do_chase && do_verify) {
        fprintf(stderr, "--chase does not compute the matmul. Nothing to verify.\n");
        return 2;
    }
    if (compute_only && do_verify) {
        fprintf(stderr, "--compute-only computes the wrong answer on purpose "
                        "(every row is row 0). Do not ask it to --verify.\n");
        return 2;
    }
    W = alloc_region(Wbytes);
    x = alloc_region(xbytes);
    y = alloc_region(ybytes);

    if (flush_mb) {
        scrub_bytes = flush_mb << 20;
        scrub_buf = mmap(NULL, scrub_bytes, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (scrub_buf == MAP_FAILED) { perror("mmap scrub"); return 1; }
        memset(scrub_buf, 1, scrub_bytes);   /* fault it in now, not mid-run */
        printf("  cache scrub  %zu MiB between runs (LLC is 16 MiB shared)\n", flush_mb);
    }

    rng_s = SEED ? SEED : 1;
    for (size_t i = 0; i < N * N; i++) W[i] = rnd();
    for (size_t i = 0; i < N; i++)     x[i] = rnd();
    /*
     * --chase turns W into a single cycle over its 128-byte lines: the first
     * word of each line holds the index of the next. Following it makes every
     * load DEPEND on the one before, so nothing overlaps, nothing prefetches,
     * and no memory-level parallelism can hide anything. What comes out is the
     * medium's latency, which is a different number from what the matmul pays.
     *
     * Sattolo's algorithm rather than a plain shuffle: it produces exactly one
     * cycle covering every line, so the walk cannot fall into a short loop and
     * measure a working set smaller than W.
     *
     * The chain is written ONCE, here. After this W is only read, which is what
     * lets the policy see it as read-mostly and promote it.
     */
    if (do_chase) {
        size_t lines = Wbytes / 128, i;
        uint32_t *perm = malloc(lines * sizeof *perm);
        if (!perm) { perror("chase perm"); return 1; }
        for (i = 0; i < lines; i++) perm[i] = (uint32_t)i;
        for (i = lines - 1; i > 0; i--) {         /* Sattolo: one cycle */
            size_t j;
            rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17;
            j = (size_t)(rng_s % i);
            uint32_t t2 = perm[i]; perm[i] = perm[j]; perm[j] = t2;
        }
        for (i = 0; i < lines; i++)
            *(uint32_t *)((char *)W + i * 128) = perm[i];
        free(perm);
        printf("CHASE %zu lines of 128 B over %zu MiB, one dependent load each\n",
               lines, Wbytes >> 20);
    }
    memset(y, 0, ybytes);

    if (do_protect) {
        struct sigaction sa; memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = segv; sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS,  &sa, NULL);
        if (mprotect(W, Wbytes, PROT_READ)) { perror("mprotect"); return 1; }
    }

    printf("matmul  N=%zu  iters=%d  runs=%d  seed=%llu\n",
           N, ITERS, RUNS, (unsigned long long)SEED);
    printf("  weights  %8.1f MiB  %s\n", Wbytes / 1048576.0,
           do_protect ? "mprotect PROT_READ ok" : "(writable -- pass --protect-weights to prove RO)");
    printf("  result   %8.1f KiB\n", ybytes / 1024.0);
    if (do_ranges) {
        printf("RANGES pid=%d\n", getpid());
        printf("RANGE weights %p %p %zu\n", (void *)W, (void *)((char *)W + Wbytes), Wbytes);
        printf("RANGE result  %p %p %zu\n", (void *)y, (void *)((char *)y + ybytes), ybytes);
        fflush(stdout);
    }
    phys_report("start");

    /* Untimed passes until the weights are in flash, THEN start the clock.
     * Reads only, so it drives the scanner exactly as a timed pass would; the
     * only difference is that nothing here lands in t[]. y is zeroed after,
     * and every timed run memsets it anyway, so --verify is unaffected. */
    if (wait_res > 0.0) {
        double tw = now(), share = 0.0;
        int pass = 0;

        if (!phys_ready)
            phys_open();
        printf("WARMUP waiting for weights to reach %.1f%% LtRAM residency"
               " (timeout %d s)\n", wait_res, wait_max);
        fflush(stdout);
        for (;;) {
            for (size_t i = 0; i < N; i++) {
                const float *row = W + i * N;
                float acc = 0.0f;
                for (size_t j = 0; j < N; j++) acc += row[j] * x[j];
                y[i] += acc;
            }
            pass++;
            share = weights_ltram_pct();
            printf("WARMUP pass %3d  residency %6.2f%%  %7.1f s\n",
                   pass, share, now() - tw);
            fflush(stdout);
            if (share >= wait_res)
                break;
            if (now() - tw > (double)wait_max) {
                printf("WARMUP TIMEOUT after %.1f s at %.2f%% -- measuring anyway,"
                       " treat this point as contaminated\n", now() - tw, share);
                break;
            }
        }
        memset(y, 0, ybytes);
        printf("WARMUP done  residency %.2f%%  %d passes  %.1f s\n",
               share, pass, now() - tw);
        fflush(stdout);
    }

    double *t = calloc(RUNS, sizeof(double));
    char digest[65] = "", d2[65];

    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    /* Absolute epoch of the run loop's origin, so a separate sampler can line
     * its promotion curve up against these timings with no guesswork about how
     * long the fill took. */
    printf("TSTART %.3f\n", rt.tv_sec + rt.tv_nsec * 1e-9);
    fflush(stdout);
    double t_start = now();   /* wall origin for POINT lines */
    for (int r = 0; r < RUNS; r++) {
        memset(y, 0, ybytes);
        cache_scrub();          /* BEFORE t0: excluded from the measurement */
        double t0 = now();
        if (do_chase) {
            /* One full cycle: every line visited exactly once, each load
             * waiting on the previous. lines accesses, no overlap. */
            size_t lines = Wbytes / 128, k;
            uint32_t cur = 0;
            for (k = 0; k < lines; k++)
                cur = *(volatile uint32_t *)((char *)W + (size_t)cur * 128);
            chase_sink += cur;
            t[r] = now() - t0;
            printf("  run %2d/%d  %8.3f s   %7.1f ns/access\n",
                   r + 1, RUNS, t[r], t[r] * 1e9 / (double)lines);
            printf("POINT %d %.6f %.3f\n", r + 1, t[r], now() - t_start);
            fflush(stdout);
            if (do_phys) {
                char tag[16];
                snprintf(tag, sizeof tag, "run%d", r + 1);
                phys_report(tag);
            }
            continue;
        }
        for (int it = 0; it < ITERS; it++) {
            /* y accumulates every iteration: continuously written.
             * W is read N*N times per iteration and never touched. */
            for (size_t i = 0; i < N; i++) {
                /*
                 * --compute-only pins every row to W's FIRST row, so the inner
                 * loop reads the same N*4 bytes N times and stays in L1D. Same
                 * N, same bounds, same instruction stream, same FMA count. The
                 * ONLY thing removed is the memory traffic.
                 *
                 * That makes the difference between this and a normal run the
                 * cost of reaching the weights, measured rather than inferred:
                 *
                 *   T_full - T_compute = time spent getting at W
                 *
                 * Run it against DRAM and against LtRAM and the two memory
                 * terms are directly comparable. The result is wrong on purpose
                 * (every row is row 0), so --verify is refused below.
                 */
                const float *row = compute_only ? W : W + i * N;
                float acc = 0.0f;
                for (size_t j = 0; j < N; j++) acc += row[j] * x[j];
                y[i] += acc;
            }
        }
        t[r] = now() - t0;   /* stop the clock HERE: everything below --
                              * digest, printing, --phys scan and diff --
                              * is outside the measurement by construction */

        if (do_verify) {
            sha256_t h; sha256_init(&h);
            sha256_update(&h, y, ybytes);
            sha256_final(&h, d2);
            if (!digest[0]) memcpy(digest, d2, 65);
            else if (memcmp(digest, d2, 65)) {
                fprintf(stderr, "FATAL: digest changed between runs\n  run1 %s\n  run%d %s\n",
                        digest, r + 1, d2);
                return 44;
            }
        }
        printf("  run %2d/%d  %8.3f s\n", r + 1, RUNS, t[r]);
        /* Machine-readable, with the wall offset so a run can be lined up
         * against the promotion curve sampled by the driving script. */
        printf("POINT %d %.6f %.3f\n", r + 1, t[r], now() - t_start);
        fflush(stdout);
        if (do_phys) {
            char tag[16];
            snprintf(tag, sizeof tag, "run%d", r + 1);
            phys_report(tag);
        }
    }

    double mean = 0, sd = 0;
    for (int r = 0; r < RUNS; r++) mean += t[r];
    mean /= RUNS;
    for (int r = 0; r < RUNS; r++) sd += (t[r] - mean) * (t[r] - mean);
    sd = RUNS > 1 ? sqrt(sd / (RUNS - 1)) : 0.0;

    printf("RESULT mean %.3f s   sd %.3f s   (%.2f%%)\n", mean, sd, mean ? 100.0 * sd / mean : 0.0);
    if (do_verify) printf("DIGEST %s\n", digest);
    phys_report("end");
    phys_verdict();
    printf("%s\n", (mean && 100.0 * sd / mean <= 2.0)
           ? "GATE PASS: variance <= 2% -- usable as a control"
           : "GATE FAIL: variance > 2% -- too noisy to validate a later regression");

    if (hold_secs) { printf("holding %d s (pid %d) for targeting\n", hold_secs, getpid());
                     fflush(stdout); sleep(hold_secs); }
    return 0;
}
