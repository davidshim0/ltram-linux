#!/bin/bash
# Time an actual fill, instead of extrapolating an 8-second rate.
#
#   sudo ~/fill_rate.sh            # 16,384 pages (64 MiB) per interval
#   TARGET=8192 sudo ~/fill_rate.sh
#
# sweep_promote.sh measures dst_allocated over 8 s and fig5 multiplies that out
# to 65,536 pages -- a 600x extrapolation from an arbitrary window. It assumes
# the rate is constant over a whole fill, which is exactly the thing worth
# testing: as clean sectors run down and the bitmap fills, the allocator's
# find_first_bit walks further, and nothing in the 8 s window would show it.
#
# So: migrate a fixed 64 MiB at each interval and time it, logging every 1024
# pages so the rate can be checked WITHIN a fill and not just across one.
set -u
[ "$(id -u)" = 0 ] || exec sudo -E "$0" "$@"
DBG=/sys/kernel/debug/ltram; PAR=/sys/module/ltram_policy/parameters
REAL_HOME=$(getent passwd "${SUDO_USER:-$(id -un)}" | cut -d: -f6)
MM=$REAL_HOME/matmul; KO=$REAL_HOME/nor_eci/nor_eci_fulltest_ltram.ko
OUT=${OUT:-/var/lib/ltram/selftest/fill-rate.csv}
PROG=${PROG:-/var/lib/ltram/selftest/fill-progress.csv}
TARGET=${TARGET:-16384}          # pages to migrate per interval; 16384 = 64 MiB
# Headroom, so the allocator is never starved. Provisioning exactly TARGET
# clean sectors for a TARGET-page fill means the last migrations find nothing
# free, and with the engine off nothing refills. Measured: the rate held at a
# flat 38.5/s for 15,420 of 16,384 pages and then collapsed to 0.24/s for the
# remaining 964. That cliff was the instrument, not the device.
HEAD=${HEAD:-8192}
NN=${NN:-5793}                   # reader: 128 MiB, so candidates never run short

w(){  awk -v k="$1" '$1==k{print $2; exit}' $DBG/wear; }
ps_(){ awk -v k="$1" '$1==k{print $2; exit}' $DBG/pagestate; }
gs(){ awk -v k="$1" '$1==k{print $2; exit}' /sys/kernel/ltram/stats; }
setp(){ echo "$2" > $PAR/$1; }
setw(){ echo "$1" > $PAR/erase_high_water; echo "$2" > $PAR/erase_low_water; }

[ -r $DBG/wear ] || { echo "no $DBG/wear -- wrong kernel?"; exit 1; }
lsmod | grep -q nor_eci || insmod "$KO" provide_ops=1 test=0 inline_erase=0 verify_erased=1 \
    || { echo "!! cannot load the backend"; exit 1; }

PB0=$(cat $PAR/promote_batch); D0=$(cat $PAR/wear_days); G0=$(cat $PAR/wear_governor)
HW0=$(cat $PAR/erase_high_water); LW0=$(cat $PAR/erase_low_water)
cleanup(){ pkill -x matmul 2>/dev/null; echo 0 > /sys/kernel/ltram/target_pid 2>/dev/null
           setp promote_batch $PB0; setp wear_days $D0; setp wear_governor $G0
           setw $HW0 $LW0; }
trap cleanup EXIT INT TERM

recycle(){                        # bring clean up to $1, engine full tilt
    [ "$(ps_ clean)" -ge "$1" ] && return 0
    echo "    recycling: clean $(ps_ clean) -> $1 (dirty $(ps_ dirty))"
    setw 65536 65535
    for i in $(seq 1 3000); do
        [ "$(ps_ clean)" -ge "$1" ] && break
        [ "$(ps_ dirty)" -eq 0 ] && break
        [ $(( i % 120 )) -eq 0 ] && echo "      clean $(ps_ clean)"
        sleep 1
    done
    setw 0 0
    [ "$(ps_ clean)" -ge "$1" ]
}

mkdir -p "$(dirname "$OUT")"
echo "interval_ms,wear_days,pages,elapsed_s,rate_per_s,first_half_per_s,second_half_per_s" > "$OUT"
echo "interval_ms,pages_done,elapsed_s" > "$PROG"

recycle $(( TARGET + HEAD )) || { echo "!! cannot get $((TARGET+HEAD)) clean sectors"; exit 1; }
setw 0 0                          # engine OFF: this measures migration, not recycling
setp promote_batch 1; setp wear_governor 1

L=/tmp/fill-rate.log
$MM --n $NN --iters 1 --runs 1000000 --print-ranges --phys > $L 2>&1 &
BG=$!
for i in $(seq 1 900); do grep -q "^RANGE" $L 2>/dev/null && break; sleep 0.1; done
PID=$(pgrep -x matmul | head -1)
[ -n "${PID:-}" ] && echo $PID > /sys/kernel/ltram/target_pid
echo "  reader pid $PID, $(( NN * NN * 4 / 1048576 )) MiB; filling $TARGET pages per interval"

for target in 24 20 9 4 1; do
    kill -0 $BG 2>/dev/null || { echo "!! reader died"; break; }
    recycle $(( TARGET + HEAD )) || { echo "  !! not enough clean for ${target}ms, stopping"; break; }
    setw 0 0

    CL=$(w cycles_left)
    wd=$(awk -v t="$target" -v c="$CL" 'BEGIN{printf "%d", t*c/1000/86400 + 1}')
    setp wear_days $wd; sleep 1
    for _ in 1 2 3 4 5; do
        [ "$(w interval_ms)" -ge "$target" ] && break
        wd=$(( wd + 1 )); setp wear_days $wd; sleep 1
    done
    IV=$(w interval_ms)

    a0=$(gs dst_allocated); t0=$(date +%s.%N); half=""
    echo "  interval ${IV}ms (wear_days $wd): filling $TARGET pages ..."
    while :; do
        sleep 1
        done_=$(( $(gs dst_allocated) - a0 ))
        now=$(date +%s.%N)
        el=$(awk -v a="$t0" -v b="$now" 'BEGIN{printf "%.2f", b-a}')
        [ $(( done_ % 1024 )) -lt 64 ] && echo "$IV,$done_,$el" >> "$PROG"
        [ -z "$half" ] && [ "$done_" -ge $(( TARGET / 2 )) ] && half=$el
        [ "$done_" -ge "$TARGET" ] && break
        kill -0 $BG 2>/dev/null || break
        awk -v e="$el" -v m="${TIMEOUT:-7200}" 'BEGIN{exit !(e > m)}' && { echo "    !! ${TIMEOUT:-7200}s timeout at $done_/$TARGET"; break; }
    done
    EL=$(awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN{printf "%.2f", b-a}')
    R=$(awk -v n="$done_" -v e="$EL" 'BEGIN{printf "%.2f", n/e}')
    H1=$(awk -v n="$TARGET" -v h="${half:-0}" 'BEGIN{printf "%.2f", (h>0 ? (n/2)/h : 0)}')
    H2=$(awk -v n="$TARGET" -v h="${half:-0}" -v e="$EL" 'BEGIN{printf "%.2f", ((e-h)>0 ? (n/2)/(e-h) : 0)}')
    echo "$IV,$wd,$done_,$EL,$R,$H1,$H2" >> "$OUT"
    printf "    %s pages in %ss = %s/s   (first half %s/s, second %s/s)\n" \
           "$done_" "$EL" "$R" "$H1" "$H2"
done
kill $BG 2>/dev/null; wait $BG 2>/dev/null; rm -f $L
echo; echo "wrote $OUT and $PROG"
