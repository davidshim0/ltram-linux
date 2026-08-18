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

echo "=== deploying $WHICH — kernel release: $KREL ==="

echo "--- 1/3 kernel -> gateway:/srv/tftp/userkernels/hushim/vmlinuz ---"
scp -q "$IMG" hushim@enzian-gateway.inf.ethz.ch:/srv/tftp/userkernels/hushim/vmlinuz
ssh -q hushim@enzian-gateway.inf.ethz.ch "ls -la /srv/tftp/userkernels/hushim/vmlinuz"

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
  # initrds for kernels we are not running: they are already on the gateway
  for i in /boot/initrd.img-*; do
    k=\${i#/boot/initrd.img-}
    [ \"\$k\" = \"\$(uname -r)\" ] && continue
    case \"\$k\" in *-generic) continue;; esac
    echo \"  removing stale initrd \$k\"
    sudo rm -f \"\$i\"
  done
  sudo journalctl --vacuum-size=20M >/dev/null 2>&1 || true
  df -h / | tail -1"

echo "--- 2/3 modules -> z08:/tmp/modules-$KREL.tar.gz ---"
tar -C "$OUT/modroot/lib/modules" -czf "/tmp/modules-$KREL.tar.gz" "$KREL"
echo "    $(du -h /tmp/modules-$KREL.tar.gz | cut -f1) compressed"
scp -q "/tmp/modules-$KREL.tar.gz" zuestoll08:/tmp/
ssh -q zuestoll08 "ls -la /tmp/modules-$KREL.tar.gz; df -h / | tail -1"

echo "--- 3/3 remaining steps are yours (they modify the running system) ---"
cat <<RUN

  RUN ON z08:
    sudo tar -C /lib/modules -xzf /tmp/modules-$KREL.tar.gz
    sudo depmod -a $KREL
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
