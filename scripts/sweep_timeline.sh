#!/bin/bash
# sweep_timeline.sh -- section N alone: the DRAM -> migrating -> flash timeline.
#
# Lifted from selftest.sh so the two cannot drift. About half an hour, most of
# it recycling enough clean sectors to fill 192 MiB into.
#
# 192 MiB, not 256. The erase engine is pinned off throughout so phase 2
# measures promotion rather than recycling -- and with erases off a fill the
# size of the whole pool stalls near 87%, because every migration that
# programs a sector then fails leaves it dirty with nothing to reclaim it.
set -u
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"
DBG=/sys/kernel/debug/ltram
PAR=/sys/module/ltram_policy/parameters
REAL_HOME=$(getent passwd "${SUDO_USER:-$(id -un)}" | cut -d: -f6)
MM=$REAL_HOME/matmul
KO=$REAL_HOME/nor_eci/nor_eci_fulltest_ltram.ko
OUT=${1:-/var/lib/ltram/selftest/timeline.csv}

w(){  awk -v k="$1" '$1==k{print $2; exit}' $DBG/wear; }
ps_(){ awk -v k="$1" '$1==k{print $2; exit}' $DBG/pagestate; }
setp(){ [ -n "${2:-}" ] || return 0; echo "$2" > $PAR/$1; }
setw(){ echo "$1" > $PAR/erase_high_water; echo "$2" > $PAR/erase_low_water; }

[ -r $DBG/wear ] || { echo "no $DBG/wear -- wrong kernel?"; exit 1; }
[ -x "$MM" ] || { echo "no matmul at $MM"; exit 1; }
lsmod | grep -q nor_eci || insmod "$KO" provide_ops=1 test=0 inline_erase=0 verify_erased=1 \
    || { echo "!! cannot load the backend"; exit 1; }

PB0=$(cat $PAR/promote_batch); D0=$(cat $PAR/wear_days); G0=$(cat $PAR/wear_governor)
HW0=$(cat $PAR/erase_high_water); LW0=$(cat $PAR/erase_low_water)
cleanup(){ pkill -x matmul 2>/dev/null; echo 0 > /sys/kernel/ltram/target_pid 2>/dev/null
           setp promote_batch $PB0; setp wear_days $D0; setp wear_governor $G0; setw $HW0 $LW0; }
trap cleanup EXIT INT TERM

NN=7094; NPAGES=$(( NN * NN * 4 / 4096 )); NLINES=$(( NN * NN * 4 / 128 ))
PHASE1=45; PHASE3=90
if [ "$(ps_ clean)" -lt $NPAGES ]; then
    echo "  recycling: clean $(ps_ clean) -> $NPAGES (dirty $(ps_ dirty))"
    setw 65536 65535
    for i in $(seq 1 2400); do
        [ "$(ps_ clean)" -ge "$NPAGES" ] && break
        [ "$(ps_ dirty)" -eq 0 ] && break
        [ $(( i % 120 )) -eq 0 ] && echo "    clean $(ps_ clean), dirty $(ps_ dirty)"
        sleep 1
    done
fi
setw 0 0
[ "$(ps_ clean)" -ge "$NPAGES" ] || { echo "!! only $(ps_ clean) clean, need $NPAGES"; exit 1; }

setp promote_batch 1; setp wear_governor 1; setp wear_days 379    # ~4 ms
L=/tmp/sweep-timeline.log
$MM --n $NN --iters 1 --runs 100000 --flush 32 --verify --print-ranges --phys \
    --resid-every 5 > $L 2>&1 &
BG=$!
for i in $(seq 1 900); do grep -q "^TSTART" $L 2>/dev/null && break; sleep 0.1; done
T_P1=$(date +%s.%N)
echo "  phase 1: ${PHASE1}s of DRAM reads"
sleep $PHASE1
T_P2=$(date +%s.%N)
PID=$(pgrep -x matmul | head -1)
[ -n "${PID:-}" ] && echo $PID > /sys/kernel/ltram/target_pid
echo "  phase 2: migrating $NPAGES pages, waiting for 99%"
# 99%, not 100%: the last handful of pages can take longer than the whole
# transition and the medium has already changed by then.
for i in $(seq 1 3000); do
    R=$(grep "^RESID" $L | tail -1 | awk '{print $4}')
    awk -v r="${R:-0}" 'BEGIN{exit !(r >= 99)}' && break
    kill -0 $BG 2>/dev/null || break
    [ $(( i % 60 )) -eq 0 ] && echo "    residency ${R:-?}%"
    sleep 0.5
done
T_P3=$(date +%s.%N)
echo "  phase 3: ${PHASE3}s of sustained flash reads (reached ${R:-?}%)"
sleep $PHASE3
kill $BG 2>/dev/null; wait $BG 2>/dev/null
echo 0 > /sys/kernel/ltram/target_pid

TS=$(awk '/^TSTART/{print $2; exit}' $L)
mkdir -p "$(dirname "$OUT")"
{ echo "elapsed_s,ns_per_line,resid_pct,phase"
  awk -v ts="$TS" -v l="$NLINES" -v p1="$T_P1" -v p2="$T_P2" -v p3="$T_P3" '
    NR==FNR { if ($1 == "RESID") rp[$2] = $4; next }
    /^POINT/ { at = ts + $4
               ph = (at < p2) ? 1 : ((at < p3) ? 2 : 3)
               printf "%.3f,%.1f,%s,%d\n", at - p1, $3*1e9/l, ($2 in rp ? rp[$2] : ""), ph }
  ' $L $L
} > "$OUT"
for p in 1 2 3; do
    awk -F, -v p=$p 'NR>1 && $4==p {n++; s+=$2} END{if(n) printf "  phase %d: %6.0f ns/line over %d passes\n", p, s/n, n}' p=$p "$OUT"
done
rm -f $L
echo "wrote $OUT ($(( $(wc -l < "$OUT") - 1 )) rows)"
