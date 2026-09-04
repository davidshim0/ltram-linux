# Queued for the next kernel build

Written 2026-09-03, revised the same day. Two unrelated changes are waiting,
both of which need a rebuild and a reboot. `ltram.o` and `ltram_policy.o` are
built into the kernel (`mm/Makefile:137`), not modules, so neither can be loaded
without a boot — and the reboot is the user's to perform.

## One change per build. Do NOT batch them.

An earlier version of this note recommended putting both in one build to save a
reboot. That was wrong, and the reasoning is worth keeping:

* **The census is the instrument.** It should run on a kernel differing from the
  validated one by the least possible amount. Both changes land in the same file
  and the *same loop* — (A) gates the `should_promote` branch, (B) rewrites the
  `!folio_test_anon` refusal a few lines above it. Default-off makes them inert,
  not absent, and this project's own history is the argument against trusting
  that difference: three stacked bugs on 2026-08-20, each hiding the next, and
  the lesson recorded as *"a fix that makes a system exercise a path it never
  exercised before is not a regression when the next thing breaks."*
* **(B) is not designed yet.** (A) is ten lines with a known verification. (B)
  has an open problem in it (kernel writes through the direct map). Batching a
  ready change with an unready one does not save a cycle; it makes the ready one
  wait.
* **(B) fails silently if wrong.** Reordering `filemap_migrate_folio()` so the
  copy precedes the mapping move is exactly the kind of bug that publishes a
  moved mapping over unwritten data. That does not belong in the kernel that
  produces the census, however well gated.

A reboot is cheap. A confounded census is not.

---

## A. `scan_only` — scanning with no promotion

**Why.** The census wants per-page write-cold counts from the real scanner
without the placement system doing anything. Every attempt to get that by
bending the watermarks has distorted the thing being measured, and the standing
instruction is not to tamper with the scan + promotion system we already have.
A parameter that is off by default leaves that system byte-for-byte unchanged.

**The change, ~10 lines in `linux/mm/ltram_policy.c`:**

```c
static bool scan_only;                    /* default 0 = today's behaviour */
module_param(scan_only, bool, 0644);

/* in the scan thread loop */
if (!scan_only && !READ_ONCE(lt_clean_count)) { msleep(...); continue; }

/* in the walk */
if (!scan_only && policy->should_promote(folio, written)) { ... }
```

**Why those two lines specifically.**

* The clean-pool guard makes the thread refuse to walk when the pool is empty,
  so a drained pool gives zero passes. `scan_only` must skip it.
* `ctx->nr` increments on every page *chosen*, so `promote_batch` bounds the
  walk, not just the batch. Skipping the `should_promote` block leaves the walk
  bounded only by `scan_ptes_per_pass`, which is what a census wants.
* A clean page falls through to the re-arm with `written == false` and is not
  re-armed, which is correct — it is already write-protected.

**Verify after boot:** `scan_only=0` must reproduce current counters exactly.
Then with `scan_only=1`: `ptes_examined` rises to `scan_ptes_per_pass` per pass,
`chosen` stays 0, `moved_to_ltram` stays 0, and passes continue on an empty pool.

---

## B. Clean read-only file mappings as promotion candidates

**Why.** 636 MiB of llama.cpp's 1,221 MiB RSS is a read-only file mapping — the
most read-mostly memory in the whole census and categorically ineligible today.
On arm64 it is worse: no AVX512 means no repack, so no anonymous copy of the
model exists at all.

Established 2026-09-03 by reading the source. Most of what was previously
assumed to block this does not:

| previously believed | actually |
|---|---|
| Reclaim needs new free funnels | **No.** `free_pages_prepare()` keys on `pfn_is_ltram()` — physical address, not page type. `__remove_mapping()` → `folio_put()` reaches the same backstop as anon. |
| We must walk `i_mmap` ourselves | **No.** `migrate_pages()` already handles file folios; `try_to_migrate()` → `rmap_walk_file()` takes `i_mmap_rwsem` itself. |
| DMA is a boundary of the mechanism | **No.** For anon, `folio_maybe_dma_pinned()` (`_pincount` / `GUP_PIN_COUNTING_BIAS`, i.e. `FOLL_PIN`) is sufficient. It is a page-cache cost, not a general hole. |

### What actually blocks it

1. **`filemap_migrate_folio()` bypasses the flash hook.**
   `linux/mm/migrate.c:876` calls `folio_migrate_mapping()` **first**, then
   `folio_migrate_copy()` — a plain memcpy into the NOR window. The mapping
   moves, the data does not, nothing reports it. This is the exact failure
   `migrate_folio_extra()` was written to avoid, and worse: the ordering is
   inverted, so there is no safe failure point. The copy must be hoisted ahead
   of the mapping swap. **Not a copy-paste of the existing hook.**

2. **The kernel writes page cache without a PTE.** *(the open problem)*
   Fault-back is `do_wp_page()` on a write-protected **user** PTE. The kernel
   reaches page cache through the direct map — `folio_address()`,
   `kmap_local_folio()` — always writable, never faults. A `write()` syscall, a
   readahead fill, truncate zeroing: each is a plain CPU store that retires
   normally and is **silently discarded**, with nothing to trap it.

3. **`MAP_SHARED` writers do not COW.** `do_wp_page` on a `VM_SHARED` file
   mapping takes `wp_page_shared`, which makes the PTE writable in place —
   precisely the failure `linux/mm/memory.c:3505` warns about. Fault-back exists
   only for private mappings.

4. **Cross-process inheritance.** A promoted libc page taxes every process on
   the box at ~1 µs instead of ~90 ns, and `target_pid=0` cannot hand it back.
   Policy question, not a mechanism one — but it needs an answer in the write-up.

### Scope for the first attempt

About a day for (1) and (3); (2) is the research project.

* Hook `filemap_migrate_folio()`, copy reordered before the mapping move.
* Refuse `buffer_migrate_folio()` outright.
* Selection predicate, replacing the bare `!folio_test_anon()` refusal:
  `folio_test_uptodate && !folio_test_dirty && !folio_test_writeback &&
  !folio_test_private && !mapping_writably_mapped(folio->mapping) &&
  !(vma->vm_flags & VM_SHARED)`.
* Keep it behind its own default-off parameter, same as (A), so the shipped
  behaviour is untouched.

**The narrow subset is sound today:** a file on a read-only mount with no
writers is never kernel-written, so (2) cannot fire. That covers llama's model
and program text — which is the whole point. The general case needs a
`folio_is_ltram()` demote-check at every kernel page-cache write site: bounded,
but scattered.

---

## Building

```bash
./scripts/build.sh ltram          # cross-build arm64 from ba8
```

Incremental — one `.c` file is a recompile plus a relink of `Image`, minutes on
32 cores, not a from-scratch build. **Do not build while a measurement is
running on ba8**; a 32-core build landing inside a census window is exactly the
contention that invalidated the first one.

Toolchain mismatch is known and documented in `scripts/build.sh:18` — the golden
kernel used `aarch64-linux-gnu-gcc-13`, ba8 has gcc-11 only.

Then the user deploys and reboots (`sudo ltram-reboot`, never `sudo reboot`).

---

## Sequencing

Both are blocked behind the running work, not behind each other:

1. `gapbs_sweep.sh` — running on z08 as of 2026-09-03, ~3.3 h.
2. `kv_sweep.sh` — running on ba8, ~5.6 h. **Build only after this finishes.**
3. **Build (A) alone.** One reboot. Run the census.
4. Only once the census is finished and its numbers are trusted, design and
   build (B) separately.
