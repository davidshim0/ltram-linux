# Where this stands — 18 August 2026

Read this first. It is written for someone with no memory of how any of it got here.

## The project in one paragraph

Micron MT35XU02GCBA octal-DDR NOR flash, reached over ECI through the Enzian FPGA,
presented to Linux as a NUMA node and a memory zone ("LtRAM"), with a kernel placement
policy that promotes read-mostly pages onto it. Cavium ThunderX arm64 plus Xilinx VU9P.
Sources and cross-builds live on `enzian-ba8`; everything runs on `zuestoll08`.

## The one hardware fact everything follows from

**The processor can LOAD from the flash window coherently. It can never STORE to it.**
A store retires normally and the data is silently discarded; a later load returns the
previous flash contents, which look entirely plausible. Data reaches the array only
through the FPGA's DMA ring.

Every mechanism in this repository exists to manufacture a signal the hardware does not
give. That is why so much of the design is in the refusals: `-ENODEV` when no backend is
registered, a failed migration that leaves the page in DRAM, a digest that gates a timing
number, a deploy that re-hashes the kernel on the gateway.

## Status

**The FPGA side is settled. Do not reopen it.** `169_phy200` is golden: whole-device soak
over all 65,536 sectors, 125,465 writes, 240,071 reads, fill 0 / soak 0 / final sweep 0 /
erase-incomplete 0. A `test=42` census of 7,012,352 words returned 0 bad, 0 ppm. The
full FPGA record is `SNAPSHOT.md` in `VivadoProjects` on ba8 — what is golden, the two
fault modes, and what was tried in each of ~250 numbered folders.

**The Linux side is built and completely untested on hardware.** All seven steps compile,
are tagged, and are archived. Steps 0 and 1 have hardware results. Steps 2 through 6 have
never run on a board.

| gate | step | what must be true | state |
|---|---|---|---|
| boots at all with the new zone | 2 | `uname -r` = `6.8.0-ltram`, iSCSI root mounts | **open** |
| node 1 declared, allocator excludes it | 2 | `/sys/devices/system/node/node1` exists; ZONE_LTRAM `nr_free_pages` never drops | **open** |
| boot leak under 3 pages | 2 | stray-alloc counter in dmesg | **open** |
| window readable from kernel context | 3 | `self-test completed without abort` appears | **open** |
| one page programmed through the driver | 4 | `writes_ok` increments; status word `beats +64 pages +16 erases +1` | **open** |
| migration carries the data | 5 | positive test AND the `-EIO` negative test | **open** |
| policy promotes weights, never the result buffer | 6 | `ltram-inspect`: weights high %, result **0** | **open** |
| end-to-end off/on with the digest intact | 7 | both runs reproduce `41bd154e...` | **open** |

Deploy **step 6's image** — it contains steps 2 through 6 plus step 7's tooling.

## The repository

One repository, `github.com/davidshim0/ltram-linux`, cloned on ba8 at
`/local/home/hushim/ltram-linux`.

```
linux/          the kernel: mainline v6.8 plus one commit per step
scripts/        build.sh  deploy.sh  push-initrd.sh  assert_kconfig.sh
configs/        golden-6.8.0-64.config, the seed for every build
workloads/      matmul, ltram-inspect, YCSB-C, rwstress, basic_tests, pagerank,
                monitoring, profiles
docs/           design decisions, kernel architecture, migration mechanism
baselines/      one directory per verified build, indexed by BASELINES.md
reference/      not on the build path: the earlier out-of-tree module, the FPGA
                harness source, the buildroot/VM config
```

Names are `YYMMDD_stepN_what` — date first so tags and archives sort chronologically,
step number only when the thing really is one step of an ordered sequence. Archive
directory and git tag always share the name.

```
260818_v6.8-base        pristine mainline v6.8 under linux/   (no step: it isn't one)
260818_step0_vanilla68  the buildable control
260818_step1_matmul     the measurement control
260818_step2_zone       node 1 and ZONE_LTRAM
260818_step3_selftest   boot-time read self-test
260818_step4_backend    the registerable flash write backend
260818_step5_migrate    migration routed through the driver, not memcpy
260818_step6_policy     policy vtable, page allocator, scanner, pid targeting
```

`git checkout <tag>` gives the source, the script that built it, the config it was built
with, and what happened when it ran — atomically. That was the point of collapsing three
repositories into one.

**Upstream history is not carried.** `linux/` was imported as a tree, so `git blame` on
kernel code we did not write stops at `260818_v6.8-base`. Our own history is complete.

## Key decisions, so they are not re-litigated

- **NUMA node AND zone.** The node is what userspace and the migration machinery address;
  the zone keeps the allocator from ever handing out a flash page by accident.
- **`ZONE_LTRAM` is in no zonelist**, and its pages are never released to buddy
  (`managed` stays 0). That is *how* residency is enforced — and it forces
  `mm/ltram_policy.c` to own the pages through a private bitmap allocator, because the
  buddy allocator cannot hand out a flash page even when asked.
- **`ZONE_LTRAM` is excluded from `GFP_ZONE_TABLE`**, exactly as `ZONE_DEVICE` is.
  2^4 x 3 = 48 fits in 64 bits; 2^5 x 3 = 96 does not. The compiler only says
  "shift count >= width" from inside a macro.
- **The migration hook goes BEFORE `folio_migrate_mapping()`.** There the source is
  already unmapped so its contents are stable, and a flash error returns with nothing
  published — the page stays in DRAM, contents intact. Hooked after, there is no way to
  fail safely.
- **Write-protect-and-fault, not clear-young.** This CPU has no hardware dirty-bit
  management (`ID_AA64MMFR1_EL1` HAFDBS = 0, measured). Clearing young observes *access*;
  the policy is about *writes*.
- **A vtable, not BPF `struct_ops`, for now.** The policy call is ~30 ns against a 16.4 ms
  erase — 1 : 550,000 — so dispatch cost is irrelevant. The reason is debuggability in a
  project whose defining hazard is silent failure. The interface is already the shape
  `struct_ops` needs. Switch at three or more policies worth comparing.
- **sysfs `target_pid`, not `prctl`.** Attaching to a *running* process is the point: the
  workload reaches steady state first, and before/after happens in one process lifetime.
- **Large folios and pinned pages are ineligible.** A 2 MiB folio is 512 flash sectors, so
  one write anywhere in it demotes all 512 — 256x the wear of a wrong guess on a base page.
- **Read-back verify must evict with a PHYSICAL address (`CVMCACHE`).** `dc civac` is a
  no-op here: the LLC is the point of coherence, so a verify without eviction re-reads the
  line it is meant to check and always passes.
- **Archive the resolved `.config`, not just the tag.** `build.sh` transforms the golden
  config through `olddefconfig`; Kconfig defaults move between versions.

## Settled, with the evidence

- **Toolchain.** `aarch64-linux-gnu-gcc-11` built the vanilla control that booted z08 and
  passed every hardware test. gcc-13 is not needed and is not packaged for Ubuntu 22.04.
- **tftp.** `/srv/tftp/userkernels/hushim/` is owned by `hushim` and writable.
  `deploy.sh` scp's straight in. No admin in the loop.
- **Trail is a WINDOW, not a floor.** +0.209 gives sparse b16 faults, **+0.433 is clean**,
  +0.650 is gross corruption. Three bitstreams from one race of 179 prove the bracket.

## Live constraints

- **z08 `/` is 4.4 GB and sits at 97%, 146 MB free.** `modules_install` plus an initramfs
  does not fit. `deploy.sh` prunes old module trees. Never build on z08.
- **Initramfs cannot be built on ba8.** It has to be generated on z08 with
  `initramfs-tools` after modules are installed, because only z08 has the iSCSI userspace.
  `Image` alone is not a bootable deliverable.
- **Mainline vs Ubuntu.** The golden kernel is Ubuntu's `6.8.0-64-generic`, patched;
  `linux/` is mainline v6.8. If something works under golden and breaks here, "Ubuntu's
  patches" is a live explanation alongside "our config".
- **NEVER `sudo reboot` zuestoll08.** iSCSI tears down before root unmounts, the journal
  aborts, systemd freezes, and the BMC cycle WIPES the FPGA. Use
  `echo b > /proc/sysrq-trigger`.
- **`emg release` does not delete a named volume.** `ltram` is persistent, so `~/nor_eci`,
  `/lib/modules/*` and ssh keys survive.
- **Gateway ssh dying is Kerberos, not the key.** `kinit` on the gateway.

## Current deployment state

z08 is running `6.8.0-vanilla68`. `/lib/modules/6.8.0-ltram` and
`/boot/initrd.img-6.8.0-ltram` are staged there, **but the gateway is still serving the
vanilla vmlinuz** — the LtRAM kernel has never been put in front of the boot loader.

## Open questions

- Every hardware gate above.
- **Read-side capture fault, unresolved.** Single bit, DQ5/DQ6, first beat of a 128 B
  line, flash content provably correct, roughly 1 in 48,000 reads. An FPGA problem: the
  RWDS input path has no timing constraint in any bitstream. Sets a noise floor under
  every measurement. Known, not blocking.
- **The step-7 delta mixes three effects** — reads hitting flash, a different NUMA node,
  and migration cost — and no control separates them on this hardware (a DRAM-backed
  node 1 does not exist). Report it as combined and say so.
- **Migration hook coverage is narrower than it looks.** `migrate_folio_extra()` is one
  entry point; `buffer_migrate_folio()` and the huge-page paths do not pass through it.
- The address-slice fix from 179/180 is **still latent in 169** and unapplied.
- `tb_read_write_scheduler` has 18 pre-existing failures.

## Commands

```bash
# build -- ends with an identity block: tag, commit, date, toolchain, Image sha256
./scripts/build.sh ltram
./scripts/build.sh vanilla
./scripts/assert_kconfig.sh out-ltram/.config

# deploy -- prints that block, then re-hashes the kernel ON THE GATEWAY and aborts on
# a mismatch, so "is the kernel about to boot the one I built?" has an answer
./scripts/deploy.sh ltram
./scripts/push-initrd.sh 6.8.0-ltram

# then, ON THE GATEWAY
emg acquire -n ltram -g 2025-07-28 -k hushim/vmlinuz -i hushim/initrd.img zuestoll08
```

Confirm which bitstream is on the chip before trusting any result: `st_wait` around
736-743 us means `169_phy200`; around 1329 us means 179/t51.

## Where else the record lives

| what | where |
|---|---|
| step-by-step implementation pages | https://claude.ai/code/artifact/2a15f6cb-b540-41cb-bad9-2e403466d4c2 |
| the build plan with test methodology | https://claude.ai/code/artifact/41ad587e-5f89-4cbc-8b09-475f46f24527 |
| the FPGA campaign record | `SNAPSHOT.md` in `VivadoProjects` on ba8 |
| design decisions | `docs/DESIGN-DECISIONS.md` |
| baseline registry | `BASELINES.md` |

Artifact links 404 on the university account — open them with the personal account.

## Still on disk, deliberately

`ltram-kernel` (54 GB), `ltram-new` (28 GB) and `ltram-policy-bench` (2.6 GB) are the
superseded directories. Everything unique in them has been copied here and verified
against checksums. They are kept until the first six steps are finished on hardware, then
they can go.
