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
#include "sha256.h"

static size_t N = 8192;          /* W is N*N floats: 8192 -> 256 MiB, the window size */
static int    ITERS = 100;
static int    RUNS = 10;
static int    do_verify = 0, do_protect = 0, do_ranges = 0, hold_secs = 0;
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

int main(int argc, char **argv)
{
    static struct option lo[] = {
        {"n",1,0,'n'}, {"iters",1,0,'i'}, {"runs",1,0,'r'},
        {"verify",0,0,'V'}, {"protect-weights",0,0,'P'},
        {"print-ranges",0,0,'R'}, {"hold",1,0,'H'}, {"seed",1,0,'S'}, {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "n:i:r:VPRH:S:", lo, NULL)) != -1) {
        switch (c) {
        case 'n': N = strtoul(optarg, NULL, 0); break;
        case 'i': ITERS = atoi(optarg); break;
        case 'r': RUNS = atoi(optarg); break;
        case 'V': do_verify = 1; break;
        case 'P': do_protect = 1; break;
        case 'R': do_ranges = 1; break;
        case 'H': hold_secs = atoi(optarg); break;
        case 'S': SEED = strtoull(optarg, NULL, 0); break;
        default:
            fprintf(stderr,
              "usage: %s [--n DIM] [--iters K] [--runs R] [--verify]\n"
              "          [--protect-weights] [--print-ranges] [--hold SECS] [--seed S]\n", argv[0]);
            return 2;
        }
    }

    Wbytes = N * N * sizeof(float);
    xbytes = N * sizeof(float);
    ybytes = N * sizeof(float);

    W = alloc_region(Wbytes);
    x = alloc_region(xbytes);
    y = alloc_region(ybytes);

    rng_s = SEED ? SEED : 1;
    for (size_t i = 0; i < N * N; i++) W[i] = rnd();
    for (size_t i = 0; i < N; i++)     x[i] = rnd();
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

    double *t = calloc(RUNS, sizeof(double));
    char digest[65] = "", d2[65];

    for (int r = 0; r < RUNS; r++) {
        memset(y, 0, ybytes);
        double t0 = now();
        for (int it = 0; it < ITERS; it++) {
            /* y accumulates every iteration: continuously written.
             * W is read N*N times per iteration and never touched. */
            for (size_t i = 0; i < N; i++) {
                const float *row = W + i * N;
                float acc = 0.0f;
                for (size_t j = 0; j < N; j++) acc += row[j] * x[j];
                y[i] += acc;
            }
        }
        t[r] = now() - t0;

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
        fflush(stdout);
    }

    double mean = 0, sd = 0;
    for (int r = 0; r < RUNS; r++) mean += t[r];
    mean /= RUNS;
    for (int r = 0; r < RUNS; r++) sd += (t[r] - mean) * (t[r] - mean);
    sd = RUNS > 1 ? sqrt(sd / (RUNS - 1)) : 0.0;

    printf("RESULT mean %.3f s   sd %.3f s   (%.2f%%)\n", mean, sd, mean ? 100.0 * sd / mean : 0.0);
    if (do_verify) printf("DIGEST %s\n", digest);
    printf("%s\n", (mean && 100.0 * sd / mean <= 2.0)
           ? "GATE PASS: variance <= 2% -- usable as a control"
           : "GATE FAIL: variance > 2% -- too noisy to validate a later regression");

    if (hold_secs) { printf("holding %d s (pid %d) for targeting\n", hold_secs, getpid());
                     fflush(stdout); sleep(hold_secs); }
    return 0;
}
