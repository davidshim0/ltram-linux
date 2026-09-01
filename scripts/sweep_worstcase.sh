#!/bin/bash
# sweep_worstcase.sh -- the timeline, but with erases in the picture.
#
# sweep_timeline.sh pre-cleans the pool and pins the engine off, so its second
# phase is promotion into free sectors: the easy case. This is the hard one.
#
#   phase 1  DRAM             nothing targeted
#   phase 2  first migration  into a CLEAN pool, no erase needed
#   phase 3  evicted          every page written, all its sectors now dirty
#   phase 4  second migration every promotion waits for an erase
#   phase 5  flash            settled again
#
# Phase 4 is the point. A promotion can only proceed when a sector has been
# blanked, and the engine retires roughly 40 a second, so the fill is gated on
# erase throughput rather than on the wear budget -- and the reader is paying
# for both at once. That is the worst thing the policy can do to a workload.
set -u
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"

# Refuse to measure on a machine that is not in a state where the result would
# mean anything. Costs milliseconds; has already caught a stale binary, a
# missing nohz_full, and a leftover run from a previous session.
PREFLIGHT=$(dirname "$0")/preflight.sh
if [ -x "$PREFLIGHT" ]; then
    "$PREFLIGHT" --quiet || { echo "!! preflight failed -- run $PREFLIGHT for detail"; exit 1; }
fi
DBG=/sys/kernel/debug/ltram
PAR=/sys/module/ltram_policy/parameters
REAL_HOME=$(getent passwd "${SUDO_USER:-$(id -un)}" | cut -d: -f6)
MM=$REAL_HOME/matmul
KO=$REAL_HOME/nor_eci/nor_eci_fulltest_ltram.ko
OUT=${1:-/var/lib/ltram/selftest/worstcase.csv}

w(){  awk -v k="$1" '$1==k{print $2; exit}' $DBG/wear; }
ps_(){ awk -v k="$1" '$1==k{print $2; exit}' $DBG/pagestate; }
setp(){ [ -n "${2:-}" ] || return 0; echo "$2" > $PAR/$1; }
setw(){ echo "$1" > $PAR/erase_high_water; echo "$2" > $PAR/erase_low_water; }
# Named engine states. "setw $HW0 $LW0" reads like "engine on" but means
# "whatever was there", and an aborted run leaves 0/0 -- so using it as "on"
# turns the engine OFF. That stranded a fill at 99.45% with clean=0.
# The watermarks are on CLEAN, not dirty: clean >= high turns the engine OFF,
# clean < low turns it ON, and between them it holds its last state.
engine_on(){  setw 8192 1024; }     # erase when clean falls under 1024, stop at 8192
engine_max(){ setw 65536 65535; }   # recycle everything, for pool preparation
engine_off(){ setw 0 0; }

[ -r $DBG/wear ] || { echo "no $DBG/wear -- wrong kernel?"; exit 1; }
lsmod | grep -q nor_eci || insmod "$KO" provide_ops=1 test=0 inline_erase=0 verify_erased=1 \
    || { echo "!! cannot load the backend"; exit 1; }

PB0=$(cat $PAR/promote_batch); D0=$(cat $PAR/wear_days); G0=$(cat $PAR/wear_governor)
EP0=$(cat $PAR/erase_poll_ms)
HW0=$(cat $PAR/erase_high_water); LW0=$(cat $PAR/erase_low_water)
cleanup(){ pkill -x matmul 2>/dev/null; echo 0 > /sys/kernel/ltram/target_pid 2>/dev/null
           setp promote_batch $PB0; setp wear_days $D0; setp wear_governor $G0
           setp erase_poll_ms $EP0; setw $HW0 $LW0; }
trap cleanup EXIT INT TERM

NN=${NN:-7094}; NPAGES=$(( NN * NN * 4 / 4096 )); NLINES=$(( NN * NN * 4 / 128 ))
P1=60; P3=45; P5=60
echo "  $(( NN * NN * 4 / 1048576 )) MiB, $NPAGES pages"

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
[ "$(ps_ clean)" -ge "$NPAGES" ] || { echo "!! only $(ps_ clean) clean, need $NPAGES"; exit 1; }

# The engine stays ON at its normal watermarks. Phase 2 needs no erases
# because the pool is clean; phase 4 needs nothing else.
# OFF for phases 1-3. Phase 2 is the writes-only condition: migration programs
# flash and nothing erases, so the read latency in it is the cost of writes
# alone. Leaving the engine at its defaults did not change the physics -- there
# is nothing dirty to erase during phase 2, since migration produces `data`
# pages -- but clean falls to ~0 by the end of phase 2, which trips the engine
# ON to poll for work it will not find. Off means off.
engine_off
setp promote_batch 1; setp wear_governor 1; setp wear_days 379   # ~4 ms

L=/tmp/sweep-worstcase.log
$MM --n $NN --iters 1 --runs 100000 --flush 32 --print-ranges --phys --resid-every 5 > $L 2>&1 &
BG=$!
for i in $(seq 1 900); do grep -q "^TSTART" $L 2>/dev/null && break; sleep 0.1; done
T1=$(date +%s.%N); echo "  phase 1: ${P1}s DRAM"
sleep $P1

T2=$(date +%s.%N)
PID=$(pgrep -x matmul | head -1)
[ -n "${PID:-}" ] && echo $PID > /sys/kernel/ltram/target_pid
echo "  phase 2: first migration into a clean pool"
# 4000 x 0.5 s = 2000 s was not enough: the erase-gated refill runs at
# ~12 pages/s, so 49,145 pages takes about 67 minutes. The cap truncated it at
# 52% and made the settled phase a blend rather than NOR.
waitres(){ for i in $(seq 1 12000); do
    R=$(grep "^RESID" $L | tail -1 | awk '{print $4}')
    awk -v r="${R:-0}" -v t="$1" 'BEGIN{exit !(r >= t)}' && return 0
    kill -0 $BG 2>/dev/null || return 1
    [ $(( i % 60 )) -eq 0 ] && echo "    residency ${R:-?}%  clean $(ps_ clean) dirty $(ps_ dirty)"
    sleep 0.5
  done; return 1; }
waitres 99

T3=$(date +%s.%N); echo "  phase 3: settled, holding ${P3}s then evicting"
sleep $P3
kill -USR1 "$PID" 2>/dev/null
# WAIT FOR THE DROP before waiting for the climb. The last RESID line still
# reads 100% for a moment after the signal, so asking for >=99% straight away
# is satisfied by a sample taken before the eviction -- which is exactly what
# happened: phase 4 came out empty and phase 5 recorded DRAM reads.
for i in $(seq 1 240); do
    R=$(grep "^RESID" $L | tail -1 | awk '{print $4}')
    awk -v r="${R:-100}" 'BEGIN{exit !(r < 10)}' && break
    kill -0 $BG 2>/dev/null || break
    sleep 0.5
done
echo "    evicted: residency $(grep '^RESID' $L | tail -1 | awk '{print $4}')%, dirty $(ps_ dirty)"

# Phase 4 is the writes-AND-erases condition. Turn the engine on only now, with
# the pool already dirty, so the contrast against phase 2 is exactly one thing.
# clean is ~0 here, so it latches on immediately -- no sticky-state problem.
setp erase_poll_ms 30
engine_on
echo "    engine on: clean $(ps_ clean), dirty $(ps_ dirty), poll $(cat $PAR/erase_poll_ms) ms"

T4=$(date +%s.%N); echo "  phase 4: second migration, every promotion gated on an erase"
waitres 99
T5=$(date +%s.%N); echo "  phase 5: ${P5}s settled"
sleep $P5
kill $BG 2>/dev/null; wait $BG 2>/dev/null
echo 0 > /sys/kernel/ltram/target_pid

TS=$(awk '/^TSTART/{print $2; exit}' $L)
mkdir -p "$(dirname "$OUT")"
{ echo "elapsed_s,ns_per_line,resid_pct,phase"
  awk -v ts="$TS" -v l="$NLINES" -v a="$T1" -v b="$T2" -v c="$T3" -v d="$T4" -v e="$T5" '
    NR==FNR { if ($1 == "RESID") rp[$2] = $4; next }
    /^POINT/ { at = ts + $4
               ph = (at<b)?1:((at<c)?2:((at<d)?3:((at<e)?4:5)))
               printf "%.3f,%.1f,%s,%d\n", at - a, $3*1e9/l, ($2 in rp ? rp[$2] : ""), ph }
  ' $L $L
} > "$OUT"
for p in 1 2 3 4 5; do
    awk -F, -v p=$p 'NR>1 && $4==p {n++; s+=$2} END{if(n) printf "  phase %d: %6.0f ns/line over %5d passes\n", p, s/n, n}' "$OUT"
done
rm -f $L
echo "wrote $OUT ($(( $(wc -l < "$OUT") - 1 )) rows)"
