/*
 * cache-probe — find the cache topology directly, by working-set size.
 *
 * matmul cannot do this. Its inner loop is a serial FMA chain with a 3.566
 * ns/element floor, so memory is hidden wherever the cache keeps up and the L1
 * boundary shows as a 9% ripple. This probe does one thing instead: ONE
 * DEPENDENT LOAD PER 128 B LINE, and nothing else.
 *
 * WHY DEPENDENT. The next address is not known until the current load returns,
 * so there is no prefetch to exploit and no memory-level parallelism to hide
 * behind: the number is a LATENCY. That distinction already cost this project
 * once -- DESIGN-DECISIONS 0.3, where lat_pass() timed a sequential sweep of
 * INDEPENDENT loads and reported 858 ns as a latency when it was a throughput
 * figure, inflated ~9% by its own accumulator. --seq reproduces that mistake on
 * purpose: same chain, ascending order, so prefetch and TLB locality both help.
 * The gap between --seq and the default IS the prefetch benefit.
 *
 * WHY ONE ACCESS PER 128 B LINE. Two loads in the same line would make the
 * second a hit regardless of where the line came from, and the measurement
 * would be diluted by the line size rather than reporting it.
 *
 * AT EACH SIZE, TWO NUMBERS:
 *   COLD  a scrub buffer larger than L2 is streamed first, so the chain is
 *         guaranteed evicted; one pass then measures the cost of fetching from
 *         wherever the data actually lives -- DRAM, at every size.
 *   WARM  the identical chain, immediately repeated, so it is served by
 *         whichever level now holds it.
 *
 * COLD/WARM IS THE TOPOLOGY. Above the LLC they converge: a pass evicts what it
 * loads, so a second pass is no better off. Below a level they diverge sharply.
 * Every knee in WARM is a cache boundary, and COLD staying flat across the whole
 * sweep is the control proving the scrub works.
 *
 * There is no unprivileged full-cache-invalidate on aarch64 (the same
 * constraint that forces CVMCACHE with a physical address for the flash
 * read-back verify), so the scrub buffer is the only mechanism available.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/mman.h>

#define LINE 128                  /* getconf LEVEL1_DCACHE_LINESIZE on z08 */

static size_t max_bytes = 256UL << 20;
static size_t min_bytes = 4UL   << 10;
static size_t scrub_bytes = 64UL << 20;   /* >= 4x the 16 MiB L2 */
static int    halve = 0, seq_order = 0, cold_reps = 10, warm_reps = 5;
static uint64_t seed = 20260818;

static char *region, *scrub_buf;
static volatile uint64_t sink;

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static uint64_t rs;
static uint64_t rnd64(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return rs;
}

static void *xalloc(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    return p;
}

/* Link every 128 B slot into one cycle. Random order defeats the prefetcher and
 * the TLB; --seq walks ascending so both help, which is the contrast. */
static void build_chain(size_t nlines)
{
    size_t *ord = malloc(nlines * sizeof *ord), i;

    if (!ord) { fprintf(stderr, "oom building chain\n"); exit(1); }
    for (i = 0; i < nlines; i++)
        ord[i] = i;
    if (!seq_order) {
        rs = seed ? seed : 1;
        for (i = nlines - 1; i > 0; i--) {
            size_t j = (size_t)(rnd64() % (i + 1)), t = ord[i];
            ord[i] = ord[j]; ord[j] = t;
        }
    }
    for (i = 0; i < nlines; i++)
        *(void **)(region + ord[i] * LINE) =
            region + ord[(i + 1) % nlines] * LINE;
    free(ord);
}

/* The whole measurement. Not inlined away: p is consumed by the caller. */
static void *chase(void *p, size_t steps)
{
    while (steps--)
        p = *(void **)p;
    return p;
}

static void scrub(void)
{
    size_t i;
    for (i = 0; i < scrub_bytes; i += LINE)
        scrub_buf[i] = (char)(i >> 7);
    sink += (uint64_t)scrub_buf[0];
}

static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* COLD takes the MEDIAN, not the minimum.
 *
 * min is right for WARM -- the fastest pass is the one least disturbed by the
 * scheduler, and every pass is equally resident. It is WRONG for COLD, where
 * the repetitions are not equivalent: whichever one happened to retain the most
 * residual cache is the fastest, so min systematically reports the LEAST cold
 * repetition. That produced a non-monotonic COLD column -- 44 ns at 64 KiB and
 * 9 ns at 16 KiB -- which is not a property of any memory system.
 */
static double median(double *v, size_t n)
{
    qsort(v, n, sizeof *v, cmp_dbl);
    return n & 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

static const char *human(size_t b, char *buf, size_t n)
{
    if (b >= (1UL << 20)) snprintf(buf, n, "%.1fM", b / 1048576.0);
    else                  snprintf(buf, n, "%.0fK", b / 1024.0);
    return buf;
}

int main(int argc, char **argv)
{
    static struct option lo[] = {
        {"max-mb",1,0,'M'}, {"min-kb",1,0,'m'}, {"scrub-mb",1,0,'s'},
        {"halve",0,0,'H'},  {"seq",0,0,'q'},    {"cold-reps",1,0,'c'},
        {"warm-reps",1,0,'w'}, {"seed",1,0,'S'}, {0,0,0,0}
    };
    size_t bytes, prev_lines = 0;
    double prev_warm = 0;
    char hb[32];
    int c;

    while ((c = getopt_long(argc, argv, "M:m:s:Hqc:w:S:", lo, NULL)) != -1) {
        switch (c) {
        case 'M': max_bytes   = strtoul(optarg,NULL,0) << 20; break;
        case 'm': min_bytes   = strtoul(optarg,NULL,0) << 10; break;
        case 's': scrub_bytes = strtoul(optarg,NULL,0) << 20; break;
        case 'H': halve = 1; break;
        case 'q': seq_order = 1; break;
        case 'c': cold_reps = atoi(optarg); break;
        case 'w': warm_reps = atoi(optarg); break;
        case 'S': seed = strtoull(optarg,NULL,0); break;
        default:
            fprintf(stderr, "usage: %s [--max-mb 256] [--min-kb 4] [--scrub-mb 64]\n"
                            "          [--halve] [--seq] [--cold-reps N] [--warm-reps N]\n",
                    argv[0]);
            return 2;
        }
    }

    region    = xalloc(max_bytes);
    scrub_buf = xalloc(scrub_bytes);
    memset(scrub_buf, 1, scrub_bytes);      /* fault it in, once */
    memset(region, 0, max_bytes);           /* fault it in, once: no page fault
                                             * may land inside a timed pass */

    printf("cache-probe  region %zu MiB  scrub %zu MiB  line %d B  order %s\n",
           max_bytes >> 20, scrub_bytes >> 20, LINE,
           seq_order ? "sequential (prefetch + TLB HELP -- contrast run)"
                     : "random (prefetch and TLB defeated -- true latency)");
    {   /* state the scrub cost so "is it actually evicting?" has a number */
        double t0 = now(); scrub(); scrub();
        printf("scrub          %.1f ms per pass over %zu MiB\n",
               500.0 * (now() - t0), scrub_bytes >> 20);
    }
    printf("%9s %10s %10s %10s %8s %9s\n",
           "WORKSET", "LINES", "COLD_ns", "WARM_ns", "COLD/WARM", "WARM_STEP");

    for (bytes = max_bytes; bytes >= min_bytes; bytes = halve ? bytes / 2
                                                             : (size_t)(bytes / 1.4142)) {
        size_t nlines = bytes / LINE, mult, r;
        double cold, warm = 1e30;
        double *creps = malloc((size_t)cold_reps * sizeof *creps);

        if (!creps) { fprintf(stderr, "oom\n"); return 1; }

        if (nlines < 2)
            break;
        bytes = nlines * LINE;                   /* snap to a whole line */
        build_chain(nlines);

        for (r = 0; r < (size_t)cold_reps; r++) {
            double t0;
            scrub();
            t0 = now();
            sink += (uint64_t)(uintptr_t)chase(region, nlines);
            creps[r] = (now() - t0) / (double)nlines;
        }
        cold = median(creps, (size_t)cold_reps);
        free(creps);

        /* Enough steps that a warm pass is long against the clock, and the
         * chain is fully resident before the first timed warm pass. */
        mult = nlines < 2000000 ? (2000000 / nlines) : 1;
        sink += (uint64_t)(uintptr_t)chase(region, nlines * mult);
        for (r = 0; r < (size_t)warm_reps; r++) {
            double t0 = now(), d;
            sink += (uint64_t)(uintptr_t)chase(region, nlines * mult);
            d = (now() - t0) / (double)(nlines * mult);
            if (d < warm) warm = d;
        }

        printf("%9s %10zu %10.2f %10.2f %8.1fx ",
               human(bytes, hb, sizeof hb), nlines, cold * 1e9, warm * 1e9,
               warm > 0 ? cold / warm : 0.0);
        if (prev_lines)
            printf("%+8.1f%%\n", 100.0 * (warm / prev_warm - 1.0));
        else
            printf("%8s\n", "--");
        fflush(stdout);
        prev_warm = warm; prev_lines = nlines;
    }

    printf("\nWARM is the topology: a knee is where WARM_STEP jumps and then settles.\n"
           "\nCOLD is a DRAM latency only while the working set spans enough pages that\n"
           "the prefetcher cannot amortise the fill. Below roughly 1 MiB a cold pass\n"
           "touches few enough pages that hardware prefetch fills them wholesale, and\n"
           "COLD becomes a bandwidth figure -- do not read the small-size COLD numbers\n"
           "as latencies. Its diagnostic value is at the top of the sweep: if COLD\n"
           "there is not flat near the DRAM latency, the scrub is not evicting and\n"
           "nothing below can be trusted either.\n");
    return 0;
}
