/*
 * Commenting this all out since this is wrong, but parts of it may be reusable later.
// Just check writing to (write-protected) NUMA node 1 causes a CoW
// where the copy is placed on NUMA node 0
//
// NOTE: Make sure your VM is setup with 2 NUMA nodes as
// assumed above before running.
//
// Run `apt install libnuma-dev`if numa.h is missing.
//
// gcc -O3 -march=native matrix_multiply.c -lnuma

#include <stdio.h>
#include <numa.h>
#include <numaif.h>
#include <sys/mman.h>

#define DRAM_NODE  0
#define LTRAM_NODE 1

static int get_page_node(void *ptr) {
    int status[1], ret_code;
    void *pages[] = { ptr };
    ret_code = move_pages(0, 1, pages, NULL, status, 0);
    printf("Memory at %p is at %d node (retcode %d)\n", ptr, status[0], ret_code);
    return status[0];
}

static long read_vmstat(const char *key) {
    FILE *f = fopen("/proc/vmstat", "r");
    char k[64]; long val;
    while (fscanf(f, "%s %ld", k, &val) == 2)
        if (!strcmp(k, key)) { fclose(f); return val; }
    fclose(f);
    return -1;
}

int main() {
    // Allocate a buffer on NUMA node 0
    int *dram_buf = (int *)numa_alloc_onnode(4096, 0);

    // Allocate a buffer on NUMA node 1
    int *ltram_buf = (int *)numa_alloc_onnode(4096, 1);



    // Double check that allocation landed on the right nodes
    // int dram_node = get_page_node(dram_buf);
    // int ltram_node = get_page_node(ltram_buf);
    // printf("dram_buf  on node %d (expected %d): %s\n", dram_node,  DRAM_NODE,  dram_node  == DRAM_NODE  ? "OK" : "FAIL");
    // printf("ltram_buf on node %d (expected %d): %s\n", ltram_node, LTRAM_NODE, ltram_node == LTRAM_NODE ? "OK" : "FAIL");

    // Write-protect the ltram buffer so writes fault
    // mprotect(ltram_buf, 4096, PROT_READ);

    // Write to DRAM and check this doesn't cause a CoW
    long cow_before = read_vmstat("pgmigrate_success");

    dram_buf[0] = 42;

    long cow_after = read_vmstat("pgmigrate_success");
    // int dram_node_after = get_page_node(dram_buf);
    printf("\nDRAM write:\n");
    printf("  value written: %d\n", dram_buf[0]);
    // printf("  page still on node %d: %s\n", dram_node_after, dram_node_after == DRAM_NODE ? "OK" : "FAIL");
    printf("  migrations delta: %ld (expected 0)\n", cow_after - cow_before);

    long cow_before2 = read_vmstat("pgmigrate_success");

    ltram_buf[0] = 99;

    long cow_after2 = read_vmstat("pgmigrate_success");
    // int ltram_node_after = get_page_node(ltram_buf);
    printf("\nLtRAM write:\n");
    printf("  value written: %d\n", ltram_buf[0]);
    // printf("  page now on node %d (expected %d): %s\n", ltram_node_after, DRAM_NODE, ltram_node_after == DRAM_NODE ? "OK" : "FAIL");
    printf("  migrations delta: %ld (expected 1)\n", cow_after2 - cow_before2);

    numa_free(dram_buf, 4096);
    numa_free(ltram_buf, 4096);
    return 0;
}

*/
