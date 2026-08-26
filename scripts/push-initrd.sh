#!/bin/bash
# push-initrd.sh <kernelrelease> — copy the initramfs generated on z08 to the gateway.
# Separate from deploy.sh because it can only run after update-initramfs has been run.
#
# WHY THIS PUBLISHES VIA A TEMP NAME
#   The obvious form of this is
#       ssh z08 "cat /boot/initrd.img-$KREL" | ssh gateway "cat > .../initrd.img"
#   and it has a failure mode that ends with an unbootable board: the redirect truncates
#   the live initrd.img the instant the remote shell starts, so ANY interruption after
#   that -- a dropped link, an expired ticket mid-stream, a full disk on the gateway --
#   leaves a short file sitting at the name tftp will serve. The board then fetches a
#   truncated initramfs and fails in the boot path, a long way from the cause.
#   So: stream to a temp name, verify the hash end to end, and only then rename over the
#   live one. mv within a directory is atomic, so initrd.img is either the old file or
#   the fully verified new one, never a partial write.
set -euo pipefail
KREL=${1:?usage: push-initrd.sh <kernelrelease>}
GW=hushim@enzian-gateway.inf.ethz.ch
DIR=/srv/tftp/userkernels/hushim

ssh -q zuestoll08 "test -f /boot/initrd.img-$KREL" \
  || { echo "!! z08 has no /boot/initrd.img-$KREL — run update-initramfs there first"; exit 3; }

# A missing Kerberos ticket shows up as "Permission denied" from the gateway, which reads
# like a key problem and is not one. Say so before the transfer rather than after it.
ssh -q -o ConnectTimeout=15 "$GW" true 2>/dev/null \
  || { echo "!! cannot reach the gateway — run 'kinit' (the ticket expires, the board is fine)"; exit 6; }

SRC_SHA=$(ssh -q zuestoll08 "sha256sum /boot/initrd.img-$KREL" | cut -d' ' -f1)
echo "--- z08:/boot/initrd.img-$KREL -> gateway:$DIR/initrd.img ---"
echo "    source sha256 $SRC_SHA"

ssh -q zuestoll08 "cat /boot/initrd.img-$KREL" \
  | ssh -q "$GW" "cat > $DIR/initrd.img.incoming"

DST_SHA=$(ssh -q "$GW" "sha256sum $DIR/initrd.img.incoming" | cut -d' ' -f1)
if [ "$SRC_SHA" != "$DST_SHA" ]; then
    echo "!! TRANSFER CORRUPTED — the live initrd.img has NOT been touched"
    echo "!!   z08     $SRC_SHA"
    echo "!!   gateway $DST_SHA"
    ssh -q "$GW" "rm -f $DIR/initrd.img.incoming"
    exit 5
fi

ssh -q "$GW" "mv $DIR/initrd.img.incoming $DIR/initrd.img"
echo "    gateway MATCHES — published"
ssh -q "$GW" "ls -la $DIR/"
echo
echo "Both files are in place. Acquire with:"
echo "  emg acquire -n ltram -g 2025-07-28 -k hushim/vmlinuz -i hushim/initrd.img zuestoll08"
