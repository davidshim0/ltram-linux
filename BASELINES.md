# Kernel baselines — the index

Every verified kernel gets three things, named so each finds the others:

| thing | where | naming rule |
|---|---|---|
| **archive** | `baselines/<name>/` in this repository | `<name>` |
| **git tag** | this repository | `<name>` — the same string |
| **registry** | this file | one row per baseline |

**The name is `YYMMDD_stepN_what`.** Date first so tags and directories sort
chronologically; the step number only when the thing really is one step of an ordered
sequence. A one-off — a bisect point, a variant, a rebuild — drops the step and is just
`YYMMDD_what`, as `260818_v6.8-base` does.

**Everything is in one repository.** `linux/` is the kernel; the directories beside it are
what builds and evaluates it. One `git checkout <tag>` gives you the source, the script
that built it, the config it was built with, and what happened when it ran — atomically,
with no second step to forget.

```bash
git tag -l                       # every step and baseline
git log --oneline 260818_v6.8-base..    # one commit per step
git checkout 260818_step4_backend
./scripts/build.sh ltram         # linux/ with CONFIG_LTRAM on
./scripts/build.sh vanilla       # the same tree with it off -- the control
```

Steps, in order:

| tag | adds |
|---|---|
| `260818_v6.8-base` | pristine mainline v6.8 under `linux/`, nothing else |
| `260818_step0_vanilla68` · `260818_step1_matmul` | scripts, configs, workloads, docs. **No kernel change** |
| `260818_step2_zone` | node 1 and `ZONE_LTRAM` |
| `260818_step3_selftest` | boot-time read self-test |
| `260818_step4_backend` | the registerable flash write backend |
| `260818_step5_migrate` | migration routed through the driver, not memcpy |
| `260818_step6_policy` | policy vtable, page allocator, scanner, pid targeting |

**Upstream history is not carried.** `linux/` was imported as a tree, so `git blame` on
kernel code we did not write stops at `260818_v6.8-base`. Our own history is complete, one
commit per step. Keep a mainline clone for upstream archaeology.

---

## 260818_step0_vanilla68 — first custom kernel proven on hardware

**The control.** Unmodified mainline v6.8. Everything after this is measured against it.

### What this is, and what it is NOT

```
e8f897f4af  "Linux 6.8" (2024-03-10)  <-- THIS BASELINE (mainline v6.8 release)
   |
   +-- 14 commits --> 1f20ed07d1      <-- ltram branch TIP (x86-only, NOT this)
   |
   +-- separate lineage --> Ubuntu 6.8.0-64-generic  <-- the GOLDEN image (NOT this)
```

- **It is the BASE of this repository.** Every LtRAM step commit sits on it.
- **It is NOT the golden image kernel.** Golden is Ubuntu `6.8.0-64-generic`: mainline 6.8
  plus Ubuntu's patch set and years of stable backports. Same version *number*,
  meaningfully different *code*. We inherited golden's **config**, not its **code**.
- **It is NOT the ltram branch tip** — 14 commits below it, deliberately.

**Consequence to remember:** if something works under golden and breaks here, "Ubuntu's
patches" is a live explanation alongside "our config". Do not assume a divergence is ours.

| | |
|---|---|
| kernel release | `6.8.0-vanilla68` |
| archive | `baselines/260818_step0_vanilla68/` (241 MB) |
| git tag | `260818_step0_vanilla68` → commit `e8f897f4af` (= v6.8) |
| source changes | **none** — risk lives in the config and toolchain, both archived |
| toolchain | `aarch64-linux-gnu-gcc-11` (golden Ubuntu kernel used gcc-13) |
| **paired bitstream** | **`169_phy200`** — trail +0.433 |

**Verified on hardware 2026-08-18:** booted with iSCSI root · `uname -r` = `6.8.0-vanilla68` ·
FPGA harness `test=32` **PASS** (erase ok, write ok, 0 bad) · status word
`bytes=4096 beats=64 pages=16 erases=1` · **`st_wait 736 us`** · MMIO integrity clean.

**Validated at scale 2026-08-18, same kernel, same bitstream:**

| test | result | `st_wait` |
|---|---|---|
| `test=42` census, 7,012,352 words | **0 bad, 0 ppm** — per-bit none, per-region none, all patterns clean | 743 us |
| `test=33` sectors 49152-57344 | **PASS** — fill 0, soak 0, final sweep 0 | 741 us |

The 49152 region is the discriminating one: it is where `179/t51` produced 11 failures and
`169_phy200` produced none. This kernel reproduces 169's behaviour exactly, so the kernel
is not perturbing the flash path.

### The fingerprint that tells you which bitstream is on the chip

`st_wait` ≈ **736 us** → 169_phy200. ≈ **1329 us** → 179/t51. A kernel result is
meaningless without knowing which bitstream produced it; this is how to tell after
the fact from a log.

**Whole-device soak, 2026-08-18:** `test=33` over all 65,536 sectors — 125,465 writes,
240,071 reads, **fill 0 / soak 0 / final sweep 0 / ERASE-INCOMPLETE 0**. Covers both regions
that have ever produced faults (16384-20479 and 49152+). This is the strongest validation
the baseline has.

---

## 260818_step1_matmul — the measurement control

Userspace only; no kernel changes. The denominator for every later number.

| | |
|---|---|
| archive | `baselines/260818_step1_matmul/` (52 KB) |
| git tag | `260818_step1_matmul` (this repo) |
| source | `workloads/matmul/` |
| ran under | kernel `6.8.0-vanilla68`, bitstream `169_phy200` |

```
matmul --n 7168 --iters 20 --runs 10 --verify --protect-weights
  weights 196.0 MiB @ 0xf81488a00000    result 28.0 KiB @ 0xf8149509c000
  mean 6.836 s   sd 0.001 s  (0.01%)
  DIGEST 41bd154efbce9bd07461680229516268bd481e8bbdb3d187cd8937ca7ae93a92
```

**The digest is the correctness gate for steps 5-7.** Every later run must reproduce it
exactly; a faster run with a different digest is corruption, not a result.

Variance 0.01% means a 1% regression (68 ms) is ~68x the noise. Weights at 196 MiB fit the
256 MiB window with headroom, so the whole region can be promoted without the policy having
to choose a subset. `--protect-weights` proved the read-only claim rather than assuming it.

---

## The LtRAM steps — all BUILT, none BOOTED

Branch `ltram-arm64`, all on mainline v6.8 (`e8f897f4af`) — the same commit as the vanilla
baseline, so they share an ancestor and the comparison is honest.

| step | tag | archive | what it adds |
|---|---|---|---|
| 2 | `260818_step2_zone` | `baselines/260818_step2_zone/` (Image + initrd + modules) | node 1 + ZONE_LTRAM; allocator excluded by zonelist; boot allocations, memmap and pgdat forced to node 0 |
| 3 | `260818_step3_selftest` | `baselines/260818_step3_selftest/` | boot-time read self-test |
| 4 | `260818_step4_backend` | `baselines/260818_step4_backend/` | registerable flash write backend, debugfs `write_test` |
| 5 | `260818_step5_migrate` | *(source only — code is in step 6's image)* | migration routed through the driver, hooked before the mapping moves |
| 6 | `260818_step6_policy` | `baselines/260818_step6_policy/` **+ step 7 tooling** | policy vtable, private page allocator, scanner, pid targeting |

**Deploy step 6's image** — it contains everything. Its archive holds the Image, modules,
`ltram-inspect` and `run-experiment.sh`; the initrd is unchanged since step 2, so use
`baselines/260818_step2_zone/initrd.img`.

### CONFIG_LTRAM=n is inert, and that is the point

Zero ltram symbols, differing from vanilla only in four initcall names whose encoded source
line numbers moved. One config symbol separates the two kernels, not two trees — which is
what makes an A/B measurement mean anything.

### Three traps this work hit, all silent by nature

- **Boot allocations land on flash by default.** `NUMA_NO_NODE` + memblock's top-down
  search + the window being highest-PFN = per-CPU areas, swiotlb, the qspinlock hash and
  CPU-entry page tables all on NOR, every store discarded. Also the memmap (`sparse.c`)
  and the node's own `pg_data_t` (`arch_numa.c`). All three forced to node 0.
- **A new zone overflows `GFP_ZONE_TABLE`.** 2^4 x 3 = 48 fits in 64 bits, 2^5 x 3 = 96
  does not. The compiler says only "shift count >= width" from inside a macro. ZONE_LTRAM
  is excluded from the table exactly as ZONE_DEVICE is.
- **`IS_ENABLED()` still compiles its branch**, so naming `ZONE_LTRAM` inside one fails to
  build with the option off. Preprocessor guards wherever the enum value appears.

### Two bugs caught in review before they could mislead

- `migrate_pages()`'s last argument is `ret_succeeded` — the number that **moved**. Read as
  failures, the counter reports success while nothing moves.
- The dirty re-arm cleared the **young** bit, which observes access rather than writes; a
  read-heavy workload would look busy and never be promoted. Correct mechanism is to
  write-protect and let the next write fault, because this CPU has **no hardware DBM**.

---

## Conventions worth not rediscovering

- **Archive the resolved `config`, not just the tag.** `build.sh` transforms Ubuntu's
  golden config via `olddefconfig` + enable/disable calls, and those depend on Kconfig
  defaults that move between versions. Re-running the script later may not reproduce the
  same config. The archived `config` is the artifact that does.
- **Cross-compile modules on ba8, never build on z08.** z08's root is 4.4 GB and has hit
  100%; `/lib/modules/<rel>/build` there is a dangling symlink to a ba8 path.
  A `.ko` only loads on its own kernel (`vermagic`), so rebuild per baseline.
- **Drivers are `=y` not `=m`** in these builds (ThunderX NIC, iSCSI). The initrd
  therefore contains no `nicvf`/`iscsi_tcp` and that is correct, not a defect.
- **`emg release` does not delete a named volume.** `hushim-ltram` is persistent, so
  `~/nor_eci`, `/lib/modules/*` and ssh keys survive release/acquire.
- **Gateway ssh dying is Kerberos, not the key.** `/home/hushim` on the gateway is on
  Kerberized storage; when the ticket expires sshd cannot read `authorized_keys` and it
  looks exactly like the key was removed. Fix is `kinit` on the gateway, not re-pasting.

## Where new work goes

Branch **`ltram-arm64`**, cut from `v6.8`. The old LtRAM commits (`1f20ed07d1`) are
**x86-only** — `arch/x86/mm/{init,numa}.c`, nothing under `arch/arm64`. Generic parts
(`mm/ltram.c`, gfp plumbing, the zone) are reusable; the arch hooks are not.
First increment: `ltram_declare_node()` in `arch/arm64/mm/init.c`.
