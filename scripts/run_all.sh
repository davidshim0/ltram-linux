#!/bin/bash
# run_all.sh -- clean slate, then the full sweep. Start it and walk away.
#
#   1. check the kernel is the one with the fixes
#   2. load the backend, because without an erase op the pool cannot recover
#   3. erase the entire array, so every sector is provably blank
#   4. start a fresh CSV
#   5. run all five modes at every size
#
# Step 3 is the one that takes time: ~16.4 ms a sector, so up to 18 minutes.
# It is not optional. The sweep measures residency and timing on the
# assumption that a clean sector is blank, and the only way to know that
# after a reboot is to have erased it or read it.
set -u
OUT=${1:-/scratch/hushim/sweep.csv}
KO=$HOME/nor_eci/nor_eci_fulltest_ltram.ko
P=/sys/kernel/debug/ltram/pagestate
HW=/sys/module/ltram_policy/parameters/erase_high_water
LW=/sys/module/ltram_policy/parameters/erase_low_water
say(){ echo "[$(date +%H:%M:%S)] $*"; }
g(){ sudo -n cat $P | awk -v k="$1" '$1==k{print $2; exit}'; }

say "=== 1. kernel check ==="
uname -r; sudo -n dmesg | grep -E "ltram: (pool scan|page states|policy)" | tail -3
if [ -z "$(g clean)" ]; then
    echo "!! pagestate has no 'clean' field. This kernel predates the rename," >&2
    echo "!! so it also predates the lt_written fix. Reboot into the new one." >&2
    exit 3
fi

say "=== 2. backend ==="
lsmod | grep -q nor_eci || sudo -n insmod $KO provide_ops=1 test=0 \
    inline_erase=0 verify_erased=1 || exit 4
say "  loaded, inline_erase=0 verify_erased=1"

say "=== 3. erasing the whole array ==="
say "  before: clean=$(g clean) dirty=$(g dirty) data=$(g data)"
echo 65536 | sudo -n tee $HW >/dev/null; echo 65535 | sudo -n tee $LW >/dev/null
for i in $(seq 1 2400); do
    D=$(g dirty); [ "${D:-1}" = "0" ] && break
    [ $((i % 60)) -eq 0 ] && say "  dirty=$D still draining (${i}s)"
    sleep 1
done
echo 8192 | sudo -n tee $HW >/dev/null; echo 2048 | sudo -n tee $LW >/dev/null
say "  after:  clean=$(g clean) dirty=$(g dirty)  engine=$(g erase_engine)"
[ "$(g dirty)" = "0" ] || { echo "!! array not fully erased, refusing to start" >&2; exit 5; }

say "=== 4. fresh CSV ==="
[ -f "$OUT" ] && mv "$OUT" "$OUT.$(date +%m%d-%H%M%S).bak" && say "  old one kept as a .bak"

say "=== 5. sweep, all five modes ==="
exec "$HOME/sweep.sh" --out "$OUT"

# Persist the erase counts now that the sweep has finished burning through
# them. No-op if the systemd unit is not installed.
[ -x /usr/local/sbin/ltram-erase-counts ] && \
    sudo -n /usr/local/sbin/ltram-erase-counts save || true
