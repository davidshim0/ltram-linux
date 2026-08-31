#!/bin/bash
# probe_engine_off.sh -- why is the IDLE flash condition worse than the busy one?
#
# From the last sweep, at 99.999% of reads:
#     engine_off     18.4 us   (nothing erasing, nothing promoting)
#     engine_spaced   1.7 us   (actively erasing)
# and engine_off held the only 4 events above 100 us in the whole run, on a
# measurement floor of 2.3 us. With no writes and no erases there is no
# mechanism for the idle case to be slower. Something else is going on.
#
# This makes NO hypothesis about what. It runs the SAME condition several times
# at different positions in the sequence, and instruments every phase
# identically. Then:
#
#   anomaly in the first OFF only        -> it is position: residue from the fill
#   anomaly in every OFF equally         -> it is the condition itself
#   anomaly wanders                      -> it is neither; something external
#   anomaly gone                         -> it was transient, and we now know
#
# Everything is recorded per phase -- every interrupt line, context switches,
# per-event stalls with timestamps, and the pool state -- so whatever separates
# the phases is visible rather than guessed at.
set -u
[ "$(id -u)" = 0 ] || exec sudo -E "$0" "$@"
# /scratch is root-owned on NFS, so a new top-level directory cannot be created
# there under root_squash -- but an existing per-user one can be written. Try the
# invoking user, then any existing writable directory, then give up to /tmp.
scratch_dir(){
    local d
    for d in "${SCRATCH:-}" "/scratch/${SUDO_USER:-$(id -un)}/ltram" \
             $(ls -d /scratch/*/ 2>/dev/null | head -20 | sed 's|$|ltram|'); do
        [ -z "$d" ] && continue
        mkdir -p "$d" 2>/dev/null && [ -w "$d" ] && { echo "$d"; return 0; }
    done
    echo /tmp
}
SCRATCH=$(scratch_dir)
PREFLIGHT=$(dirname "$0")/preflight.sh
[ -x "$PREFLIGHT" ] && { "$PREFLIGHT" --quiet || { echo "!! preflight failed"; exit 1; }; }

DBG=/sys/kernel/debug/ltram; PAR=/sys/module/ltram_policy/parameters
REAL_HOME=$(getent passwd "${SUDO_USER:-$(id -un)}" | cut -d: -f6)
MM=${MM:-$REAL_HOME/matmul}
PIN=${PIN:-47}; NN=${NN:-2896}; HOLD=${HOLD:-60}
SETTLE=${SETTLE:-0}          # seconds of quiet between the fill and the first phase
OUT=${OUT:-$SCRATCH/engineoff}
mkdir -p "$OUT"

ps_(){ awk -v k="$1" '$1==k{print $2; exit}' $DBG/pagestate; }
setp(){ echo "$2" > $PAR/$1; }
setw(){ echo "$1" > $PAR/erase_high_water; echo "$2" > $PAR/erase_low_water; }
engine_off(){ setw 0 0; }
engine_on(){  setw 65536 65535; sleep 0.3; setw 8192 2048; }
irqsnap(){ awk -v c="CPU$PIN" 'NR==1{for(i=1;i<=NF;i++) if($i==c) k=i+1; next}
                               k&&NF>=k{n=$1; sub(/:$/,"",n); print n, $k}' /proc/interrupts; }

PB0=$(cat $PAR/promote_batch); D0=$(cat $PAR/wear_days)
HW0=$(cat $PAR/erase_high_water); LW0=$(cat $PAR/erase_low_water)
cleanup(){ pkill -x matmul 2>/dev/null; echo 0 > /sys/kernel/ltram/target_pid 2>/dev/null
           setp promote_batch $PB0; setp wear_days $D0; setw $HW0 $LW0; }
trap cleanup EXIT INT TERM

NPAGES=$(( NN * NN * 4 / 4096 ))
if [ "$(ps_ clean)" -lt $NPAGES ]; then
    echo "  recycling to $NPAGES clean"; setw 65536 65535
    for i in $(seq 1 2400); do [ "$(ps_ clean)" -ge $NPAGES ] && break
        [ "$(ps_ dirty)" -eq 0 ] && break; sleep 1; done
fi
engine_on
setp promote_batch 1; setp wear_governor 1; setp wear_days 379

L=$SCRATCH/engineoff.log
taskset -c $PIN nice -n -20 $MM --n $NN --iters 1 --runs 100000 --chase --chase-hist \
    --slow-ns 5000 --print-ranges --phys --resid-every 20 > $L 2>&1 &
BG=$!
for i in $(seq 1 900); do grep -q "^TSTART" $L 2>/dev/null && break; sleep 0.1; done
PID=$(pgrep -x matmul | head -1); [ -n "${PID:-}" ] && echo $PID > /sys/kernel/ltram/target_pid
echo "  filling..."
for i in $(seq 1 6000); do
    R=$(grep "^RESID" $L | tail -1 | awk '{print $4}'); R=${R:-0}
    awk -v r="$R" 'BEGIN{exit !(r >= 99.0)}' && break
    kill -0 $BG 2>/dev/null || break; sleep 0.5
done
echo 0 > /sys/kernel/ltram/target_pid
echo "  filled to ${R}%; settling ${SETTLE}s"
[ "$SETTLE" -gt 0 ] && sleep "$SETTLE"

: > $OUT/marks.txt
phase(){   # $1 = label
    local h0 h1 c0 c1 iv
    irqsnap > /tmp/i0.$$
    c0=$(awk '/^ctxt/{print $2}' /proc/stat); h0=$(grep -c "^HIST" $L)
    sleep $HOLD
    h1=$(grep -c "^HIST" $L); c1=$(awk '/^ctxt/{print $2}' /proc/stat)
    irqsnap > /tmp/i1.$$
    iv=$(awk -v a="$h0" -v b="$h1" '/^CTX/{n++; if(n>a && n<=b) s+=$4} END{print s+0}' $L)
    echo "$1 $h0 $h1" >> $OUT/marks.txt
    printf "  %-14s passes %4d  ctxt(system) %8d  involuntary %4d  clean %6s dirty %6s\n" \
        "$1" "$(( h1 - h0 ))" "$(( c1 - c0 ))" "$iv" "$(ps_ clean)" "$(ps_ dirty)"
    join -j1 /tmp/i0.$$ /tmp/i1.$$ | awk -v t="$HOLD" -v p="$1" \
        '{d=$3-$2; if(d>0) printf "    irq %-10s %8d %7.1f/s\n", $1, d, d/t}' | sort -k3 -nr | head -4
    join -j1 /tmp/i0.$$ /tmp/i1.$$ | awk -v p="$1" '{d=$3-$2; if(d>0) print p, $1, d}' >> $OUT/irq.txt
    rm -f /tmp/i0.$$ /tmp/i1.$$
}

echo "  === same condition, four positions ==="
engine_off; phase off_1
engine_on;  phase erasing_1
engine_off; phase off_2
engine_on;  phase erasing_2
engine_off; phase off_3
engine_off; phase off_4          # two in a row: does it depend on what preceded?

kill $BG 2>/dev/null; wait $BG 2>/dev/null
grep "^SLOW" $L > $OUT/slow.txt; grep "^SECT" $L > $OUT/sect.txt
cp $L $OUT/full.log
echo
echo "  === stalls per phase, from the per-event records ==="
awk 'NR==FNR{lo[$1]=$2; hi[$1]=$3; next}
     /^SLOW/{ for(p in lo) if ($2>lo[p] && $2<=hi[p]) { n[p]++; if ($5+0>mx[p]) mx[p]=$5+0 } }
     END{ for (p in lo) printf "  %-14s %6d stalls >5us   worst %10.1f us\n", p, n[p]+0, mx[p]/1000 }' \
     $OUT/marks.txt $OUT/slow.txt | sort
echo
echo "  wrote $OUT/{slow,sect,irq,marks}.txt and full.log"
rm -f $L
