// LtRAM read-only -> writable stress test.
//
// Purpose: deliberately exercise the case the read-only routing policy is
// betting against -- a page that is read-only at fault time (so it gets placed
// in LtRAM) but is LATER made writable and written. This is the event the
// write-fault detector in do_wp_page() should catch, and the case the (not yet
// implemented) repatriation path would need to handle.
//
// Sequence:
//   1. Create a backing file on the rootfs and map it PROT_READ, MAP_SHARED.
//   2. Read-fault every page. Because the VMA is read-only at fault time, the
//      kernel routes these allocations to ZONE_LTRAM (node 1). Confirm via the
//      profiler's node-1 numa_hit delta and/or the per-page routing log.
//   3. mprotect() the mapping to PROT_READ|PROT_WRITE and write every page.
//      Each write now lands on a page that may be sitting in LtRAM -> write
//      fault -> "ltram: write-fault on LtRAM page" in dmesg, and node-1 Dirty
//      should move if the page is dirtied in place.
//
// Build static (see Makefile) so it runs in the buildroot guest unchanged.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>

#define PGSZ 4096

int main(int argc, char **argv)
{
    long npages = (argc > 1) ? strtol(argv[1], NULL, 10) : 256;  // default 1 MB
    const char *path = (argc > 2) ? argv[2] : "/root/.ltram_rw_stress.dat";
    size_t len = (size_t)npages * PGSZ;

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror("open"); return 1; }
    if (ftruncate(fd, len) < 0) { perror("ftruncate"); close(fd); return 1; }

    // 1+2) read-only shared mapping, then read-fault every page -> routes to LtRAM
    char *p = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("mmap PROT_READ"); close(fd); return 1; }

    volatile long sink = 0;
    for (size_t i = 0; i < len; i += PGSZ)
        sink += p[i];                 // read fault: VMA is read-only here
    (void)sink;
    printf("[rwstress] read-only phase: %ld pages read-faulted (should land in LtRAM)\n",
           npages);

    // 3) make it writable and write every page -> write-fault on (LtRAM?) pages
    if (mprotect(p, len, PROT_READ | PROT_WRITE) < 0) {
        perror("mprotect RW"); munmap(p, len); close(fd); return 1;
    }
    for (size_t i = 0; i < len; i += PGSZ)
        p[i] = 0x5a;                  // write fault: page may be in LtRAM
    printf("[rwstress] write phase: %ld pages written after mprotect(PROT_WRITE)\n",
           npages);

    msync(p, len, MS_SYNC);
    munmap(p, len);
    close(fd);
    unlink(path);
    return 0;
}
