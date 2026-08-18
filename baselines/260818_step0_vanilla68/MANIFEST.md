# Baseline: 6.8.0-vanilla68 — first custom kernel proven on zuestoll08

**2026-08-18.** Mainline Linux v6.8, cross-built on ba8, network-booted on zuestoll08
with an iSCSI root, and confirmed driving the NOR flash through the FPGA.

This is the control. Everything after it is measured against this.

**It is the BASE of the ltram-policy-bench linux branch** (every LtRAM commit sits on
`e8f897f4af`), **not** the branch tip, and **not** the golden image kernel. Golden is
Ubuntu `6.8.0-64-generic` — mainline 6.8 plus Ubuntu's patches and stable backports. We
took golden's **config**, not its **code**. If behaviour diverges from golden, Ubuntu's
patch set is a candidate cause, not only our configuration.

## What was verified, on hardware

| check | result |
|---|---|
| `uname -r` | `6.8.0-vanilla68` — our build, not the golden image |
| root filesystem | mounted, ext4 on the iSCSI device |
| network | `cpu40g0` / `cpu40g1` up with addresses |
| FPGA harness `test=32` | **PASS** — erase ok, write ok, 0 bad |
| status word after 1 sector | `bytes=4096 beats=64 pages=16 erases=1` (traffic reached the FPGA) |
| `st_wait` | **736 us** — matches 169_phy200's signature (t51 measures ~1329) |
| MMIO integrity | CLEAN over 2000 samples |

Paired with FPGA bitstream **169_phy200** (trail +0.433). The kernel result is only
meaningful against a known bitstream: see `169_phy200/VERSION.md`.

## Provenance — how to find this from git, and git from here

| | |
|---|---|
| **archive** | `ba8:/local/home/hushim/ltram-kernel/baselines/vanilla68-2026-08-18/` (this directory) |
| **git tag** | `baseline/vanilla68-2026-08-18` — same name, `baseline/` + this directory's name |
| **repo** | `ba8:/local/home/hushim/ltram-policy-bench/linux` (a git submodule with its own `.git`) |
| **commit** | `e8f897f4af` = mainline **v6.8** release |
| **work branch** | `ltram-arm64`, cut from `v6.8` |
| **registry** | `ba8:/local/home/hushim/ltram-kernel/BASELINES.md` lists every baseline |

```bash
cd /local/home/hushim/ltram-policy-bench/linux
git tag -n20 -l baseline/vanilla68-2026-08-18    # the tag names this archive
git checkout baseline/vanilla68-2026-08-18
```

- source: mainline **v6.8**, commit `e8f897f4af`, tagged `baseline/vanilla68-2026-08-18`
- **no source changes** — this is unmodified upstream. The reproducibility risk is
  entirely in the config and toolchain, which is why both are archived here.
- toolchain: see `toolchain.txt` (aarch64-linux-gnu-gcc-11 — note the golden Ubuntu
  kernel used gcc-13; a different major version is a real variable if results diverge)
- `build.sh.asbuilt` is the exact script that produced this

## Why `config` is the important file here

`build.sh` starts from the golden Ubuntu config and transforms it: `olddefconfig` onto
mainline, then a series of enable/disable calls. Those transformations depend on Kconfig
defaults, which move between kernel versions. **Re-running the script on a different tree
will not necessarily reproduce this config.** The resolved `config` in this directory is
the artifact that does.

Notable deviations from the golden config, all deliberate:
- integrity/IMA/keyring stack **off** — ba8 has no openssl headers and no sudo to install
  them; none of it is needed to boot an unsigned kernel over tftp
- ThunderX NIC and iSCSI built **`=y`** rather than `=m`. Better for booting: the drivers
  exist before the initrd is unpacked, so there is no chicken-and-egg reaching the iSCSI
  root. This is why the initrd contains no `nicvf`/`iscsi_tcp` modules and that is correct.
- `LOCALVERSION_AUTO=n` plus an annotated `v6.8` tag, so `setlocalversion` stops appending
  `+`. A stray `+` lands in `/lib/modules/<rel>` and the initramfs name and causes
  module-path mismatches at boot.

## Restoring this exactly

```bash
B=/local/home/hushim/ltram-kernel/baselines/vanilla68-2026-08-18
cd "$B" && sha256sum -c SHA256SUMS          # verify nothing rotted

# publish to the gateway (tftp source for emg)
scp "$B/Image"      hushim@enzian-gateway.inf.ethz.ch:/srv/tftp/userkernels/hushim/vmlinuz
scp "$B/initrd.img" hushim@enzian-gateway.inf.ethz.ch:/srv/tftp/userkernels/hushim/initrd.img

# modules onto z08
scp "$B/modules.tar.gz" zuestoll08:/tmp/
ssh zuestoll08 'sudo tar -C /lib/modules -xzf /tmp/modules-*.tar.gz && sudo depmod -a 6.8.0-vanilla68'

# boot it
emg acquire -n ltram -g 2025-07-28 -k hushim/vmlinuz -i hushim/initrd.img zuestoll08
```

No rebuild required — the binaries here are the ones that booted.

## To rebuild from source instead

```bash
cd /local/home/hushim/ltram-kernel
git -C ../ltram-policy-bench/linux checkout baseline/vanilla68-2026-08-18
cp baselines/vanilla68-2026-08-18/config out-vanilla/.config
./scripts/build.sh vanilla     # will run olddefconfig over the archived config
```
Expect the binary to differ (timestamps, and gcc-11 vs whatever is installed then).
`config` and `System.map` are the things to diff if behaviour changes.

## The harness module

`nor_eci_fulltest.ko` here is built against **this** kernel (`vermagic=6.8.0-vanilla68`).
It will not load on any other. Rebuild for a new kernel with:

```bash
make -C /local/home/hushim/ltram-kernel/out-vanilla \
     M=/local/home/hushim/ltram-kernel/modbuild \
     ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
```

Cross-compiling on ba8 is deliberate: z08's root is 4.4 GB and has been at 100%, and
`/lib/modules/6.8.0-vanilla68/build` there is a dangling symlink to a ba8 path.

## Where the work continues

Branch **`ltram-arm64`**, cut from `v6.8`. The existing LtRAM commits (`1f20ed07d1`) are
**x86-only** — they touch `arch/x86/mm/{init,numa}.c` and nothing under `arch/arm64`. The
generic parts (`mm/ltram.c`, the gfp plumbing, the zone) are reusable; the arch hooks are
not. First increment is `ltram_declare_node()` in `arch/arm64/mm/init.c`.
