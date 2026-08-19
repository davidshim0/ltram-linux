# Where this stands — 19 August 2026, end of the hardware bring-up session

Read this first. Written for someone with no memory of how any of it got here.

## The project in one paragraph

Micron MT35XU02GCBA octal-DDR NOR flash, reached over ECI through the Enzian FPGA,
presented to Linux as a NUMA node and a memory zone ("LtRAM"), with a kernel placement
policy that promotes read-mostly pages onto it. Cavium ThunderX arm64 plus Xilinx VU9P.
Sources and cross-builds on `enzian-ba8`; everything runs on `zuestoll08`.

## The one hardware fact everything follows from

The CPU reaches the flash through **three windows**, and only one is coherent:

| window | mapping | role |
|---|---|---|
| `rd_win` @ `RD_BASE 0x14000000000` | `ioremap_cache` | **reads** — coherent, cacheable |
| `io_win` @ `IO_BASE 0x900000000000` | `ioremap` (uncached) | **control** — `writeq(desc, io_win + dst)` |
| `er_win` @ `ER_BASE 0x1C000000000` | `ioremap` (uncached) | erase trigger |

**A store to the READ window is silently discarded.** It retires normally, the data goes
nowhere, and a later load returns the previous flash contents, which look plausible. The
device is **not** read-only — the CPU writes to it on every sector program, by storing a
descriptor to `io_win` after which the FPGA's DMA engine reads the source page out of DRAM
and programs it. What is impossible is `memcpy()` INTO the read window, which is exactly
what `folio_migrate_copy()` does, and the only reason `ltram_copy_to_flash()` exists.

## STATUS: gates 2, 3 and 4 PASS on hardware. Step 6 is blocked.

| gate | state | evidence |
|---|---|---|
| 2 node + zone | **PASS** | `spanned/present 65536`, **`managed 0`**, `stray_allocs 0`, `numactl --membind=1` refused |
| 2b window reachable from kernel | **PASS** | harness loads, 64 sectors erase+write+verify, `bad=0` |
| 3 boot self-test | **PASS** | `self-test completed without abort`; read the address stamp back correctly |
| 4 backend, refusal | **PASS** | no backend &rarr; `-ENODEV` (`-19`), `writes_failed 1`, `writes_ok 0` |
| 4 backend, positive | **PASS** | page programmed; FPGA `beats +64, pages +16, erases +1`; data confirmed by a later boot's self-test checksum changing `099708ea` &rarr; `00663ddc` |
| 5 migration hook | **mechanism proven, negative test NOT RUN** | 379 pages migrated with FPGA counters exact |
| 6 policy | **BLOCKED — see issue 10** | 5,184,674 scanned, **0 weights promoted** |
| 7 measurement | not started | blocked by 6 |

## THE BLOCKER — issue 10

The policy scans millions of pages and promotes **none of the weights**:

```
scanned         5184674      promoted 0      promote_failed 0
weights   50176 pages      0 LtRAM (0.0%)    50176 DRAM
result        7 pages      0 LtRAM (0.0%)        7 DRAM
```

Zero candidates were ever selected — `promote_failed` is 0 too, so nothing was attempted.

**Suspected cause, NOT yet confirmed.** `matmul --protect-weights` mprotects the weights
`PROT_READ`, so `pte_write()` is false, so the re-arm

```c
if (dirty && pte_write(p)) { ptep_set_wrprotect(...); flush_tlb_page(...); }
```

never runs, so the dirty bit set during the initial fill is **never cleared**.
`should_promote()` sees a dirty page every pass and the clean-run counter never advances.
A page that cannot be written without faulting is trivially clean; the policy treats it as
permanently dirty.

**Likely fix:** treat `!pte_write(p)` as clean in `should_promote()`, or clear the dirty
bit for read-only mappings instead of trying to write-protect what is already unwritable.

**FIRST ACTION: instrument `should_promote()` with a per-rejection-reason counter.** The
diagnosis above is a hypothesis. A counter would have answered it immediately, and its
absence is why it is still a hypothesis.

The 379 promotions seen earlier were the process's **other** anonymous pages — heap, stack,
libc — which is why provenance shows 0% for both named regions. The mechanism works end to
end; it is aimed at the wrong pages.

## What was fixed this session

**Issue 1 — the window had no kernel address (was blocking steps 3-5).**
`ltram_declare_node()` ran in `bootmem_init()`, after `paging_init()` built the linear map,
so the range was in `memblock.memory` (making `pfn_is_map_memory()` true) with no page-table
entry. `ioremap_cache()` returned an unmapped `__phys_to_virt()`, `ioremap()` returned NULL,
`memremap(MEMREMAP_WB)` followed `ioremap_cache()`. First touch = level-0 translation fault.

**Two causes, not one.** `should_skip_region()` backs BOTH `for_each_free_mem_range()`
(allocation) AND `for_each_mem_range()`, which `map_mem()` walks. The step-2 range filter
there kept allocations off flash *and* hid the range from the linear map. Moving the
declaration earlier alone would not have helped.

Fix: **`memblock_reserve()` the range** (excluded from allocation and from
`memblock_free_all()`, so `managed` stays 0, but still walked by plain memory iteration so
`map_mem()` maps it) and declare it at the end of `arm64_memblock_init()`, after
`high_memory` is computed. Runtime escape hatch: **`ltram=off`** on the kernel command line
skips the declaration entirely.

**Issue 9 — `mm/ltram_policy.c` leaked a folio reference per candidate.**
`folio_try_get()` then `folio_isolate_lru()` — which takes its own ref — and the promote
path `continue`d without dropping the first. Candidates reached `migrate_pages()` at +2
where `folio_migrate_mapping()` expects +1, so every one returned `-EAGAIN`. Symptom:
`promote_failed 448, promoted 0`. Fix is one `folio_put()`; `mm/madvise.c` is the pattern.

**Issue 3 — declaring a second node silently enabled automatic NUMA balancing.**
`num_online_nodes() > 1` plus Ubuntu's `CONFIG_NUMA_BALANCING_DEFAULT_ENABLED=y`. It uses
the SAME mechanism on the SAME PTEs as the policy and wants the opposite outcome: it sets
`PROT_NONE` to sample access while the scanner write-protects to observe writes, and it
migrates pages TOWARD node 0, undoing every promotion. Now `--disable
NUMA_BALANCING_DEFAULT_ENABLED` in `build.sh`, with a NEGATIVE assertion beside the
boot-critical symbols. `CONFIG_NUMA_BALANCING` stays `y` deliberately — stock NUMA
balancing is the obvious comparison policy, one sysctl away.

**Issue 6 — `rmmod` use-after-free.** `ft_exit()` called `kthread_stop()` on a worker that
may already have exited: `refcount_t: addition on 0`, then a NULL deref, so `rmmod`
segfaulted and the module could never be unloaded — a full reboot each time. Now guarded.

**Issue 5 — an old step tag carries old tooling.** Checking out a step tag rewinds
`scripts/` too, so step 3 was first built by a `build.sh` predating the NUMA fix and came
out with balancing defaulted ON, silently. Use **`./scripts/build-step.sh <tag>`**, which
checks the tag into a throwaway worktree and builds it with the CURRENT scripts, recording
both halves in `BUILDINFO`.

## New this session: the write backend

Nothing in the kernel implements `write_page()`, so the write path was untestable. The FPGA
harness now supplies one — `ltram-backend/nor_eci_fulltest.c`:

```bash
sudo insmod nor_eci_fulltest.ko provide_ops=1   # register the backend, start no worker
echo 1 > /sys/module/nor_eci_fulltest/parameters/fail_writes   # inject -EIO
```

It waits on the FPGA's **erase counter**, not a fixed sleep — a dropped trigger is invisible
to a sleep and has bitten this project before.

## STILL OPEN

- **Issue 10** (above) — the blocker.
- **Step 5's negative test has NEVER RUN.** Two attempts failed for setup reasons: wrong
  module loaded, then no candidates to migrate. This is the test that validates hook
  placement — the positive path passes whether or not the hook precedes
  `folio_migrate_mapping()`. Procedure: `fail_writes=1`, then assert the migration FAILS,
  the page is STILL IN DRAM, and its contents are STILL CORRECT.
- **Issue 12 — one unexplained hard reset** during a migration run. No oops, no panic text,
  log stops mid-stream. `pstore` has never worked here (`efi_pstore ... error (-5)`), so
  nothing is captured. Has not recurred. Leading explanation: `rmmod` was run while
  migrations were in flight, unregistering the backend mid-migration — self-inflicted, not
  proven. **Attach a console before the next long migration run.**
- **Failed migrations still cost an erase.** The hook writes flash before the mapping move,
  so a migration that then fails has already burned an erase+program. Correct for safety;
  real wear against the 41.5 erases/s budget.
- **A 6.72% variance run** was seen once (`matmul` usually reports 0.01-0.04%). Unexplained.
  Anything that noisy fails the workload's own gate for validating a regression.

## Gotchas that cost real time

- **The FPGA status word only prints at module load.** `dmesg | grep "STATUS WORD" | tail -1`
  returns a stale snapshot; `rmmod`/`insmod` to sample. Misreading this nearly produced a
  report of catastrophic corruption that was not happening (issue 11).
- **Status word `erases` and `pages` are 8-bit and wrap at 256** — compare modulo 256.
  Worked example for 379 writes: `beats 12352 + 379*64 = 36608`,
  `erases (193+379)&0xFF = 60`, `pages (16+379*16)&0xFF = 192`. All three matched.
- **`grep -A12` on `/proc/zoneinfo` cuts off before `managed`** (~45 lines of per-node stats
  intervene). Use `sed -n '/Node 1, zone *LtRAM/,/^Node /p'`. An `awk` anchored on
  `/zone +LtRAM/` matches **node 0's** empty LtRAM zone first and reports 0/0/0.
- **Never edit `scripts/*.sh` while a build runs** — bash reads a script by byte offset and
  resumes mid-token. **Never run two builds on one output directory.** After any raced or
  interrupted build, delete the output directory: a truncated `.o` links without complaint.
- **Check `BUILDINFO`, not the exit code.** A build can exit 0 with a stale identity block.
- **`kinit` on the gateway expires roughly hourly** and blocks every deploy.
- **z08 `/` is at 98%** — deploy the Image only, not the 145 MB module tree. Drivers are `=y`
  so boot does not need it.
- **NEVER `sudo reboot` zuestoll08** — iSCSI tears down before root unmounts, the journal
  aborts, systemd freezes, and the BMC cycle WIPES the FPGA. Use
  `sudo sh -c 'echo b > /proc/sysrq-trigger'`.
- **Do not `rmmod` the backend while a target pid is set.**

## The record

| what | where |
|---|---|
| implementation steps + **12 issue reports** | https://claude.ai/code/artifact/2a15f6cb-b540-41cb-bad9-2e403466d4c2 |
| **issue 10 — the blocker** | https://claude.ai/code/artifact/5a22f854-d1e9-41c8-b74f-58bedbf07a69 |
| issue 1 — linear map (resolved) | https://claude.ai/code/artifact/2b0ca5ef-ca05-4d59-a2ae-2f39d3af970c |
| issue 9 — folio refcount (resolved) | https://claude.ai/code/artifact/d4904e3e-1ba2-4104-b8a7-2da0214b48c4 |
| issue 11 — stale status word (resolved) | https://claude.ai/code/artifact/ceef8d43-ac65-42b9-81f8-196958ff75ae |
| issue 12 — hard reset (open) | https://claude.ai/code/artifact/f9dba960-2bbf-4d0c-b681-4196a357ab5d |
| baseline registry | `BASELINES.md` |
| per-gate evidence | `baselines/260819_*/RESULT.md` |
| FPGA campaign | `SNAPSHOT.md` in `~/VivadoProjects`, and github.com/davidshim0/ltram-fpga |

Artifact links 404 on the university account — open them with the personal account.

## State of the tree

- **`main`** — steps 2-6 with the linear-map fix; tags `260819_step2_solid`,
  `260819_step3_selftest`, `260819_step4_backend` are hardware-verified and archived.
- **`step6-refcount`** — `main` plus the folio-reference fix. **This is the current working
  kernel** and what is deployed.
- Deployed on z08: `6.8.0-ltram #1 ... 17:10:31`, Image `608ce04b...`.
- Gateway restore points: `vmlinuz.prev-step2`, `.prev-step3`, `.prev-step4`, `.prev-step6`.
- Bitstream **`169_phy200`**, confirmed by `st_wait` 738-739 us. Survived the hard reset.

## Commands

```bash
./scripts/build-step.sh <tag>          # build an older step with CURRENT tooling
./scripts/build.sh ltram|vanilla       # build the checked-out tree
./scripts/deploy.sh ltram              # kernel -> gateway, modules -> z08
sudo sh -c 'echo b > /proc/sysrq-trigger'    # reboot z08 -- never `sudo reboot`

# on the gateway
emg acquire -n ltram -g 2025-07-28 -k hushim/vmlinuz -i hushim/initrd.img zuestoll08

# on z08
sudo insmod ~/nor_eci/nor_eci_fulltest_ltram.ko provide_ops=1
bash ~/gate2.sh          # gate 2, all sub-checks
bash ~/gatefull.sh       # steps 5+6: provenance then the negative test
```
