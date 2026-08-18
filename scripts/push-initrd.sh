#!/bin/bash
# push-initrd.sh <kernelrelease> — copy the initramfs generated on z08 to the gateway.
# Separate from deploy.sh because it can only run after update-initramfs has been run.
set -euo pipefail
KREL=${1:?usage: push-initrd.sh <kernelrelease>}
ssh -q zuestoll08 "test -f /boot/initrd.img-$KREL" \
  || { echo "!! z08 has no /boot/initrd.img-$KREL — run update-initramfs there first"; exit 3; }
echo "--- z08:/boot/initrd.img-$KREL -> gateway:/srv/tftp/userkernels/hushim/initrd.img ---"
ssh -q zuestoll08 "cat /boot/initrd.img-$KREL" \
  | ssh -q hushim@enzian-gateway.inf.ethz.ch "cat > /srv/tftp/userkernels/hushim/initrd.img"
ssh -q hushim@enzian-gateway.inf.ethz.ch "ls -la /srv/tftp/userkernels/hushim/"
echo
echo "Both files are in place. Acquire with:"
echo "  emg acquire -n ltram -g 2025-07-28 -k hushim/vmlinuz -i hushim/initrd.img zuestoll08"
