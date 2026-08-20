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
| 6 policy, detection | **PASS — issue 10 fixed 2026-08-20** | no application hint: `rej_writable == rearmed == 50,179` against exactly 50,176 weight pages |
| 6 policy, end to end | **PASS 2026-08-20 (re-proven)** | digest identical on 15/15 runs after the cache-invalidate fix; no sector was reused in either run |
| 6 capacity (window full) | **NOT RUN** | earlier attempt contaminated by the leak; leak now fixed |
| 6 writeback (demotion) | **PASS 2026-08-20** | 7,517/8,192 on NOR, STALE 0 WRONG 0, `demoted` +7,519 -- one per resident page |
| 6 steady-state cost | **partial** | 4.87x measured once, but on the run whose sectors were never reused |
| 7 measurement | not started | unblocked |

## ISSUE 10 IS FIXED — 2026-08-20

The policy detects read-mostly pages, **with no application hint of any kind**. Evidence:
`baselines/260820_step6_detection/RESULT.md`. Kernel `#3`, tree `244a66761`, bitstream
`169_phy200`.

The scanner asked `pte_dirty(p)` — *has this page ever been written?* It now asks
`pte_write(p)` — *is it writable right now?* — which the scanner itself can change by
write-protecting and waiting for a fault.

**The 08-19 hypothesis was directionally right and wrong about the cause.** It blamed
`--protect-weights` for making `pte_write()` false so the re-arm never ran. In fact the
defect was **unconditional**: `ptep_set_wrprotect()` — the re-arm itself — *sets* bit 55
rather than clearing it, and nothing in LtRAM ever calls `pte_mkclean()`. This CPU has no
hardware DBM (HAFDBS = 0) and arm64 has no soft-dirty at all. So the old rule could only
ever promote a page **never written since its PTE was established**; the mprotect changed
nothing. The proposed fix ("clear the dirty bit for read-only mappings") would have fixed
only the mprotected case and left the transparent case just as broken.

What separated the guess from the truth was `a06b6973c`, one commit that did nothing but
count. **Instrument before theorising** — it cost under an hour and would have answered
this on the first run.

| commit | what |
|---|---|
| `a06b6973c` | count WHY a scanned page was not selected |
| `f558a316f` | a flash page may never be writable, and may never reach buddy |
| `244a66761` | ask whether a page was written, not whether it was ever written |

Measured over a 240 s attach, `matmul --n 7168 --iters 1000 --runs 1`:

| | 08-19 broken | A: `--protect-weights` | B: **no hint** |
|---|---|---|---|
| `rej_dirty_ro` → `rej_writable` | 13,247,550 | 41 | 50,179 |
| `rearmed` | 40 | 41 | **50,179** |
| `stale_dirty` | *(n/a)* | 108,999 | 108,450 |
| `sel_isolated` | 8,384 | 7,456 | **7,488** |
| DIGEST | `eac22204…` | `eac22204…` | `eac22204…` |

Phase B reaches the same `sel_isolated` as phase A, so **the mprotect hint bought nothing** —
it only skipped pass 1. The 379 promotions seen on 08-19 were the process's other anonymous
pages, which is why provenance showed 0% for both named regions.

**Two ways to misread these counters, both of which cost time here:**

- **A large `scanned` is the symptom of finding nothing.** `walk_page_range(mm, 0, TASK_SIZE)`
  restarts at address 0 every pass with no cursor, and the PTE loop is gated on
  `ctx->nr < promote_batch`, so a healthy pass short-circuits early. 13.2 M → 110,732 is the
  fix working.
- **`sel_isolated` measures `promote_batch`, not the predicate.** 32 pages/s, saturated on
  every pass in both phases; 26 minutes to drain 50,176 pages. Live-writable at
  `/sys/module/ltram_policy/parameters/promote_batch`; the default is set by the wear budget,
  so raise it for measurement only.

**Still unproven: nothing has reached flash.** The `nor_eci` backend was deliberately not
loaded, so `promoted 0` and `promote_failed == sel_isolated` are arithmetic, not symptoms.
`demoted 0` means the new demotion guard in `f558a316f` is **unexercised** — and a store to
the flash read window is silently discarded by the hardware, so if that guard is wrong the
failure mode is silent corruption or a hang. The next run loads the backend; attach a console
first (issue 12) and do not `rmmod` while a target pid is set.

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

## THREE BUGS FOUND ON 2026-08-20, ALL FIXED, ALL PREVIOUSLY LATENT

They were stacked: each one hid the next, so they could only be found in this order.

| commit | bug | what was hiding it |
|---|---|---|
| `e6f915089` | **Flash page leak.** `release_pages()`/`free_unref_page_list()` -- the batch path process exit and munmap take -- bypass the `__folio_put_small()` hook, so pages hit the `free_pages_prepare()` backstop, stayed out of buddy, and were then dropped. 16,502 lost in one evening. | Nothing. Found by its own dmesg flood, which then filled a root filesystem already at 14 MB free and made the box unreachable. |
| `53fc3a101` | **Page-cache folios promoted to flash.** No anonymity filter, and the scanner *preferred* file-backed text -- it is the most read-mostly memory a process has. Migration then took `__buffer_migrate_folio()`, which is in the SError panic trace. | Never looked for. `rej_not_anon` now shows ~850-1,100 refusals per run, so it was happening constantly. |
| `529197685` | **Stale CPU cache lines on sector reuse.** `rd_win` is `ioremap_cache`d and an FPGA-side erase/program does not invalidate it. The backend's verifiers always knew this (`inval_sector()` exists for it); the migration path never called it. A freshly promoted page could be read out of a line belonging to the sector's previous occupant. | **The leak.** While flash pages never returned to the bitmap, no sector was ever reused, so no line could be stale. |

**The third one is the reason to be careful about what "PASS" meant.** The first station-4
run matched its digest on all 15 runs -- and passed for the wrong reason. Fixing the leak
enabled sector reuse and the corruption appeared at run 12 of 15:

```
run1  8873ba56c40ff27a3bc077dd36c2fce7aedb72c7c06a30ec15cd22488dc23302
run12 8d57ab9cafc64a8109040cb6f50f3a87fc127177ea5fd0da67c5516acb24eba5
```

The re-run with all three fixed is clean: 15/15, `late_free 6,779` against `promoted 6,784`,
zero write or promotion failures. Evidence in `baselines/260820_step6_flash/`.

**The lesson worth carrying:** a fix that makes a system exercise a path it never exercised
before is not a regression when the next thing breaks. Both later bugs were older than the
fixes that exposed them.

**The backend module was rebuilt** for `529197685` and deployed as
`~/nor_eci/nor_eci_fulltest_ltram.ko`; the previous one is kept as `.pre-invalfix`. Rebuild it
whenever `ltram-backend/nor_eci_fulltest.c` changes:

```bash
mkdir -p /tmp/norbuild && cp ltram-backend/nor_eci_fulltest.c /tmp/norbuild/
echo 'obj-m := nor_eci_fulltest.o' > /tmp/norbuild/Makefile
ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- make -C linux O=$PWD/out-ltram M=/tmp/norbuild modules
```

## STATE AS OF 2026-08-20 09:00 — z08 NEEDS HANDS

**Station 4 passed.** 100% of the 16,384 weight pages went onto NOR, zero promotion failures
once the backend was live, digest re-checked between all 15 runs. Steady state **31.3 s vs a
6.432 s DRAM control = 4.87x**. Full evidence: `baselines/260820_step6_flash/RESULT.md`.

**Then a real bug surfaced and took the box's root filesystem with it.**

`free_pages_prepare()` reported 16,502 stray LtRAM pages — a quarter of the window. The
`f558a316f` guard hooks `__folio_put_small()`, which sees only single-page puts;
`release_pages()` and `free_unref_page_list()` — **the batch path process exit and munmap
take** — bypass it. Fixed in `e6f915089` by returning the page at the backstop itself, since
`free_pages_prepare()` is the one funnel every free path passes through.

The cascade: one `pr_info` per leaked page → 16,502 lines → journald wrote them into a root
filesystem already at **14 MB free** → `sshd` could no longer fork a session and started
closing connections before key exchange (`kex_exchange_identification: Connection closed by
remote host`).

**The kernel never died.** The `dmesg -w` stream started before the run held its connection
with 30 s keepalives long after new logins stopped. Streaming the kernel log off-box is the
only reason this was diagnosable — keep doing it.

### To recover, in this order

```bash
kinit                                     # on the gateway -- it has expired
# z08 will not accept ssh; power-cycle it via the BMC/gateway
# then, once it boots, BEFORE anything else:
ssh zuestoll08 'sudo journalctl --vacuum-size=32M; df -h /'
```

The kernel to deploy is already built and clean:
`260819_step4_backend-11-g8bab31e3a`, Image sha `5915794f1b7c9a9f0f2755bda8e07430b0325b63672b5981578fe4a44d3bfcd8`.

```bash
scp out-ltram/arch/arm64/boot/Image \
    hushim@enzian-gateway.inf.ethz.ch:/srv/tftp/userkernels/hushim/vmlinuz
```

Station 4's raw output is on `/scratch/hushim/station4-0820-0610/` (NFS — it survives the
reset). Rerun capacity and writeback on the fixed kernel; both are staged as
`~/station4.sh`, `~/station5.sh`, `~/ltram-writeback`.

### Two rules this cost a night to learn

- **Write run output to `/scratch`, never `$HOME`.** z08's root is 4.4 GB and cannot absorb
  even a modest log. `/scratch` is NFS with 507 GB and survives a board reset.
- **Never print per-page in a hot kernel path.** Count it. A rate-limited `pr_info` still
  emitted 16,502 lines and that was enough to end the filesystem.

## STILL OPEN

- **Step 6 end to end has never run** — detection is proven, the flash write path and the
  new demotion guard are not. This is the next run, and it wants a console.
- **`sel_isolate_fail` drops 18-22% of every selection** — `folio_isolate_lru()` refusing.
  Not new, not caused by the issue-10 fix, unexplained, and a fifth of the promotion budget.
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
| issue 10 — detection (**resolved**) | https://claude.ai/code/artifact/5a22f854-d1e9-41c8-b74f-58bedbf07a69 |
| issue 1 — linear map (resolved) | https://claude.ai/code/artifact/2b0ca5ef-ca05-4d59-a2ae-2f39d3af970c |
| issue 9 — folio refcount (resolved) | https://claude.ai/code/artifact/d4904e3e-1ba2-4104-b8a7-2da0214b48c4 |
| issue 11 — stale status word (resolved) | https://claude.ai/code/artifact/ceef8d43-ac65-42b9-81f8-196958ff75ae |
| issue 12 — hard reset (open) | https://claude.ai/code/artifact/f9dba960-2bbf-4d0c-b681-4196a357ab5d |
| baseline registry | `BASELINES.md` |
| per-gate evidence | `baselines/260819_*/RESULT.md`, `baselines/260820_step6_detection/RESULT.md` |
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
