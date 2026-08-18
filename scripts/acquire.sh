#!/bin/bash
# acquire.sh — take zuestoll08 and boot it, with a custom kernel or with the stock image.
#
#   ./scripts/acquire.sh                 # our kernel + our initrd  (the usual case)
#   ./scripts/acquire.sh --stock         # the golden image, no custom kernel
#   ./scripts/acquire.sh -n              # print the command, run nothing
#
# Every field is an environment variable, so any one of them can be overridden without
# editing this file:
#
#   NODE=zuestoll09 ./scripts/acquire.sh
#   VOL=experiment2 ./scripts/acquire.sh
#   KERNEL=hushim/vmlinuz-step6 ./scripts/acquire.sh
#   GOLDEN=2026-01-15 ./scripts/acquire.sh
#
# WHAT THE ARGUMENTS MEAN
#   -n <VOL>     the PERSISTENT VOLUME. `emg release` does NOT delete a named volume, so
#                ~/nor_eci, /lib/modules/* and ssh keys survive release/acquire. Reuse the
#                same name or you get a fresh machine with none of your tools on it.
#   -g <GOLDEN>  the golden root image to boot.
#   -k / -i      paths UNDER /srv/tftp/userkernels on the gateway. deploy.sh puts the
#                kernel at hushim/vmlinuz and push-initrd.sh the initrd at
#                hushim/initrd.img, which is why those are the defaults.
#
# ORDER MATTERS. Deploy before acquiring, or the machine boots whatever the gateway
# happened to be holding:
#   ./scripts/build.sh ltram
#   ./scripts/deploy.sh ltram          # kernel -> gateway, modules -> z08
#   ./scripts/push-initrd.sh 6.8.0-ltram
#   ./scripts/acquire.sh
set -euo pipefail

NODE=${NODE:-zuestoll08}
VOL=${VOL:-ltram}
GOLDEN=${GOLDEN:-2025-07-28}
KERNEL=${KERNEL:-hushim/vmlinuz}
INITRD=${INITRD:-hushim/initrd.img}

STOCK=0; DRY=0
for a in "$@"; do
  case "$a" in
    --stock) STOCK=1 ;;
    -n|--dry-run) DRY=1 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "unknown option: $a"; exit 2 ;;
  esac
done

if [ "$STOCK" = 1 ]; then
    CMD=(emg acquire -n "$VOL" -g "$GOLDEN" "$NODE")
else
    CMD=(emg acquire -n "$VOL" -g "$GOLDEN" -k "$KERNEL" -i "$INITRD" "$NODE")
fi

printf '%q ' "${CMD[@]}"; echo
[ "$DRY" = 1 ] && exit 0

# The gateway is the authority on what will actually boot -- print it before committing,
# because a stale vmlinuz there is indistinguishable from a bad kernel once the machine
# is up. Non-fatal: acquiring without being able to look is still allowed.
echo "--- what the gateway is holding right now ---"
ssh -o BatchMode=yes -o ConnectTimeout=8 hushim@enzian-gateway.inf.ethz.ch \
    'ls -la /srv/tftp/userkernels/hushim/' 2>/dev/null || echo "(could not read the gateway)"
echo

exec "${CMD[@]}"
