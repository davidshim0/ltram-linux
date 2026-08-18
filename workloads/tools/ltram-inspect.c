/*
 * ltram-inspect — per-page provenance for a target process.
 *
 * Answers the question step 6 actually needs answered: not "how many pages were
 * promoted" (a counter can say 261,890 while the data never moved) but "WHICH
 * pages are on flash, and are they the ones that should be".
 *
 * Resolves every page of the named ranges through /proc/PID/pagemap and reports
 * which side of the LtRAM pfn boundary each landed on. The boundaries come from
 * the kernel itself via debugfs, so this cannot drift from the running config.
 *
 * Usage:
 *   ltram-inspect <pid> <name> <startaddr> <len> [<name> <start> <len> ...]
 *
 * Feed it the RANGE lines matmul --print-ranges emits.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>

#define PM_PRESENT   (1ULL << 63)
#define PM_SWAPPED   (1ULL << 62)
#define PM_PFN_MASK  ((1ULL << 55) - 1)

static unsigned long rd_ulong(const char *path)
{
    FILE *f = fopen(path, "r");
    unsigned long v = 0;
    if (!f) return 0;
    if (fscanf(f, "%lu", &v) != 1) v = 0;
    fclose(f);
    return v;
}

int main(int argc, char **argv)
{
    if (argc < 5 || (argc - 2) % 3) {
        fprintf(stderr, "usage: %s <pid> <name> <start> <len> [...]\n", argv[0]);
        return 2;
    }
    pid_t pid = atoi(argv[1]);

    unsigned long lo = rd_ulong("/sys/kernel/debug/ltram/start_pfn");
    unsigned long hi = rd_ulong("/sys/kernel/debug/ltram/end_pfn");
    if (!hi) {
        fprintf(stderr, "ltram: no /sys/kernel/debug/ltram (need root, debugfs, CONFIG_LTRAM)\n");
        return 3;
    }
    printf("LtRAM pfn range: %lu .. %lu  (%lu pages)\n", lo, hi, hi - lo);

    char path[64];
    snprintf(path, sizeof path, "/proc/%d/pagemap", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return 4; }

    long psz = sysconf(_SC_PAGESIZE);
    int fail = 0;

    for (int a = 2; a + 2 < argc; a += 3) {
        const char *name = argv[a];
        uint64_t start = strtoull(argv[a+1], NULL, 0);
        uint64_t len   = strtoull(argv[a+2], NULL, 0);
        uint64_t pages = (len + psz - 1) / psz;
        uint64_t in_lt = 0, in_dram = 0, absent = 0;

        for (uint64_t i = 0; i < pages; i++) {
            uint64_t vaddr = start + i * psz, ent;
            off_t off = (vaddr / psz) * sizeof(uint64_t);
            if (pread(fd, &ent, sizeof ent, off) != sizeof ent) { absent++; continue; }
            if (!(ent & PM_PRESENT)) { absent++; continue; }
            unsigned long pfn = ent & PM_PFN_MASK;
            if (pfn >= lo && pfn < hi) in_lt++; else in_dram++;
        }

        double pct = pages ? 100.0 * in_lt / pages : 0.0;
        printf("%-10s %8" PRIu64 " pages   %8" PRIu64 " LtRAM (%5.1f%%)   %8" PRIu64 " DRAM   %6" PRIu64 " not present\n",
               name, pages, in_lt, pct, in_dram, absent);

        /*
         * A result buffer with ANY page on flash is a failure, not a low
         * percentage: it is written continuously, so it would fault straight
         * back, and its presence means the policy is misreading dirty state.
         */
        if (!strcmp(name, "result") && in_lt) {
            printf("  ^ FAIL: the result buffer is written every iteration and must never be promoted\n");
            fail = 1;
        }
    }
    close(fd);
    return fail;
}
