#!/bin/bash
# build.sh — cross-build an arm64 kernel for zuestoll08 from ba8.
#
#   ./scripts/build.sh vanilla   # linux/ with CONFIG_LTRAM off -- the control
#   ./scripts/build.sh ltram     # linux/ with CONFIG_LTRAM on
#
# WHY A VANILLA BUILD FIRST
#   zuestoll08 network-boots (/goldenimage/.../vmlinuz) with an ISCSI ROOT. Booting a
#   custom kernel exercises: tftp -> kernel -> NIC driver -> DHCP -> iSCSI initiator ->
#   root mount. Any one of those can fail for reasons that have nothing to do with LtRAM.
#   Proving a VANILLA v6.8 boots first separates "my boot path works" from "my changes
#   work". The control is this same tree with CONFIG_LTRAM off: one config symbol
#   separates the two kernels, not two trees, which is what makes the A/B honest.
#   CONFIG_LTRAM=n has been verified inert -- zero ltram symbols, differing from pure
#   v6.8 only in four initcall names whose encoded source line numbers moved.
#
# THE TOOLCHAIN MISMATCH, STATED UP FRONT
#   The golden kernel was built with aarch64-linux-gnu-gcc-13. ba8 has gcc-11 only.
#   A kernel built with a different major gcc is a different binary; it should still
#   boot, but it is a variable. If the vanilla build misbehaves, install
#   gcc-13-aarch64-linux-gnu before suspecting anything else.
set -euo pipefail

# The kernel is linux/; everything else in this repository is the apparatus around it.
# Both build targets share that one source tree and differ only by CONFIG_LTRAM, which
# is exactly the property that makes the A/B comparison mean anything -- one config
# symbol separates the two kernels, not two trees.
BASE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# KSRC lets build-step.sh point us at an earlier step's kernel while keeping THIS
# script, this config seed and these assertions. Tags pin the kernel; tooling is latest.
SRC=${KSRC:-$BASE/linux}

WHICH=${1:?usage: build.sh vanilla|ltram}
case "$WHICH" in
  vanilla) OUT=$BASE/out-vanilla; LV="-vanilla68"; CFG="--disable LTRAM" ;;
  ltram)   OUT=$BASE/out-ltram;   LV="-ltram";     CFG="--enable LTRAM"  ;;
  *) echo "unknown target: $WHICH"; exit 2 ;;
esac

export ARCH=arm64
export CROSS_COMPILE=${CROSS_COMPILE:-aarch64-linux-gnu-}
JOBS=${JOBS:-$(nproc)}

# ---- host-tool openssl, borrowed ---------------------------------------------
# scripts/sign-file and certs/extract-cert are HOST tools (x86, built on ba8) and need
# openssl headers. ba8 has libcrypto.so.3 but no headers, and no passwordless sudo to
# install them. Headers are plain text and architecture-independent, so they were copied
# from z08 (which has libssl-dev) into hostdeps/, and linked against ba8's OWN x86
# libcrypto -- z08's arm64 library could not link here.
# Header version 3.0.13 vs library 3.0.2: both OpenSSL 3.0.x, API-stable within the series.
export HOSTCFLAGS="-I$BASE/hostdeps/include ${HOSTCFLAGS:-}"
export HOSTLDFLAGS="-L$BASE/hostdeps/lib ${HOSTLDFLAGS:-}"
GOLDEN=$BASE/configs/golden-6.8.0-64.config

command -v ${CROSS_COMPILE}gcc >/dev/null || { echo "!! no ${CROSS_COMPILE}gcc"; exit 3; }
echo "=== $WHICH | $(${CROSS_COMPILE}gcc --version | head -1) | -j$JOBS ==="

mkdir -p "$OUT"

# Suppress the trailing "+" that setlocalversion appends when the tree is not at a clean
# ANNOTATED tag. The + is cosmetic but it lands in UTS_RELEASE, in /lib/modules/<rel> and
# in the initramfs name, so a stray one causes module-path mismatches at boot.
#
# The lever is LOCALVERSION being *set*, per setlocalversion's own comment: "If the
# variable LOCALVERSION is set (including being set to an empty string), we don't want to
# append a plus sign." Setting it empty leaves CONFIG_LOCALVERSION (-vanilla68 / -ltram)
# intact, because the release string concatenates both.
#
# The .scmversion trick does NOT work here: that handling was removed before 6.8, and
# creating the file only makes the tree look dirty.
export LOCALVERSION=

# ---- config -----------------------------------------------------------------
if [ ! -f "$OUT/.config" ]; then
    if [ -f "$GOLDEN" ]; then
        echo "--- seeding from the golden config, then olddefconfig for mainline ---"
        cp "$GOLDEN" "$OUT/.config"
        # Ubuntu configs carry symbols mainline does not have; olddefconfig drops them
        # and takes defaults for anything new. That is what makes the golden config
        # usable on a mainline tree at all.
        make -C "$SRC" O="$OUT" olddefconfig
    else
        echo "!! $GOLDEN missing — falling back to defconfig."
        echo "!! A defconfig kernel will NOT boot this machine: no ThunderX NIC, no iSCSI root."
        echo "!! Copy the golden config from z08 first (see README)."
        make -C "$SRC" O="$OUT" defconfig
    fi
fi

# ---- strip the Ubuntu-isms the golden config carries -------------------------
# The golden config is Ubuntu's, and Ubuntu signs its modules against Canonical keys
# that do not exist here. Left on, the build dies in scripts/sign-file needing
# openssl/opensslv.h, and even with libssl-dev it would then fail looking for
# debian/canonical-certs.pem. We do not want signed modules for our own kernel, so
# turn the whole apparatus off rather than satisfying it.
#
# NOTE: disabling MODULE_SIG is not enough on its own. IMA_APPRAISE_MODSIG also
# 'select MODULE_SIG_FORMAT' (security/integrity/ima/Kconfig), which still builds
# scripts/sign-file and still needs openssl headers. Both have to go.
#
# The same applies to certs/extract-cert, built for the trusted keyring. ba8 has NO
# openssl headers and NO passwordless sudo to install them, so every consumer of
# openssl has to be off -- which means the whole integrity/IMA/keyring stack. None of
# it is required to boot: it is measurement and appraisal for secure-boot systems, and
# we are booting our own unsigned kernel over tftp by design.
#
# KEXEC_SIG is the non-obvious one: kernel/Kconfig.kexec 'select SYSTEM_TRUSTED_KEYRING',
# so olddefconfig turns the keyring back on after we disable it, and certs/extract-cert
# gets built again. Disabling the leaf is not enough -- the selector has to go too.
"$SRC"/scripts/config --file "$OUT/.config" \
    --disable MODULE_SIG --disable MODULE_SIG_ALL --disable MODULE_SIG_FORCE \
    --disable SECURITY_LOCKDOWN_LSM --disable SECURITY_LOCKDOWN_LSM_EARLY \
    --set-str SYSTEM_TRUSTED_KEYS "" \
    --set-str SYSTEM_REVOCATION_KEYS "" \
    --disable SYSTEM_REVOCATION_LIST \
    --disable DEBUG_INFO_BTF \
    --disable IMA_APPRAISE_MODSIG \
    --disable SYSTEM_TRUSTED_KEYRING --disable SECONDARY_TRUSTED_KEYRING \
    --disable INTEGRITY --disable IMA --disable EVM \
    --disable INTEGRITY_SIGNATURE --disable INTEGRITY_ASYMMETRIC_KEYS \
    --disable KEXEC_SIG --disable SYSTEM_BLACKLIST_KEYRING \
    --disable MODULE_SIG_KEY_TYPE_RSA \
    --disable LOCALVERSION_AUTO \
    ${LTRAM_CFG:-$CFG}

# ---- automatic NUMA balancing: OFF from boot --------------------------------
# Declaring node 1 takes num_online_nodes() to 2, and mm/mempolicy.c then turns automatic
# NUMA balancing ON because Ubuntu's config sets DEFAULT_ENABLED. On the vanilla control
# there was one node, so it stayed off -- meaning the control and the experiment differed
# in a way that had nothing to do with LtRAM.
#
# It has to be off for a second, larger reason: it uses the SAME mechanism on the SAME
# PTEs as the placement policy, and wants the opposite outcome. It sets PROT_NONE to
# sample access while our scanner write-protects to observe writes, so each clobbers the
# other's state; and it migrates pages TOWARD the local node -- node 0 -- which undoes
# every promotion the policy makes.
#
# Disabling the DEFAULT rather than compiling the feature out (CONFIG_NUMA_BALANCING stays
# y) is deliberate: stock NUMA balancing is the obvious comparison policy -- "what would
# Linux do on its own?" -- and leaving the code in makes that one sysctl away instead of a
# rebuild. check_numabalancing_enable() still logs "Disabling automatic NUMA balancing",
# so the state is visible in dmesg rather than being invisible until someone measures it.
"$SRC"/scripts/config --file "$OUT/.config" --disable NUMA_BALANCING_DEFAULT_ENABLED

# ---- the settings this machine cannot boot without --------------------------
# Set explicitly rather than trusting olddefconfig, and verified after.
"$SRC"/scripts/config --file "$OUT/.config" \
    --set-str LOCALVERSION "$LV" \
    --enable  BLK_DEV_INITRD \
    --enable  ISCSI_TCP --enable SCSI_ISCSI_ATTRS --enable ISCSI_BOOT_SYSFS \
    --enable  IP_PNP --enable IP_PNP_DHCP \
    --enable  THUNDER_NIC_PF --enable THUNDER_NIC_VF --enable THUNDER_NIC_BGX \
    --enable  THUNDER_NIC_RGX --enable MDIO_THUNDER \
    --enable  RODATA_FULL_DEFAULT_ENABLED \
    --enable  ARCH_HAS_SET_DIRECT_MAP \
    --enable  PTDUMP_DEBUGFS
make -C "$SRC" O="$OUT" olddefconfig

echo "--- asserting the settings that must survive ---"
fail=0
for sym in ISCSI_TCP IP_PNP_DHCP THUNDER_NIC_VF RODATA_FULL_DEFAULT_ENABLED BLK_DEV_INITRD; do
    if grep -q "^CONFIG_$sym=y" "$OUT/.config" || grep -q "^CONFIG_$sym=m" "$OUT/.config"; then
        printf "  %-34s ok\n" "CONFIG_$sym"
    else
        printf "  %-34s MISSING\n" "CONFIG_$sym"; fail=1
    fi
done
# ...and one that must be ABSENT. Automatic NUMA balancing fights the placement policy
# for the same PTEs, so a build with it defaulted on is not a build we want to measure.
if grep -q "^CONFIG_NUMA_BALANCING_DEFAULT_ENABLED=y" "$OUT/.config"; then
    printf "  %-34s SHOULD BE OFF\n" "CONFIG_NUMA_BALANCING_DEFAULT_ENABLED"; fail=1
else
    printf "  %-34s off (correct)\n" "CONFIG_NUMA_BALANCING_DEFAULT_ENABLED"
fi

[ $fail -eq 0 ] || { echo "!! required config missing — refusing to build a kernel that cannot boot"; exit 4; }

# ---- build ------------------------------------------------------------------
make -C "$SRC" O="$OUT" -j"$JOBS" Image modules dtbs
make -C "$SRC" O="$OUT" INSTALL_MOD_STRIP=1 INSTALL_MOD_PATH="$OUT/modroot" modules_install

KREL=$(make -C "$SRC" O="$OUT" -s kernelrelease)
IMG="$OUT/arch/arm64/boot/Image"

# ---- identity ---------------------------------------------------------------
# Printed at the end and written to $OUT/BUILDINFO so it travels with the artifact.
#
# The point is being able to answer, standing at the board, "is the thing that booted
# the thing I built?" -- and neither a date nor a size answers that. The Image sha256
# does: deploy.sh re-hashes what actually landed on the gateway and compares.
#
# git describe carries the tag when HEAD is exactly on one, and tag-N-gHASH when it is
# not, so a build from an untagged commit cannot silently claim to be a tagged release.
FINISHED=$(date '+%Y-%m-%d %H:%M:%S %Z')
DESCRIBE=$(git -C "$BASE" describe --tags --always --dirty 2>/dev/null || echo "not a git checkout")
COMMIT=$(git -C "$BASE" rev-parse --short HEAD 2>/dev/null || echo unknown)
BRANCH=$(git -C "$BASE" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)
IMGSHA=$(sha256sum "$IMG" | cut -d' ' -f1)
NMOD=$(find "$OUT/modroot/lib/modules" -type f -name '*.ko*' 2>/dev/null | wc -l)
MODSZ=$(du -sh "$OUT/modroot/lib/modules" 2>/dev/null | cut -f1)
CCVER=$(${CROSS_COMPILE}gcc --version | head -1)

{
  echo "target          $WHICH"
  [ -n "${STEP_TAG:-}" ] && echo "kernel from     $STEP_TAG"
  [ -n "${STEP_TOOLING:-}" ] && echo "tooling from    $STEP_TOOLING"
  echo "kernel release  $KREL"
  echo "version         $DESCRIBE"
  echo "commit          $COMMIT  (branch $BRANCH)"
  echo "built           $FINISHED"
  echo "toolchain       $CCVER"
  echo "Image           $IMG"
  echo "Image size      $(stat -c%s "$IMG") bytes"
  echo "Image sha256    $IMGSHA"
  echo "modules         $NMOD objects, $MODSZ"
} > "$OUT/BUILDINFO"

echo
echo "================================================================"
cat "$OUT/BUILDINFO"
echo "================================================================"

# A dirty tree means the version name above is a claim the tree does not support:
# the tag names a commit, and the build contains something else.
case "$DESCRIBE" in
  *-dirty)
    echo
    echo "!! WORKING TREE IS DIRTY — '$DESCRIBE' does not describe what was built."
    echo "!! Commit or stash before archiving this as a baseline."
    git -C "$BASE" status --short | head -10
    ;;
esac

echo
echo "Written to $OUT/BUILDINFO — deploy.sh reads it and verifies the sha256 on the gateway."
echo "Next: deploy.sh $WHICH  (see README for what z08 has to do with it)"
