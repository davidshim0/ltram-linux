#!/bin/bash
# sweep_gated.sh -- migration into an ALREADY-DIRTY pool.
#
# sweep_worstcase.sh reaches this state the long way: drain the pool, fill it,
# evict it, then refill. But the only thing the refill needs is a workload
# promoting into sectors that must be erased first -- and after any real use
# the pool is already like that. So skip the preamble.
#
#   phase 1  DRAM              nothing targeted
#   phase 2  erase-gated fill  every promotion waiting on the erase engine
#   phase 3  NOR, engine on    settled, still recycling
#   phase 4  NOR, engine off   settled, nothing recycling
#
# Phases 1 and 2 of sweep_timeline.sh are the clean-pool comparison: same
# size, same interval, same workload, so the two migrations can be read
# against each other even though they are different runs.
#
# DOES NOT DRAIN. Draining is exactly what this measurement must not have.
set -u
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"
DBG=/sys/kernel/debug/ltram
PAR=/sys/module/ltram_policy/parameters
REAL_HOME=$(getent passwd "${SUDO_USER:-$(id -un)}" | cut -d: -f6)
MM=$REAL_HOME/matmul
KO=$REAL_HOME/nor_eci/nor_eci_fulltest_ltram.ko
OUT=${1:-/var/lib/ltram/selftest/gated.csv}

w(){  awk -v k="$1" '$1==k{print $2; exit}' $DBG/wear; }
ps_(){ awk -v k="$1" '$1==k{print $2; exit}' $DBG/pagestate; }
setp(){ [ -n "${2:-}" ] || return 0; echo "$2" > $PAR/$1; }
setw(){ echo "$1" > $PAR/erase_high_water; echo "$2" > $PAR/erase_low_water; }

[ -r $DBG/wear ] || { echo "no $DBG/wear -- wrong kernel?"; exit 1; }
lsmod | grep -q nor_eci || insmod "$KO" provide_ops=1 test=0 inline_erase=0 verify_erased=1 \
    || { echo "!! cannot load the backend"; exit 1; }

PB0=$(cat $PAR/promote_batch); D0=$(cat $PAR/wear_days); G0=$(cat $PAR/wear_governor)
HW0=$(cat $PAR/erase_high_water); LW0=$(cat $PAR/erase_low_water)
cleanup(){ pkill -x matmul 2>/dev/null; echo 0 > /sys/kernel/ltram/target_pid 2>/dev/null
           setp promote_batch $PB0; setp wear_days $D0; setp wear_governor $G0; setw $HW0 $LW0; }
trap cleanup EXIT INT TERM

NN=${NN:-7094}; NPAGES=$(( NN * NN * 4 / 4096 )); NLINES=$(( NN * NN * 4 / 128 ))
P1=60; P3=60
CLEAN0=$(ps_ clean)
echo "  $(( NN * NN * 4 / 1048576 )) MiB, $NPAGES pages"
echo "  pool at start: clean $CLEAN0, dirty $(ps_ dirty)"
[ "$(ps_ dirty)" -ge "$NPAGES" ] || { echo "!! only $(ps_ dirty) dirty, need $NPAGES to gate on"; exit 1; }
if [ "$CLEAN0" -gt 2000 ]; then
    echo "  note: the first $CLEAN0 promotions will NOT be gated -- that is"
    echo "        $(awk -v c="$CLEAN0" -v n="$NPAGES" 'BEGIN{printf "%.0f", 100*c/n}')% of the fill, visible as a knee. The rate is taken"
    echo "        from the gated portion only."
fi

setw $HW0 $LW0                        # the engine must run, or nothing recycles
setp promote_batch 1; setp wear_governor 1; setp wear_days 379
L=/tmp/sweep-gated.log
$MM --n $NN --iters 1 --runs 200000 --flush 32 --print-ranges --phys --resid-every 5 > $L 2>&1 &
BG=$!
for i in $(seq 1 900); do grep -q "^TSTART" $L 2>/dev/null && break; sleep 0.1; done
T1=$(date +%s.%N); echo "  phase 1: ${P1}s DRAM"
sleep $P1
T2=$(date +%s.%N)
PID=$(pgrep -x matmul | head -1)
[ -n "${PID:-}" ] && echo $PID > /sys/kernel/ltram/target_pid
# 99.9%, not 99%. At 99% the fill is still creeping -- residency climbed
# 99.17 -> 99.74 through the last run's settled phase -- so any excess over
# the clean-pool value could be argued as leftover migration rather than
# erase interference. Run it out and the argument disappears.
echo "  phase 2: erase-gated fill, waiting for 99.9%"
for i in $(seq 1 20000); do
    R=$(grep "^RESID" $L | tail -1 | awk '{print $4}')
    awk -v r="${R:-0}" 'BEGIN{exit !(r >= 99.9)}' && break
    kill -0 $BG 2>/dev/null || break
    [ $(( i % 120 )) -eq 0 ] && echo "    residency ${R:-?}%  clean $(ps_ clean) dirty $(ps_ dirty)"
    sleep 0.5
done
T3=$(date +%s.%N); echo "  phase 3: ${P3}s settled, engine ON (reached ${R:-?}%)"
sleep $P3

# And then the same thing with the engine pinned off. Fully resident either
# side of this line, so the difference between phase 3 and phase 4 is erase
# interference and nothing else.
setw 0 0
T4=$(date +%s.%N); echo "  phase 4: ${P3}s settled, engine OFF (residency $(grep '^RESID' $L | tail -1 | awk '{print $4}')%)"
sleep $P3
kill $BG 2>/dev/null; wait $BG 2>/dev/null
echo 0 > /sys/kernel/ltram/target_pid

TS=$(awk '/^TSTART/{print $2; exit}' $L)
mkdir -p "$(dirname "$OUT")"
{ echo "elapsed_s,ns_per_line,resid_pct,phase"
  awk -v ts="$TS" -v l="$NLINES" -v a="$T1" -v b="$T2" -v c="$T3" -v d="$T4" '
    NR==FNR { if ($1 == "RESID") rp[$2] = $4; next }
    /^POINT/ { at = ts + $4
               ph = (at<b)?1:((at<c)?2:((at<d)?3:4))
               printf "%.3f,%.1f,%s,%d\n", at - a, $3*1e9/l, ($2 in rp ? rp[$2] : ""), ph }
  ' $L $L
} > "$OUT"
for p in 1 2 3 4; do
  awk -F, -v p=$p 'NR>1 && $4==p {n++; s+=$2} END{if(n) printf "  phase %d: %6.0f ns/line over %5d passes\n", p, s/n, n}' "$OUT"
done
rm -f $L
echo "wrote $OUT ($(( $(wc -l < "$OUT") - 1 )) rows)"
