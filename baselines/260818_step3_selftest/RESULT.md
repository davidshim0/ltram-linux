# Step 3 — boot-time read self-test

**Built 2026-08-18. NOT YET BOOTED.**

Kernel `6.8.0-ltram`, tag `step3-selftest-2026-08-18` → `72f79aa3d8` on `ltram-arm64`.

**No initrd in this archive on purpose.** Only built-in code changed, so the release
string and the module set are identical to step 2 — use
`baselines/step2-zone-2026-08-18/initrd.img`.

## What it proves, and what it cannot

Proves the window is **mapped, coherent and readable from kernel context**. That is a
different claim from "the node appears in `/proc/zoneinfo`", and it is the one that
separates a working coherent mapping from a plausible-looking one.

It **only reads**. The CPU physically cannot store here — data reaches the array through
the FPGA's DMA ring — so the write half of the self-test belongs after the driver
registers a backend, in step 4.

## Why it reports rather than asserts

The harness writes `word = (byte offset) ^ seed`, but **the seed is per sector**, so no
single seed describes the device. Asserting one would report mismatches on healthy
hardware, and a test that cries wolf trains you to ignore it.

So by default it prints a fingerprint — erased-word count, zero count, checksum, and the
first four `(offset, value)` pairs — to cross-check against a harness dump. Verification
is opt-in:

```
ltram_selftest_seed=0x11        # only for a region filled with ONE known seed
ltram_selftest_words=4096       # default 1024
```

**The load-bearing result is reaching the end at all.** That means the window was mapped
and read without an external abort.

## Expected boot output

```
ltram: node 1 = 0x14000000000 + 256 MiB, pfn ... (65536 pages)
ltram: self-test sample +0x00000000 = ........
ltram: self-test read 1024 words, stride 4099: erased(ff) N, zero N, checksum ........
ltram: self-test completed without abort -- window is readable from kernel context
```

Absence of the last line means the read aborted — check the FPGA is programmed before
suspecting the kernel.

## Design notes worth keeping

**Late initcall, deliberately.** Reading before the FPGA is programmed and the ECI link is
up is an external abort, and an abort there is a dead boot with nothing after it on the
console.

**Stride 4099 words**, coprime with both the 4 KiB sector and the 128 B line, so the walk
crosses sector and line boundaries instead of sitting in one region. The known read-side
fault on this hardware appears only on the **first word of a line**; a test that never
lands there cannot see it.
