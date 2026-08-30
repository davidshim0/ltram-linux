#!/bin/bash
# sweep_promote.sh -- re-measure promotion rate against interval, nothing else.
#
# Section E of the self-test produces the only data fig5 needs, but running it
# means a full --cycle: two hours, a reboot, and every other section. This is
# that section on its own, lifted verbatim so the two cannot drift.
#
# About ten minutes: most of it draining enough clean sectors to fill into.
set -u
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"
DBG=/sys/kernel/debug/ltram
PAR=/sys/module/ltram_policy/parameters
REAL_HOME=$(getent passwd "${SUDO_USER:-$(id -un)}" | cut -d: -f6)
MM=$REAL_HOME/matmul
KO=$REAL_HOME/nor_eci/nor_eci_fulltest_ltram.ko
OUT=${1:-/var/lib/ltram/selftest/promote-rate.csv}

w(){  awk -v k="$1" '$1==k{print $2; exit}' $DBG/wear; }
ps_(){ awk -v k="$1" '$1==k{print $2; exit}' $DBG/pagestate; }
gs(){ awk -v k="$1" '$1==k{print $2; exit}' /sys/kernel/ltram/stats; }
setp(){ [ -n "${2:-}" ] || return 0; echo "$2" > $PAR/$1; }
setw(){ echo "$1" > $PAR/erase_high_water; echo "$2" > $PAR/erase_low_water; }
say(){ echo "  $*"; }

[ -r $DBG/wear ] || { echo "no $DBG/wear -- wrong kernel?"; exit 1; }
[ -x "$MM" ] || { echo "no matmul at $MM"; exit 1; }
lsmod | grep -q nor_eci || insmod "$KO" provide_ops=1 test=0 inline_erase=0 verify_erased=1 \
    || { echo "!! cannot load the backend -- nothing can erase"; exit 1; }

PB0=$(cat $PAR/promote_batch); D0=$(cat $PAR/wear_days)
G0=$(cat $PAR/wear_governor); HW0=$(cat $PAR/erase_high_water); LW0=$(cat $PAR/erase_low_water)
cleanup(){ pkill -x matmul 2>/dev/null; echo 0 > /sys/kernel/ltram/target_pid 2>/dev/null
           setp promote_batch $PB0; setp wear_days $D0; setp wear_governor $G0
           setw $HW0 $LW0; }
trap cleanup EXIT INT TERM

EN=4096; EPAGES=$(( EN * EN * 4 / 4096 ))
if [ "$(ps_ clean)" -lt $EPAGES ]; then
    say "recycling: clean $(ps_ clean) -> $EPAGES (dirty $(ps_ dirty))"
    setw 65536 65535
    for i in $(seq 1 1200); do
        [ "$(ps_ clean)" -ge "$EPAGES" ] && break
        [ "$(ps_ dirty)" -eq 0 ] && break
        sleep 1
    done
fi
setw 0 0
[ "$(ps_ clean)" -ge "$EPAGES" ] || { echo "!! only $(ps_ clean) clean, need $EPAGES"; exit 1; }

mkdir -p "$(dirname "$OUT")"
echo "wear_days,interval_ms,measured_per_s,predicted_per_s" > "$OUT"
L=/tmp/sweep-promote.log
$MM --n $EN --iters 1 --runs 4000 --print-ranges --phys > $L 2>&1 &
BG=$!
for i in $(seq 1 600); do grep -q "^RANGE" $L 2>/dev/null && break; sleep 0.1; done
PID=$(pgrep -x matmul | head -1)
[ -n "${PID:-}" ] && echo $PID > /sys/kernel/ltram/target_pid
setp promote_batch 1; setp wear_governor 1

for target in 24 20 9 4 1; do
    kill -0 $BG 2>/dev/null || break
    CL=$(w cycles_left)
    wd=$(awk -v t="$target" -v c="$CL" 'BEGIN{printf "%d", t*c/1000/86400 + 1}')
    setp wear_days $wd; sleep 1
    # Walk up until the kernel reports the interval asked for: seconds_left is
    # measured from the epoch to NOW, so the open-form inversion always lands
    # a millisecond low.
    for _ in 1 2 3 4 5; do
        IV=$(w interval_ms)
        [ "${IV:-0}" -ge "$target" ] && break
        wd=$(( wd + 1 )); setp wear_days $wd; sleep 1
    done
    sleep 2
    IV=$(w interval_ms)
    a0=$(gs dst_allocated); t0=$(date +%s.%N)
    sleep 8
    a1=$(gs dst_allocated); t1=$(date +%s.%N)
    MR=$(awk -v a="$a0" -v b="$a1" -v x="$t0" -v y="$t1" 'BEGIN{printf "%.1f", (b-a)/(y-x)}')
    PR=$(awk -v i="$IV" 'BEGIN{printf "%.1f", 1000.0/(i+3)}')
    echo "$wd,$IV,$MR,$PR" >> "$OUT"
    printf "  wear_days %-5s interval %2s ms   measured %6s/s   model %6s/s\n" "$wd" "$IV" "$MR" "$PR"
    [ "$(ps_ clean)" -lt 500 ] && break
done
kill $BG 2>/dev/null; wait $BG 2>/dev/null
rm -f $L
echo
echo "wrote $OUT"
