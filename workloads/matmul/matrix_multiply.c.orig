// Matrix multiply two matrices together iteratively, like
// loop {
//    tmp = A @ result
//    result = tmp
// }
//
// where A's data is stored in NUMA node 1 (LtRAM) and
//       result's and tmp's data is stored in NUMA node 0 (DRAM)
//
// NOTE: Make sure your VM is setup with 2 NUMA nodes as
// assumed above before running.
//
// Run `apt install libnuma-dev`if numa.h is missing.
//
// gcc -O3 -march=native matrix_multiply.c -lnuma

#include <numa.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/mman.h>

#define N 1024
#define ITERS 10

static void fill_random(float *x, size_t n, uint64_t seed)
{
    srand(time(NULL)); // Seed the rng
    for (size_t i = 0; i < n; i++) {
      x[i] = (float)rand() / (float)RAND_MAX;
    }
}

static void matmul(float *C, const float *A, const float *B)
{
    // C = A @ B, row-major, naive O(N^3)
    for (int i = 0; i < N; i++) {
        for (int k = 0; k < N; k++) {
            float a_ik = A[(size_t)i * N + k];
            const float *b_k = &B[(size_t)k * N];
            float *c_i = &C[(size_t)i * N];
            for (int j = 0; j < N; j++) {
                c_i[j] += a_ik * b_k[j];
            }
        }
    }
}

void numa_mm_repeat(int iters)
{
    if (numa_available() < 0) {
        fprintf(stderr, "NUMA not available\n");
        return;
    }

    const size_t elems = (size_t)N * N;
    const size_t bytes = elems * sizeof(float);

    float *result = (float *)numa_alloc_onnode(bytes, 0); // node 0
    // A was on node 1 (LtRAM), but ZONE_LTRAM is unreachable via the userspace
    // NUMA API under the zone_ltram kernel (node 1 is out of N_MEMORY and
    // requires __GFP_LTRAM), so numa_alloc_onnode(.,1) returns EINVAL. Keep A on
    // node 0 as a DRAM baseline. TODO: route A into LtRAM via a read-only hint
    // (madvise / read-only file-backed mmap) so the kernel tags it __GFP_LTRAM.
    float *A      = (float *)numa_alloc_onnode(bytes, 0); // node 0 (was node 1/LtRAM)
    float *tmp    = (float *)numa_alloc_onnode(bytes, 0); // node 0

    if (!result || !A || !tmp) {
        fprintf(stderr, "numa_alloc_onnode failed\n");
        if (result) numa_free(result, bytes);
        if (A)      numa_free(A, bytes);
        if (tmp)    numa_free(tmp, bytes);
        return;
    }

    fill_random(result, elems, 0x1234);
    fill_random(A,      elems, 0x9876);

    // debugging: make sure A is actually only read from
    mprotect(A, bytes, PROT_READ);

    for (int t = 0; t < iters; t++) {
        memset(tmp, 0, bytes);
        matmul(tmp, A, result);          // tmp = A @ result
        float *swap = result; result = tmp; tmp = swap;
    }

    volatile float sink = result[0];
    (void)sink;

    numa_free(tmp, bytes);
    numa_free(A, bytes);
    numa_free(result, bytes);
}

int main() {
    numa_mm_repeat(ITERS);
    return 0;
}
