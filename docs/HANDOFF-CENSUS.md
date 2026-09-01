# Read this, then run the census on a host where you have root

Written 1 September 2026 for the next agent. You are picking up one specific,
self-contained job. You do **not** need the LtRAM hardware, the arm64 kernel,
the Enzian board, or `zuestoll08`. Everything here runs on an ordinary x86_64
Linux box. The only thing you need that the previous session did not have is
**root**.

Repo: `git@github.com:davidshim0/ltram-linux.git`, branch `step6-refcount`.

---

## 1. What this measurement is for

LtRAM places **read-mostly** pages on NOR flash. NOR reads are fast (959 ns
measured, against 175 ns for DRAM); NOR writes are catastrophic (a 20 ms erase
blocks a concurrent read for its whole duration). So the policy moves pages
that are *read but not written*.

Every prior far-memory system moves pages that are **not accessed** — Google's
software-defined far memory, Meta's TMO. That set is strictly smaller:

    not accessed in T   =>   not written in T
    the converse is false

A page read a million times a second and never written is **write-cold and not
cold**. It is invisible to every access-based tiering system and it is exactly
what LtRAM can move.

**The objective of this job is one figure**: for each of five workloads, the
share of memory that is write-cold against the share that is cold, as a
function of the age threshold T. If the write-cold share is much larger, the
premise of the project holds. Pilot data says it is: pagerank is 0.3% cold and
94.2% write-cold.

---

## 2. What root buys, and what it does not

Read this carefully — an earlier draft of this document overstated the case and
was corrected.

**Cold as a function of T is measurable WITHOUT root.** Both bits are sticky:
once set they stay set until something clears them. So clear once at t0 and
read repeatedly without clearing again, and the count at time t is "pages
touched anywhere in [t0, t]". A count is all this statistic needs:

    cold(T)       = 1 - Referenced(t0+T) / Rss        <- smaps, unprivileged
    write-cold(T) = 1 - soft_dirty(t0+T) / resident   <- pagemap, unprivileged

`scripts/decay_census.py` does exactly this and needs no privilege. Do not
arrive believing the measurement is impossible unprivileged. It is not.

**What per-page state actually changes is which INSTANT each point comes from.**

* *Ladder (counts only).* The T=5 s point is measured at t0+5, the T=600 s
  point at t0+600. Different T come from different moments, and one pass yields
  exactly one sample of each T.
* *Per-page ages (`page_idle`).* At any single instant every page's age is
  known, so the whole curve over all T is read off from one moment — and a
  fresh sample of every T arrives at every window boundary.

Three consequences, and only these three:

1. **Sample efficiency.** A 1800 s run gives the ladder 3 samples of T=600 s.
   Per-page ages at a 5 s window give ~240 samples of T=600 s in the same wall
   time. Roughly 80x, and the gap widens with T.
2. **Internal consistency.** Points taken from one instant are monotone in T by
   construction. The artefact in section 4 (bfs write-cold rising with T)
   cannot occur.
3. **One estimator for both columns.** Today write-cold is a sliding-window
   steady-state average and ladder-cold is decay-from-t0. Under stationarity
   they agree in expectation, but they are not the same statistic, and putting
   them in one bar is a small dishonesty. `page_idle` makes both the sliding-
   window statistic — which is also the one Google's `kstaled` computes.

**Root does NOT fix attribution.** An earlier draft claimed it did. It does not.
`page_idle` is per *physical* page and clears the young bit across every mapping
via rmap, so another process touching a shared page confounds it exactly as it
confounds `smaps Referenced:`. An idle `sleep` measures ~12% cold rather than
~100% under either mechanism, because its 1.9 MiB is nearly all shared libc that
other processes keep touching. If attribution matters for a claim, restrict to
private mappings (`--anon-only`, section 3.2) rather than expecting privilege to
solve it.

**So why bother with root at all?** Because of 1-3 above, and because matching
`kstaled`'s estimator makes the comparison against published far-memory numbers
an apples-to-apples one. It is a better measurement, not the only possible one.

The interfaces, for reference:

| bit | per-page read? | interface | privilege |
|---|---|---|---|
| dirty | **yes** | `/proc/PID/pagemap` bit 55 | none |
| accessed | **yes** | `/sys/kernel/mm/page_idle/bitmap` | **root** |
| accessed | no, only a *count* | `smaps` `Referenced:` | none |

`page_idle` is `0600 root:root` and indexed by PFN, and `pagemap` zeroes the PFN
field for unprivileged readers, so even the index is unavailable. That is the
whole of what privilege changes.

## 3. Do this

### 3.0 Set up

```bash
git clone git@github.com:davidshim0/ltram-linux.git
cd ltram-linux && git checkout step6-refcount
./workloads/build_motivation.sh        # clones + builds; needs network
```

Builds GAPBS, redis, memcached + libevent, llama.cpp, YCSB-C, downloads two
GGUF models (~1.8 GB) and generates a 522 MB kron22 graph. Takes ~15 min. It is
idempotent — every step is cached and re-running is safe. **These artefacts are
gitignored on purpose**; do not commit them.

Confirm root actually helps here:

```bash
sudo test -r /sys/kernel/mm/page_idle/bitmap && echo OK
grep CONFIG_IDLE_PAGE_TRACKING /boot/config-$(uname -r)   # must be =y
```

If `CONFIG_IDLE_PAGE_TRACKING` is not set, root does not save you and the
kernel needs rebuilding. Stop and say so.

### 3.1 FIRST: validate the page_idle path — it has never executed

`rw_census.py` has a `HAVE_IDLE` branch (`idle_write` / `idle_read`, and the
PFN gathering in `scan()`). **Every line of it is untested**, because the
previous session never had root. Assume it is broken until you prove otherwise.

Validate against ground truth, not against itself:

```bash
sudo ./scripts/rw_census.py --label smoke --interval 5 --run 25 --warmup 15 \
  --cmd "$PWD/workloads/gapbs/pr -f $PWD/workloads/gapbs/benchmark/graphs/kron22.sg -n 100000 -i 20" \
  --out /tmp/smoke.csv
```

Expected, and these are real numbers from the previous session:

* `cold_method` column says `page_idle`, not `smaps_referenced`
* **`cold_pct` is populated at EVERY T**, not just the first row. If only the
  first row has a value, the `page_idle` path silently fell through.
* `write_cold_pct` ≈ **94.2%** at every T. pagerank writes a fixed 32 MiB score
  array and reads a 522 MiB graph, so this is flat by construction.
* `cold_pct` ≈ **0.3%**. pagerank streams the whole graph every iteration.

Independent cross-check of write-cold, which does not use the census at all:

```
clear_refs=4, sleep 4, count soft-dirty pages
  -> heap 8,203 of 142,830 pages dirty = 5.7% written = 94.3% write-cold
```

If the census disagrees with that by more than a few tenths, the census is
wrong, not the probe.

### 3.2 Switch the denominator to anonymous memory

This is a **known bug in the pilot results**, already diagnosed, not yet fixed.

`llama.cpp` reported 41% cold. `scripts/where_cold.py` attributed it:

```
 resident    cold  written  mapping
   636 MiB   77.0%     0.0%  tinyllama-1.1b-q4.gguf     <- mmap'd, r--s
   455 MiB    0.0%     0.0%  anon                       <- repacked weights
    88 MiB    0.0%    75.0%  anon                       <- KV cache
```

The 41% is the mmap'd GGUF: read once at load, then never again because
llama.cpp repacks the weights into a 455 MiB anonymous buffer. Those are
**clean file-backed pages the kernel drops for free**. They need no far memory,
no LtRAM, no tiering — and counting them inflates the denominator and both
columns.

Google's system compresses into **zswap**, which handles anonymous pages only,
so their movable set is anon regardless. Ours should match.

`rw_census.py` already takes `--anon-only`. Wire it through `run_census.sh` and
make it the **primary** measurement; keep all-pages as a secondary run. On the
anon denominator llama should be roughly **0% cold / 87.5% write-cold** — both
more honest and a larger gap than the 41 / 94 it currently shows.

### 3.3 Run the census

```bash
sudo ./scripts/run_census.sh --pilot          # 5 workloads x 10 min, ~55 min
sudo ./scripts/run_census.sh --full           # 5 x 60 min
```

Five workloads: `pagerank`, `bfs` (GAPBS on kron22), `llama` (TinyLlama 1.1B
Q4), `redis`, `memcached` (both 7M x 140 B, 95/5 zipfian at 10k ops/s). Pass
names to run a subset. Output and figures land in
`baselines/<date>_census/<mode>/`.

Prefer `--pilot` first. With root you no longer need `decay_census.py` — it
exists only to work around the missing per-page accessed bit, and its
from-t0-decay estimator is *not* the one Google used. Keep it for cross-checks;
do not make it primary.

### 3.4 Make the figures

```bash
./scripts/plot_census.py baselines/<date>_census/pilot/*.csv -o docs/figures
```

Produces `fig10` (panel per workload), `fig10b` (grouped by T, cold as a
*lighter* shade) and `fig10c` (same, cold *darker*). The user prefers **10c**.

Settled figure conventions — do not relitigate these, they were iterated on at
length:

* workloads in **descending write-cold at the smallest T** (computed, not
  hardcoded)
* Tableau colour-blind palette; cold is the same hue shifted in lightness; no
  outline on the cold bar (matplotlib strokes edges centred on the boundary, so
  an outlined inner bar renders wider than its parent)
* legend one row, in bar order, bare workload names
* bars flush within a group; group width 0.70
* x is `Time (min)` with bare numbers; y is `Percentage of Memory`
* title: `Share of Read-Mostly Data and Cold Data per Workload`
* shade key: `Top: Read-mostly data (Dirty = 0)` /
  `Bottom: Cold data (Accessed = 0)`
* cold labels in white, all labels horizontal, decimals only below 1
* **the daggers should disappear.** They mark T values where cold was not
  measured. With `page_idle` every T has a cold value. If daggers survive, the
  `page_idle` path is not working — go back to 3.1.

---

## 4. Traps. Every one of these cost the previous session real time

**A stray workload ruined an entire 55-minute run.** A `pr` from a smoke test
outlived its parent and sat at 2592% CPU across all 32 cores for 82 minutes.
`rw_census.py` `setsid`s its child, so a `timeout` killing the census left the
workload running. Now fixed (reaps on SIGTERM/SIGINT/SIGHUP and via `atexit`),
and `run_census.sh` refuses to start when a workload is already running. **Do
not disable that check.** Before any run: `pgrep -af 'gapbs|llama-cli|kv_load'`.

**Never run two measurements at once.** Contention changes coverage rates and
therefore the results.

**Wait after a bulk load.** memcached's LRU maintainer crawls every slab after
a load: 98.6% of pages dirtied in the 15 s after, 0.1% once settled. A census
started immediately measures the crawl. `run_census.sh` settles 150 s; keep it.

**Write-cold is monotonically non-increasing in T.** A page not written in 8
minutes was not written in 6. `plot_census.py` warns if it rises. The pilot
triggered it on bfs (96.46 -> 96.58 -> 96.65) because a windowed census
averages T = k·S over (windows − k + 1) boundaries — five samples at T=2m, one
at T=10m — so the mean is over *different sample sets*. Harmless at 0.19 pp but
do not let it grow, and never explain it as a property of the workload.

**Reads dirty pages in both cache engines.** Pure GET traffic, zero client
writes, 15 s: memcached 95.3%, redis 95.1% of resident pages dirtied. Idle is
0.1% / 0.0%. memcached bumps `it->time` and relinks the LRU on every GET; redis
writes `robj->lru` on every `lookupKey`, and both fields share a page with the
data. Reproduce with `./scripts/readpath_probe.sh`. **Always measure idle
before and after the traffic** — the bracket is what proved this is the lookup
path and not the post-load crawl.

**YCSB-C's redis binding hangs.** 2 keys in 45 s, on its own shipped
`workloadmini.spec`. Both KV servers are driven by `scripts/kv_load.py`
instead — one generator, identical keys/values/skew/read-ratio, so the two runs
differ by the server and nothing else. Do not go back to YCSB-C.

**`pgrep -f` matches your own shell.** Bitten twice on this project. Exclude
`$$`. Related: `pgrep -c` prints `0` *and* exits 1, so `|| echo 0` yields
`"0\n0"`.

**Don't truncate a pipe from a long run.** `... | head -3` closed the pipe and
SIGPIPE'd the census before it wrote its CSV; the stale CSV then looked like a
result.

**Contaminated results are quarantined, not deleted.**
`baselines/260901_census/contaminated-0201/` has a README saying why. Keep that
habit.

---

## 5. Tools

| file | what it does |
|---|---|
| `scripts/rw_census.py` | the census. per-page soft-dirty age; `page_idle` for cold under root, `smaps` fallback otherwise. `--anon-only` exists and should be used. |
| `scripts/run_census.sh` | drives all five workloads, then plots |
| `scripts/plot_census.py` | fig10 / 10b / 10c, plus the monotonicity check |
| `scripts/where_cold.py` | per-mapping attribution. **run this whenever a number surprises you** — it is what caught the GGUF |
| `scripts/readpath_probe.sh` | the read-path counterexample, with idle brackets |
| `scripts/kv_load.py` | redis + memcached load generator; YCSB zipfian, `--ops-per-sec` |
| `scripts/decay_census.py` | ladder estimator. **unprivileged workaround — not primary under root** |
| `workloads/build_motivation.sh` | builds everything, idempotent |

---

## 6. Pilot numbers to reproduce or refute

Unprivileged, ba8, T = 2 min, all-pages denominator. Cold is the `smaps` proxy
and is the number most likely to move under `page_idle`.

| workload | resident | write-cold | cold |
|---|---|---|---|
| bfs (kron22) | 544 MiB | 96.6% | 0.31% |
| pagerank (kron22) | 558 MiB | 94.2% | 0.31% |
| llama.cpp (tinyllama) | 1,221 MiB | 94.2% | 41.0% ← the GGUF, see 3.2 |
| redis (7M x 140 B, 10k ops/s) | 1,779 MiB | 44.5% | 0.14% |
| memcached (7M x 140 B, 10k ops/s) | 1,671 MiB | 23.0% | 0.01% |

**The open question worth your attention.** For redis and memcached the client
touches few pages at 10k ops/s over 7M keys, yet cold is ~0 while write-cold is
23–45%. Something is *reading* nearly every page without writing it — most
likely background maintenance (memcached's LRU crawler, redis's incremental
rehash and expiry sampling). If `page_idle` confirms that, it is a **point in
LtRAM's favour and not against**: maintenance threads keep pages looking hot to
an access-based policy while leaving them genuinely write-cold. It rests on the
`smaps` proxy today, which cannot attribute, which is exactly the weakness that
matters for this claim. **Confirm it with `page_idle` before anyone writes it
down.**

---

## 7. Done means

1. `page_idle` validated against the ground-truth probe (3.1)
2. census run under root with `--anon-only` primary, all-pages secondary
3. fig10c with **no daggers** and a cold value at every T
4. the redis/memcached background-read hypothesis confirmed or killed
5. CSVs in `baselines/<date>_census/`, figures in `docs/figures/`, committed

Report what the numbers are, including where they contradict the pilot. Several
of the pilot's most interesting results were later found to be measurement
artefacts; the useful habit on this project has been to attack your own number
before presenting it.
