#!/bin/bash
# sweep_qos.sh -- per-READ latency distribution, with and without erasing.
#
# Every other measurement in this project is a pass average. At 192 MiB a pass
# is 1.5M cache lines, so a 16.4 ms erase stall is one percent of one sample:
# the average moves 7% and the individual read that waited is invisible. That
# is precisely the number a QoS-minded reader asks about.
#
# --chase issues one dependent load per cache line, so accesses are serialised
# and each can be timed on its own. --chase-hist buckets them by log2 ns and
# prints a histogram per pass.
#
#   phase A  engine off   the medium alone
#   phase B  engine on    the medium plus background recycling
#
# Same resident data either side, so the difference in the tail is erasing.
set -u
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"
DBG=/sys/kernel/debug/ltram
PAR=/sys/module/ltram_policy/parameters
REAL_HOME=$(getent passwd "${SUDO_USER:-$(id -un)}" | cut -d: -f6)
MM=$REAL_HOME/matmul
KO=$REAL_HOME/nor_eci/nor_eci_fulltest_ltram.ko
OUT=${1:-/var/lib/ltram/selftest/qos.csv}

w(){  awk -v k="$1" '$1==k{print $2; exit}' $DBG/wear; }
ps_(){ awk -v k="$1" '$1==k{print $2; exit}' $DBG/pagestate; }
setp(){ [ -n "${2:-}" ] || return 0; echo "$2" > $PAR/$1; }
setw(){ echo "$1" > $PAR/erase_high_water; echo "$2" > $PAR/erase_low_water; }
# Named engine states. "setw $HW0 $LW0" reads like "engine on" but means
# "whatever was there", and an aborted run leaves 0/0 -- so restoring it as if
# it meant on turns the engine OFF. That cost a run.
engine_on(){  setw 8192 2048; }     # ltram_policy defaults: normal operating point
engine_max(){ setw 65536 65535; }   # recycle everything, for pool preparation
engine_off(){ setw 0 0; }

[ -r $DBG/wear ] || { echo "no $DBG/wear"; exit 1; }
lsmod | grep -q nor_eci || insmod "$KO" provide_ops=1 test=0 inline_erase=0 verify_erased=1 || exit 1
PB0=$(cat $PAR/promote_batch); D0=$(cat $PAR/wear_days); G0=$(cat $PAR/wear_governor)
HW0=$(cat $PAR/erase_high_water); LW0=$(cat $PAR/erase_low_water)
cleanup(){ pkill -x matmul 2>/dev/null; echo 0 > /sys/kernel/ltram/target_pid 2>/dev/null
           setp promote_batch $PB0; setp wear_days $D0; setp wear_governor $G0; setw $HW0 $LW0; }
trap cleanup EXIT INT TERM

NN=${NN:-2896}; NPAGES=$(( NN * NN * 4 / 4096 ))
HOLD=${HOLD:-90}
echo "  $(( NN * NN * 4 / 1048576 )) MiB, $NPAGES pages, ${HOLD}s per condition"

if [ "$(ps_ clean)" -lt $NPAGES ]; then
    echo "  recycling: clean $(ps_ clean) -> $NPAGES"
    engine_max
    for i in $(seq 1 2400); do
        [ "$(ps_ clean)" -ge "$NPAGES" ] && break
        [ "$(ps_ dirty)" -eq 0 ] && break
        sleep 1
    done
fi
[ "$(ps_ clean)" -ge "$NPAGES" ] || { echo "!! only $(ps_ clean) clean"; exit 1; }
# The engine RUNS during the fill. Sizing the clean pool exactly to the
# weights and pinning the engine off leaves no slack: the process touches more
# than W -- the 32 MiB scrub buffer is in the same address space and is a
# promotion candidate too -- those pages eat clean sectors, and the fill
# strands a few dozen pages short with no way to make more. It stalled at
# 99.45% with clean=0 for exactly that reason. Phases A and B set the
# watermarks explicitly, so the fill state cannot leak into the measurement.
engine_on
[ "$(ps_ dirty)" -ge 4000 ] || echo "  warning: only $(ps_ dirty) dirty, phase B may have little to erase"

setp promote_batch 1; setp wear_governor 1; setp wear_days 379
L=/tmp/sweep-qos.log
$MM --n $NN --iters 1 --runs 100000 --chase --chase-hist --print-ranges --phys \
    --resid-every 5 > $L 2>&1 &
BG=$!
for i in $(seq 1 900); do grep -q "^TSTART" $L 2>/dev/null && break; sleep 0.1; done
PID=$(pgrep -x matmul | head -1)
[ -n "${PID:-}" ] && echo $PID > /sys/kernel/ltram/target_pid
echo "  filling..."
for i in $(seq 1 20000); do
    R=$(grep "^RESID" $L | tail -1 | awk '{print $4}')
    awk -v r="${R:-0}" 'BEGIN{exit !(r >= 99.9)}' && break
    kill -0 $BG 2>/dev/null || break
    # A missing RESID line is indistinguishable from a slow fill, and the
    # difference is 2 minutes against 2.8 hours of spinning. --chase skipped
    # the RESID emission entirely once; fail loudly rather than wait it out.
    if [ "$i" -eq 120 ] && [ "$(grep -c '^RESID' $L)" -eq 0 ]; then
        echo "!! no RESID line after 60s -- matmul is not reporting residency."
        echo "   $(grep -c '^POINT' $L) passes done, so the run itself is alive."
        kill $BG 2>/dev/null; exit 1
    fi
    # Flat is as fatal as missing: with no clean sectors and no engine the
    # last pages can never promote, and this loop would spin for 2.8 hours.
    if [ "${R:-0}" = "${RPREV:-}" ]; then STUCK=$(( ${STUCK:-0} + 1 ))
    else STUCK=0; RPREV=${R:-0}; fi
    if [ "${STUCK:-0}" -ge 360 ]; then
        echo "!! residency stuck at ${R:-?}% for 180s (clean $(ps_ clean),"
        echo "   engine high=$(cat $PAR/erase_high_water) low=$(cat $PAR/erase_low_water))"
        kill $BG 2>/dev/null; exit 1
    fi
    [ $(( i % 120 )) -eq 0 ] && echo "    residency ${R:-?}%  clean $(ps_ clean)"
    sleep 0.5
done
echo 0 > /sys/kernel/ltram/target_pid       # stop promoting: isolate the medium
sleep 2
echo "  phase A: ${HOLD}s, engine OFF (residency ${R:-?}%)"
engine_off
A0=$(grep -c "^HIST" $L); sleep $HOLD; A1=$(grep -c "^HIST" $L)
echo "  phase B: ${HOLD}s, engine ON (dirty $(ps_ dirty))"
engine_max
EC0=$(w cycles_used)
B0=$(grep -c "^HIST" $L); sleep $HOLD; B1=$(grep -c "^HIST" $L)
EC1=$(w cycles_used)
engine_off
# Measured, not assumed: the figure should say what the engine actually did
# rather than claim a budget it might not have hit.
ERPS=$(awk -v a="${EC0:-0}" -v b="${EC1:-0}" -v t="$HOLD" 'BEGIN{printf "%.1f", (b-a)/t}')
echo "  phase B erased $(( ${EC1:-0} - ${EC0:-0} )) sectors in ${HOLD}s = ${ERPS}/s"
kill $BG 2>/dev/null; wait $BG 2>/dev/null

mkdir -p "$(dirname "$OUT")"
{ echo "condition,bucket_lo_ns,bucket_hi_ns,count"
  awk -v a0="$A0" -v a1="$A1" -v b0="$B0" -v b1="$B1" '
    /^HIST/ { n++
      cond = (n > a0 && n <= a1) ? "engine_off" : ((n > b0 && n <= b1) ? "engine_on" : "")
      if (cond == "") next
      for (b = 0; b < 28; b++) tot[cond "," b] += $(4 + b)
    }
    END { for (k in tot) { split(k, p, ",")
            lo = (p[2] == 0) ? 0 : 2 ^ (p[2] - 1)
            printf "%s,%d,%d,%d\n", p[1], lo, 2 ^ p[2] - 1, tot[k] } }
  ' $L
} > "$OUT"
echo
# Percentiles as the bucket's upper edge, so each reads as "no slower than".
pq(){ awk -F, -v c="$1" -v q="$2" 'NR>1 && $1==c && $4>0 {v[$3]=$4; n+=$4}
      END{ m=asorti(v, ix, "@ind_num_asc"); for(i=1;i<=m;i++){s+=v[ix[i]]
             if(s >= q*n){print ix[i]; exit}} print (m?ix[m]:0) }' "$OUT"; }
pn(){ awk -F, -v c="$1" 'NR>1 && $1==c {n+=$4} END{print n+0}' "$OUT"; }
printf "  %-11s %9s %9s %10s %10s %11s %12s\n" "" p50 p99 p99.9 p99.99 max reads
for C in engine_off engine_on; do
    printf "  %-11s %9s %9s %10s %10s %11s %12s\n" "$C" \
        "$(pq $C 0.5)" "$(pq $C 0.99)" "$(pq $C 0.999)" \
        "$(pq $C 0.9999)" "$(pq $C 1.0)" "$(pn $C)"
done
echo "  (ns; one erase is 16,400,000 ns)"
rm -f $L
echo "wrote $OUT"
