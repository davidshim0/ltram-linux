#!/bin/bash
# measure_ops.sh -- time the device operations themselves, and their overlap.
#
# Everything we have claims a read stalls for "one erase, 16.4 ms" and a
# promotion "costs a 1.2 ms program". Both numbers come from a datasheet and
# some arithmetic. Neither operation has ever been timed on this board, and
# nothing has measured whether a program and an erase can proceed together.
#
# That last one is not a detail: it decides what happens to promotion when the
# engine is busy. If they serialise, promotion rate is capped by erase time and
# the two costs add. If they overlap, they do not.
#
# The state that answers it is the one where both are happening: a pool short of
# clean sectors, so the engine runs, while promotion consumes what it produces.
set -u
[ "$(id -u)" = 0 ] || exec sudo -E "$0" "$@"
PREFLIGHT=$(dirname "$0")/preflight.sh
[ -x "$PREFLIGHT" ] && { "$PREFLIGHT" --quiet || { echo "!! preflight failed -- run $PREFLIGHT"; exit 1; }; }

DBG=/sys/kernel/debug/ltram
PAR=/sys/module/ltram_policy/parameters
REAL_HOME=$(getent passwd "${SUDO_USER:-$(id -un)}" | cut -d: -f6)
MM=${MM:-$REAL_HOME/matmul}
BT=${BT:-$REAL_HOME/trace_ops.bt}
SECS=${SECS:-120}
PIN=${PIN:-47}
NN=${NN:-2896}
OUT=${OUT:-/var/lib/ltram/selftest/ops.txt}

ps_(){ awk -v k="$1" '$1==k{print $2; exit}' $DBG/pagestate; }
setp(){ echo "$2" > $PAR/$1; }
setw(){ echo "$1" > $PAR/erase_high_water; echo "$2" > $PAR/erase_low_water; }
PB0=$(cat $PAR/promote_batch); D0=$(cat $PAR/wear_days)
HW0=$(cat $PAR/erase_high_water); LW0=$(cat $PAR/erase_low_water)
cleanup(){ pkill -x matmul 2>/dev/null; echo 0 > /sys/kernel/ltram/target_pid 2>/dev/null
           setp promote_batch $PB0; setp wear_days $D0; setw $HW0 $LW0; }
trap cleanup EXIT INT TERM

echo "  pool: clean $(ps_ clean)  dirty $(ps_ dirty)  data $(ps_ data)"
[ "$(ps_ dirty)" -ge 4000 ] || { echo "!! need dirty sectors for the engine to work on"; exit 1; }

# Engine on and NOT starved of work; promotion running so writes happen too.
setw 65536 65535
setp promote_batch 1; setp wear_governor 1; setp wear_days 379

L=/tmp/measure-ops.log
taskset -c $PIN nice -n -20 \
  $MM --n $NN --iters 1 --runs 100000 --chase --print-ranges --phys --resid-every 20 > $L 2>&1 &
BG=$!
for i in $(seq 1 900); do grep -q "^TSTART" $L 2>/dev/null && break; sleep 0.1; done
PID=$(pgrep -x matmul | head -1)
[ -n "${PID:-}" ] && echo $PID > /sys/kernel/ltram/target_pid
echo "  promoting into a short pool for ${SECS}s, tracing both operations"

mkdir -p "$(dirname "$OUT")"
timeout $SECS bpftrace "$BT" > "$OUT" 2>&1
echo 0 > /sys/kernel/ltram/target_pid
kill $BG 2>/dev/null; wait $BG 2>/dev/null

echo
sed -n '/^@/,$p' "$OUT" | head -80
echo
echo "  residency reached: $(grep '^RESID' $L | tail -1 | awk '{print $4}')%"
echo "  wrote $OUT"
rm -f $L
