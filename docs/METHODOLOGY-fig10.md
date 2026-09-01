# fig10 / fig10b — what each workload actually is

Working notes. Every command below is copied from `scripts/run_decay.sh`;
nothing here is a description from memory.

---

## 0. Terminology I should not have used

**"KV"** = key-value store. It means redis and memcached, nothing more.
**"KV leg"** = the part of the overnight run that does those two. Dropped.

**"pass"** = one sweep down the T ladder. Clear both bits, then read them back
at 60 s, 120 s, 180 s ... without clearing again. One pass yields one sample of
every T on the ladder.

**"round"** = one sweep over all five workloads.

---

## 1. What is measured, for all five identically

Two per-page bits, on the process's own pages:

| bit | what sets it | how it is read | privilege |
|---|---|---|---|
| soft-dirty | any store to the page | `/proc/PID/pagemap` bit 55 | none |
| accessed (young) | any access, by hardware | `smaps` `Referenced:` (a **count**) | none |

At the start of a pass: `clear_refs=3` (clears young), then `clear_refs=4`
(clears soft-dirty). Then, at each T on the ladder, read both back **without
clearing again**. Both bits are sticky, so the reading at time t is "touched
anywhere in [t0, t]", and:

    write-cold(T) = fraction of the t0 pages not written in [t0, t0+T]
    cold(T)       = fraction of the t0 pages not accessed in [t0, t0+T]

The population is fixed at t0 and a page is marked written **once and
permanently**, when it is seen dirty or when it disappears. That makes both
curves monotone in T by construction. A page freed and reallocated cannot
usefully live on flash, so counting it as written is both conservative and
correct for the decision the number informs. The `freed_pct` column reports how
much of the t0 population churned.

**y in fig10 is the share of the process's resident pages**, anonymous and
file-backed alike, excluding only `[vsyscall]`, `[vvar]` and `[vdso]`.

---

## 2. The three compute workloads

### pagerank

```
OMP_NUM_THREADS=16 gapbs/pr -f kron22.sg -n 5000000 -i 20
```

* **Graph:** Kronecker, scale 22 — 4.2 M vertices, 67 M edges, serialised to a
  522 MB `.sg` file and loaded with `-f`. Generated once with
  `converter -g 22`.
* **Why `-f` and not `-g 22`:** generating in-process writes a 134 MB EdgeList
  and then writes the whole CSR before either becomes read-only. The scanner
  would watch 32,768 pages of transient scratch. The serialised reader uses
  `ifstream::read` into `new[]` buffers, so the CSR still lands in **anonymous**
  memory and the write-once-then-read pattern is preserved.
* **`-n 5000000`:** trials, not iterations. Each trial is a full PageRank of up
  to `-i 20` iterations. The count is deliberately far more than needed — at
  ~18 ms per trial, 100,000 ran out after ~30 minutes and killed the workload
  mid-pass in every round of an earlier run.
* **What is read vs written:** the CSR arrays (`out_index_`, `in_index_`,
  `out_neigh_`, `in_neigh_`) are read every iteration and never written. Two
  float arrays of length N (`scores`, `outgoing_contrib`) are written every
  iteration. That is the read/write split the figure is measuring.
* **Resident:** ~558 MiB. `freed_pct` ~5.7% — the score array is freed and
  reallocated between trials, which is the churn the fixed-population rule
  exists to handle.

### bfs

```
OMP_NUM_THREADS=16 gapbs/bfs -f kron22.sg -n 5000000
```

Same graph, same oracle, completely different access character: a
frontier-driven, direction-optimising sweep with low reuse per page. Resident
~544 MiB.

### llama.cpp

```
llama.cpp/build/bin/llama-cli -m models/tinyllama-1.1b-q4.gguf \
    -t 16 -c 4096 -n -1 -st --context-shift --ignore-eos --no-warmup \
    -p Write_a_long_technical_report_about_memory_systems.
```

* **Model:** TinyLlama 1.1B, Q4_K_M, 636 MiB GGUF.
* **`-n -1 --context-shift --ignore-eos`:** generate until killed. Without
  `--ignore-eos` it stops at the first end-of-sequence token — measured, that
  was 30 seconds of a 600 second window. The run length must be the census's
  decision, not the model's.
* **`-st`** is single-turn, so it is not interactive.
* **Resident ~1,221 MiB, and it is four things**, from llama.cpp's own
  accounting:

      CPU_Mapped model buffer   636.18 MiB   mmap'd GGUF, file-backed
      CPU_REPACK model buffer   455.06 MiB   repacked copy, anonymous
      KV cache                   88.00 MiB   anonymous, 75% written
      compute buffer             53.01 MiB   anonymous

  The CPU reports `REPACK = 1`, so llama.cpp copies the repack-eligible Q4_K
  tensors into an AVX512-friendly layout at load and reads that copy on every
  token. The original mapping then goes 77% untouched — embeddings, norms and
  the output head are still read from it, which is the other 23%. **That
  abandoned 490 MiB is llama's entire 41% cold band.** It is real resident
  memory, but it is clean file-backed memory the kernel can drop for free.

---

## 3. redis and memcached

Both are driven by **one** generator, `scripts/kv_load.py`, so the two runs
differ by the server and nothing else — same keys, same value size, same skew,
same read ratio, same offered rate.

```
redis-server --save '' --appendonly no --protected-mode no      # port 6379
memcached -m 4096 -t 8 -u nobody                                # port 11211
```

`--save ''` and `--appendonly no` disable both persistence paths. Without them
redis forks a background save and writes an RDB, which would appear in the
measurement as write traffic that has nothing to do with the workload.

Each server then goes through three steps:

**1. Load.** 7,000,000 keys x 140 B values, 8 threads, sequential keys, blocking
until done. ~56 s for redis, ~76 s for memcached.

Why 7 M x 140 B and not the earlier 700 k x 1400 B: millions of small objects at
a moderate per-instance rate is the published memcached/Twitter shape. 700 k x
1400 B was the outlier. Note the **resident** figures are larger than the
0.91 GiB of payload — redis 1,779 MiB, memcached 1,671 MiB — because of per-key
overhead (dictEntry, robj, sds headers, slab rounding). The census counts
resident pages, so that overhead is correctly in the denominator.

**2. Settle, 150 s.** A bulk load leaves memcached's LRU maintainer crawling
every slab: **98.6% of pages dirtied in the 15 s after a load, 0.1% once
settled.** A census started immediately would measure the crawl, not the
workload. Measured, not assumed.

**3. Drive, then measure.**

```
kv_load.py --proto redis --keys 7000000 --value 140 \
           --no-load --read-ratio 0.95 --ops-per-sec 10000 --threads 8
```

* **95/5 read/write**, zipfian theta 0.99, using YCSB's own ZipfianGenerator
  (zeta precomputed, then inverted). On 200 k keys that puts 52% of traffic on
  the top 1% of keys. The earlier sampler was `random() ** (1/(1-0.99))`, an
  exponent of 100 — a far heavier skew than zipf 0.99 — so runs before
  2026-09-01 were mislabelled.
* **10,000 ops/s**, paced per thread. Unthrottled, the generator rewrites the
  whole keyspace in minutes and the measurement describes the generator rather
  than the engine.
* 15 s of traffic before the census starts.

### The finding that makes these two different from the other three

**A read dirties the page it reads.** Pure GET traffic, zero client writes,
15 s, on a quiet machine:

    idle, no client traffic       memcached  0.1%   redis  0.0%
    GETs, 1000 hot keys only                 1.6%          1.4%
    GETs, all 200k keys                     95.3%         95.1%
    idle again                               0.1%          0.0%

memcached bumps `it->time` and relinks the LRU on every GET; redis writes
`robj->lru` on every `lookupKey`. Both fields share a page with the data. The
idle-before-and-after bracket is what proves this is the lookup path and not
the post-load crawl — without it, the first reading of a fresh server is 98.6%
and reads as a result.

So the steep decay of these two curves in fig10 is not "writes ageing out". It
is read coverage: at 10,000 ops/s over 7 M keys, the fraction of pages that
have been *touched at all* grows with T, and touching is enough.

---

## 4. Settings shared by all five

| | |
|---|---|
| threads | 16 (of 32 cores) |
| ladder | 5, 10, 20, 30, 45, 60, 90, 120, 180, 240, 300, 360, 480, 600 s |
| passes | 3 per round, 3 rounds, pooled weighted by pass count |
| warmup before the first pass | 45 s (compute), 60 s (llama), 165 s (servers) |
| host | ba8, x86_64, 32 cores, 61 GB |

**One workload at a time, always.** Two at once share the page cache and the
memory controller, and a stray process from a smoke test once ran at 2592% CPU
across all 32 cores for 82 minutes and invalidated an entire 55-minute census.
`run_decay.sh` refuses to start if a workload process is already running.

---

## 5. What this measurement is not

* **Not the LtRAM hardware.** This runs on an ordinary x86 box. It measures the
  *opportunity* — how much memory is read-mostly — not what happens when it is
  placed on flash. That is fig7/fig8/fig9, on z08.
* **Not system-wide.** Per-process VMAs only. Page cache belonging to other
  processes, and kernel memory, are not counted.
* **cold is a proxy.** Reading the accessed bit per page needs
  `/sys/kernel/mm/page_idle`, which is root-only, and we have no sudo on ba8.
  `smaps Referenced:` gives a count, not a set. The ladder recovers a curve from
  counts, but it cannot attribute: a shared library page another process touches
  counts as referenced here. Workloads dominated by private anonymous memory or
  one large private mapping — all five of these — are barely affected. Root
  would not fix attribution either; `page_idle` is per physical page and clears
  young across every mapping via rmap.
