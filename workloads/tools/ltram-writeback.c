/*
 * ltram-writeback.c -- does a write to a flash-resident page come back correctly?
 *
 * WHY THIS EXISTS. A store to the NOR read window is silently discarded by the
 * hardware: no fault, no error, no signal. So if the kernel ever lets a
 * flash-backed page become writable, the write simply evaporates. Every other
 * test we have would pass while that happened, because they only ever read.
 *
 * The sequence:
 *   1. fill a region with pattern A and stop writing to it
 *   2. read it in a loop, so the policy classifies it read-mostly and promotes it
 *   3. when told to (a sentinel file appears), overwrite every page with pattern B
 *   4. read back and check every word
 *
 * A page still on flash reads back as A. A page correctly demoted to DRAM reads
 * back as B. The count of A-valued words after step 3 is the whole result.
 *
 * The pattern is position-dependent, so a page that came back from the WRONG
 * physical location is caught too, not just one that never took the write.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

#define PAGE 4096UL

static double now(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
/* Position-dependent so a page restored from the wrong place fails too. */
static uint64_t pat(uint64_t idx, uint64_t epoch) { return idx * 0x9E3779B97F4A7C15ULL ^ epoch; }

int main(int argc, char **argv)
{
    size_t mb = 32; const char *go = "/scratch/hushim/GO"; int maxwait = 900;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mb") && i + 1 < argc)      mb = strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--go") && i + 1 < argc) go = argv[++i];
        else if (!strcmp(argv[i], "--maxwait") && i+1<argc) maxwait = atoi(argv[++i]);
        else { fprintf(stderr, "usage: %s [--mb N] [--go PATH] [--maxwait S]\n", argv[0]); return 2; }
    }
    size_t bytes = mb << 20, words = bytes / 8;
    uint64_t *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }

    for (size_t i = 0; i < words; i++) p[i] = pat(i, 0xA);
    printf("writeback pid=%d  region %zu MiB  %zu pages\n", getpid(), mb, bytes / PAGE);
    printf("RANGE data %p %p %zu\n", (void *)p, (void *)((char *)p + bytes), bytes);
    printf("filled with pattern A; now READ-ONLY from here until the go file appears\n");
    fflush(stdout);

    /* Phase 2: read only. Never touch the region for write -- that is the whole
     * point. The sink is volatile so this cannot be optimised into nothing. */
    volatile uint64_t sink = 0;
    double t0 = now(); struct stat sb; int armed = 0;
    while (now() - t0 < maxwait) {
        for (size_t i = 0; i < words; i += PAGE / 8) sink ^= p[i];
        if (!stat(go, &sb)) { armed = 1; break; }
    }
    if (!armed) { printf("TIMEOUT: go file %s never appeared\n", go); return 3; }
    printf("go seen after %.1f s; writing pattern B over %zu pages\n", now() - t0, bytes / PAGE);
    fflush(stdout);

    /* Phase 3: the write that must not vanish. */
    double tw = now();
    for (size_t i = 0; i < words; i++) p[i] = pat(i, 0xB);
    double wsec = now() - tw;

    /* Phase 4: verdict. */
    size_t stale = 0, wrong = 0, first = (size_t)-1;
    for (size_t i = 0; i < words; i++) {
        uint64_t v = p[i];
        if (v == pat(i, 0xB)) continue;
        if (v == pat(i, 0xA)) { if (first == (size_t)-1) first = i; stale++; }
        else                  { if (first == (size_t)-1) first = i; wrong++; }
    }
    printf("write took %.3f s (%.1f MiB/s)\n", wsec, mb / wsec);
    printf("STALE  %zu words  (still pattern A -- the write was DISCARDED)\n", stale);
    printf("WRONG  %zu words  (neither A nor B -- corruption)\n", wrong);
    if (first != (size_t)-1)
        printf("first bad word index %zu, page %zu, offset 0x%zx\n", first, first * 8 / PAGE, first * 8);
    printf("sink %llu\n", (unsigned long long)sink);
    if (stale || wrong) { printf("WRITEBACK: FAIL\n"); return 44; }
    printf("WRITEBACK: PASS -- every page took the write and read back correctly\n");
    return 0;
}
