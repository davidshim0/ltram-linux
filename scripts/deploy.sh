#!/bin/bash
# deploy.sh — stage a built kernel for booting zuestoll08 via `emg acquire`.
#
#   ./scripts/deploy.sh vanilla
#
# WHAT BOOTS WHAT
#   emg acquire -n ltram -g 2025-07-28 -k hushim/vmlinuz -i hushim/initrd.img zuestoll08
#   so the two files must land as, on the GATEWAY:
#       /srv/tftp/userkernels/hushim/vmlinuz
#       /srv/tftp/userkernels/hushim/initrd.img
#   Omitting -k/-i acquires the stock 6.8 golden image instead -- that is the way back.
#
# This script does the parts that need no judgement:
#   1. copy the kernel Image to the gateway as vmlinuz
#   2. ship the module tree to z08 as a tarball
# It deliberately does NOT install modules or build the initramfs on z08 -- those change
# the running system, so they are printed for you to run and check.
set -euo pipefail
BASE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WHICH=${1:?usage: deploy.sh vanilla|ltram}
case "$WHICH" in
  vanilla) OUT=$BASE/out-vanilla ;;
  ltram)   OUT=$BASE/out-ltram ;;
  *) echo "unknown target: $WHICH"; exit 2 ;;
esac

KREL=$(cat "$OUT/include/config/kernel.release")
IMG="$OUT/arch/arm64/boot/Image"
[ -f "$IMG" ] || { echo "!! no Image at $IMG — build first"; exit 3; }
case "$KREL" in *+) echo "!! kernelrelease '$KREL' ends in '+'; fix before deploying"; exit 4;; esac

echo "=== deploying $WHICH ==="
if [ -f "$OUT/BUILDINFO" ]; then
    cat "$OUT/BUILDINFO"
else
    echo "!! no $OUT/BUILDINFO — this build predates identity recording, or was not built"
    echo "!! by build.sh. Rebuild if you need to know what this is."
    echo "kernel release  $KREL"
fi
echo

echo "--- 1/3 kernel -> gateway:/srv/tftp/userkernels/hushim/vmlinuz ---"
scp -q "$IMG" hushim@enzian-gateway.inf.ethz.ch:/srv/tftp/userkernels/hushim/vmlinuz

# Verify what LANDED, not what was sent. ls gives a size and a date, and both match
# between builds often enough to be useless -- two of our own Images are byte-identical
# in size. The hash is the only thing that answers "is the kernel on the gateway the one
# I just built?", which is the question you are standing at the board asking.
LOCAL_SHA=$(sha256sum "$IMG" | cut -d' ' -f1)
REMOTE_SHA=$(ssh -q hushim@enzian-gateway.inf.ethz.ch "sha256sum /srv/tftp/userkernels/hushim/vmlinuz" | cut -d' ' -f1)
if [ "$LOCAL_SHA" = "$REMOTE_SHA" ]; then
    echo "    sha256 $LOCAL_SHA — gateway MATCHES local"
else
    echo "!! GATEWAY MISMATCH — the copy did not land intact"
    echo "!!   local   $LOCAL_SHA"
    echo "!!   gateway $REMOTE_SHA"
    exit 5
fi
ssh -q hushim@enzian-gateway.inf.ethz.ch "ls -la /srv/tftp/userkernels/hushim/"

# z08's root is 4.4 GB and each kernel's module tree is ~145 MB, so two trees plus an
# initramfs do not fit. Free the ones we can rebuild before shipping the new one. Every
# tree removed here is archived on ba8 under baselines/, so this loses nothing except the
# time to copy it back.
echo "--- 1.5/3 making room on z08 ---"
ssh -q zuestoll08 "set -e
  df -h / | tail -1
  for d in /lib/modules/*; do
    k=\$(basename \$d)
    [ \"\$k\" = \"$KREL\" ] && continue                 # the one we are installing
    [ \"\$k\" = \"\$(uname -r)\" ] && continue          # the one currently running
    case \"\$k\" in *-generic) continue;; esac           # the distro kernel, leave alone
    echo \"  removing stale module tree \$k\"
    sudo rm -rf \"\$d\"
  done
  # z08 NETBOOTS: the initrd it is running came from the gateway over tftp, not from
  # /boot. Everything in /boot/initrd.img-* is a staging artifact that push-initrd.sh
  # has already published, so removing one cannot affect the running system and costs
  # only an update-initramfs to rebuild. That deliberately includes the RUNNING
  # kernel's -- which is the point, because the kernel being redeployed is usually the
  # running one, and its 70 MB is exactly the room update-initramfs needs for the new
  # one. The distro kernel's is still left alone: it is the documented way back.
  for i in /boot/initrd.img-*; do
    k=\${i#/boot/initrd.img-}
    case \"\$k\" in *-generic) continue;; esac
    echo \"  removing staged initrd \$k (republish with push-initrd.sh)\"
    sudo rm -f \"\$i\"
  done
  # configs for kernels whose module trees are long gone
  for c in /boot/config-*; do
    k=\${c#/boot/config-}
    case \"\$k\" in *-generic) continue;; esac
    [ -d \"/lib/modules/\$k\" ] || {
      echo \"  removing orphan config-\$k\"
      sudo rm -f \"\$c\"
    }
  done
  sudo journalctl --vacuum-size=20M >/dev/null 2>&1 || true
  df -h / | tail -1"

echo "--- 2/3 modules -> z08:/tmp/modules-$KREL.tar.gz ---"
tar -C "$OUT/modroot/lib/modules" -czf "/tmp/modules-$KREL.tar.gz" "$KREL"
echo "    $(du -h /tmp/modules-$KREL.tar.gz | cut -f1) compressed"
scp -q "/tmp/modules-$KREL.tar.gz" zuestoll08:/tmp/

# The kernel config, as /boot/config-$KREL. Not cosmetic: mkinitramfs reads it to check
# the kernel can decompress the compressor initramfs.conf asks for. Without it you get
#   W: Kernel configuration /boot/config-$KREL is missing, cannot check for zstd ...
# and -- this is the part that matters -- it does NOT fall back to something safe. It
# proceeds with the configured compressor unverified. COMPRESS=zstd against a kernel
# without CONFIG_RD_ZSTD is an unbootable initramfs, and the warning is the only notice
# you get. 212 KB to turn a skipped check into a real one.
echo "--- 2.5/3 kernel config -> z08:/boot/config-$KREL ---"
scp -q "$OUT/.config" "zuestoll08:/tmp/config-$KREL"
ssh -q zuestoll08 "sudo install -m 0644 /tmp/config-$KREL /boot/config-$KREL && rm -f /tmp/config-$KREL && ls -la /boot/config-$KREL"
ssh -q zuestoll08 "ls -la /tmp/modules-$KREL.tar.gz; df -h / | tail -1"

echo "--- 3/3 remaining steps are yours (they modify the running system) ---"
cat <<RUN

  RUN ON z08 (the rm matters: / has ~470 MB of slack against a ~340 MB cycle, so the
  tarball has to go before update-initramfs runs, not after):
    sudo tar -C /lib/modules -xzf /tmp/modules-$KREL.tar.gz && rm -f /tmp/modules-$KREL.tar.gz
    sudo depmod -a $KREL
    df -h /                          # want >300M free before the next line
    sudo update-initramfs -c -k $KREL
    ls -la /boot/initrd.img-$KREL
    df -h /

  THEN, to publish the initramfs (from ba8, once it exists):
    ./scripts/push-initrd.sh $KREL

  THEN acquire with the custom kernel:
    emg acquire -n ltram -g 2025-07-28 -k hushim/vmlinuz -i hushim/initrd.img zuestoll08

  TO GO BACK to the stock golden image if it does not boot:
    emg acquire -n ltram -g 2025-07-28 zuestoll08

RUN
