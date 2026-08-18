# ltram-linux — NOR flash as coherent memory on Enzian

Micron MT35XU02GCBA octal-DDR NOR flash, reached over ECI through the Enzian FPGA,
presented to Linux as a NUMA node and a memory zone, with a placement policy that
promotes read-mostly pages onto it. Cavium ThunderX arm64 + Xilinx VU9P.

**The one hardware fact everything follows from:** the processor can LOAD from the flash
window coherently, and can never STORE to it. A store retires normally and the data is
silently discarded; a later load returns the previous flash contents, which look entirely
plausible. Data reaches the array only through the FPGA's DMA ring. Every mechanism here
exists to manufacture a signal the hardware does not give.

## Layout

```
linux/          the kernel — mainline v6.8 plus one commit per step
scripts/        build.sh  deploy.sh  push-initrd.sh  assert_kconfig.sh
configs/        golden-6.8.0-64.config, the seed for every build
workloads/      matmul (the measurement control) and ltram-inspect
docs/           design decisions, kernel architecture, migration mechanism
baselines/      one directory per verified build, indexed by BASELINES.md
out-vanilla/    build output (gitignored)
out-ltram/
```

## Why a vanilla build comes first

zuestoll08 network-boots and mounts an **iSCSI root**. A custom kernel has to survive
tftp → NIC driver → DHCP → iSCSI initiator → root mount before a single line of our code
runs. Going straight to an LtRAM kernel and having it not come up tells you nothing about
which half is broken. So: build the control first, boot it, then change one thing at a time.

The control is **this same tree with `CONFIG_LTRAM` off**. One config symbol separates it
from the experiment, not two source trees, which is what makes an A/B measurement mean
anything. `CONFIG_LTRAM=n` has been verified inert: zero ltram symbols, differing from
pure v6.8 only in four initcall names whose encoded source line numbers moved.

## Building

```bash
./scripts/build.sh vanilla        # the control
./scripts/build.sh ltram          # the experiment
./scripts/assert_kconfig.sh out-ltram/.config
```

## The golden config

`configs/golden-6.8.0-64.config` is zuestoll08's own running config, copied from
`/boot/config-6.8.0-64-generic`. It is **in this repository** — no setup step needed.

It matters because a `defconfig` kernel **cannot boot this machine**: no ThunderX NIC, no
iSCSI, so no root filesystem. `build.sh` seeds from this file, runs `olddefconfig` to drop
the Ubuntu symbols mainline does not have, strips the module-signing and integrity stack
(Canonical keys that do not exist here), and then **asserts** that `ISCSI_TCP`,
`IP_PNP_DHCP`, `THUNDER_NIC_VF`, `RODATA_FULL_DEFAULT_ENABLED` and `BLK_DEV_INITRD`
survived — refusing to build a kernel that cannot boot rather than discovering it on the
board.

To refresh it from a differently-configured z08, run **on z08**:

```bash
scp /boot/config-6.8.0-64-generic \
    hushim@enzian-ba8.inf.ethz.ch:/local/home/hushim/ltram-linux/configs/golden-6.8.0-64.config
```

## Step 1 — build

```bash
cd /local/home/hushim/ltram-linux
./scripts/build.sh vanilla
```

Produces `out-vanilla/arch/arm64/boot/Image` plus stripped modules under
`out-vanilla/modroot/`. `LOCALVERSION` tags it `-vanilla68` so `uname -r` distinguishes it
from the golden kernel at a glance.

## Known variables, stated rather than discovered later

**Toolchain.** The golden kernel was built with `aarch64-linux-gnu-gcc-13`; ba8 has
**gcc-11**. Different major gcc means a different binary. It should still boot, but if the
vanilla kernel misbehaves, install `gcc-13-aarch64-linux-gnu` before suspecting anything
else.

**Mainline vs Ubuntu.** The golden kernel is Ubuntu's `6.8.0-64-generic`, which is
patched; our tree is mainline `v6.8`. `olddefconfig` drops the Ubuntu-only symbols. This
is the intended target — the LtRAM work is on mainline — but it means the vanilla build is
"mainline 6.8 configured like the golden kernel", not a byte-match.

**Initramfs is not built here, and it is required.** Network + iSCSI root needs an initrd
carrying the matching modules and the open-iscsi userspace. It has to be generated **on
z08** with `initramfs-tools` after the modules are installed there, because only z08 has
the iSCSI userspace and the boot scripts. Building `Image` alone is not a bootable
deliverable.

**z08 disk.** `/` is 4.4 GB and was recently at 100%. `make modules_install` plus an
initramfs needs headroom — `INSTALL_MOD_STRIP=1` is already set in the build. Check
`df -h /` before installing modules.

**The tftp blocker.** Booting a custom kernel needs it placed in
`/srv/tftp/userkernels/hushim`, which requires the admin. That is a standing blocker
independent of anything here.

## Step 2 — the boot test that actually proves something

Boot vanilla and confirm, in order:

1. `uname -r` ends in `-vanilla68` — you are running our kernel, not the golden one
2. `/` is mounted from iSCSI (`findmnt /`) — the hard part of the chain worked
3. `ip a` shows `enP2p1s0v0` with an address — the NIC driver and DHCP worked
4. `dmesg -l err,warn` is not full of storage or network complaints

Only after all four does an LtRAM kernel tell you anything.
