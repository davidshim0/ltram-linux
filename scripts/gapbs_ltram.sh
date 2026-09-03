#!/bin/bash
# GAPBS on LtRAM, in two conditions that answer different questions.
#
#   sudo ~/gapbs_ltram.sh A pr      selectivity: -f, attach AFTER the load
#   sudo ~/gapbs_ltram.sh B pr      lifecycle:   -g, attach BEFORE the build
#
# A -- the clean oracle. The CSR is read-only from a known instant, so
#      placement precision and recall mean something. Use this for any number
#      that gets compared against the matmul figures.
#
# B -- the honest one. Generating a scale-22 Kronecker graph writes a 512 MiB
#      EdgeList, builds the CSR from it (another ~512 MiB of writes), then
#      FREES the EdgeList. The generator fills sequentially, so early EdgeList
#      pages go quiet while later ones are still being written -- and at
#      scan_interval_ms=1000 a page quiet for a few seconds is a promotion
#      candidate. Every such page is migrated to flash, costs a ~22.3 ms erase,
#      and is then freed. That waste is the measurement, not a nuisance.
#
# Neither condition is the baseline for the other. A says what the policy
# achieves; B says what it wastes getting there.
set -u
[ "$(id -u)" = 0 ] || exec sudo -E "$0" "$@"
COND=${1:-A}; KERN=${2:-pr}
W=${LTRAM_W:-/scratch/hushim/workloads}
S=${SCRIPTS:-/scratch/hushim/ltram/scripts}
OUT=${OUT:-/scratch/hushim/ltram/baselines/$(date +%y%m%d)_gapbs}
DBG=/sys/kernel/debug/ltram; PAR=/sys/module/ltram_policy/parameters
KO=$HOME/nor_eci/nor_eci_fulltest_ltram.ko
SCALE=${SCALE:-22}; TRIALS=${TRIALS:-200}; ITERS=${ITERS:-20}
KFLAGS=(); [ "${2:-pr}" = pr ] && KFLAGS=(-i "$ITERS")   # bfs takes no -i
GRAPH="$W/gapbs/benchmark/graphs/kron${SCALE}.sg"

gs(){ awk -v k="$1" '$1==k{print $2; exit}' /sys/kernel/ltram/stats; }
ps_(){ awk -v k="$1" '$1==k{print $2; exit}' $DBG/pagestate; }
wr(){ awk -v k="$1" '$1==k{print $2; exit}' $DBG/wear; }
setp(){ echo "$2" > $PAR/$1; }
setw(){ echo "$1" > $PAR/erase_high_water; echo "$2" > $PAR/erase_low_water; }

[ -r $DBG/wear ] || { echo "!! no $DBG -- wrong kernel?"; exit 1; }
[ -x "$W/gapbs/$KERN" ] || { echo "!! no $W/gapbs/$KERN"; exit 1; }
[ -x "$S/ltram_resident.py" ] || { echo "!! no $S/ltram_resident.py"; exit 1; }
lsmod | grep -q nor_eci || insmod "$KO" provide_ops=1 test=0 inline_erase=0 verify_erased=1 \
    || { echo "!! cannot load the backend"; exit 1; }

PB0=$(cat $PAR/promote_batch); D0=$(cat $PAR/wear_days); G0=$(cat $PAR/wear_governor)
HW0=$(cat $PAR/erase_high_water); LW0=$(cat $PAR/erase_low_water)
cleanup(){ [ -n "${BG:-}" ] && kill $BG 2>/dev/null
           echo 0 > /sys/kernel/ltram/target_pid 2>/dev/null
           setp promote_batch $PB0; setp wear_days $D0; setp wear_governor $G0
           setw $HW0 $LW0; }
trap cleanup EXIT INT TERM

mkdir -p "$OUT"
TAG="$COND-$KERN"
CSV="$OUT/$TAG.csv"; TRI="$OUT/$TAG-trials.csv"; LOG="$OUT/$TAG.log"

# Shipped defaults, deliberately. Condition B is about what the policy does to
# a real application lifecycle, and wear_days=379 is an experimental
# acceleration that would quadruple the waste it is meant to measure.
setp promote_batch 1; setp wear_governor 1; setp wear_days 1826
setw 8192 2048
echo "  condition $COND, kernel $KERN, scale $SCALE, wear_days 1826 (~24 ms, ~37 pages/s)"

A0=$(gs moved_to_ltram); B0=$(gs moved_to_dram); C0=$(wr cycles_used)
echo "elapsed_s,ltram_pages,total_pages,anon_pages,moved_to_ltram,moved_to_dram,clean,data,dirty" > "$CSV"
echo "elapsed_s,trial_s" > "$TRI"

if [ "$COND" = A ]; then
    [ -s "$GRAPH" ] || { echo "!! no $GRAPH -- build it with build_z08.sh graph"; exit 1; }
    "$W/gapbs/$KERN" -f "$GRAPH" -n "$TRIALS" "${KFLAGS[@]}" > "$LOG" 2>&1 &
    BG=$!
    echo "  A: waiting for the load to finish (first Trial Time) before attaching"
    for i in $(seq 1 600); do grep -q "Trial Time" "$LOG" 2>/dev/null && break
                              kill -0 $BG 2>/dev/null || break; sleep 0.5; done
else
    "$W/gapbs/$KERN" -g "$SCALE" -n "$TRIALS" "${KFLAGS[@]}" > "$LOG" 2>&1 &
    BG=$!
    echo "  B: attaching immediately -- the scanner sees the whole build"
    sleep 0.2
fi
echo $BG > /sys/kernel/ltram/target_pid
T0=$(date +%s.%N)
echo "  attached pid $BG at $(awk -v a="$T0" 'BEGIN{printf "%.1f", 0}')s"

ntr=0
while kill -0 $BG 2>/dev/null; do
    now=$(date +%s.%N)
    el=$(awk -v a="$T0" -v b="$now" 'BEGIN{printf "%.1f", b-a}')
    read LT TOT AN < <(python3 "$S/ltram_resident.py" $BG 2>/dev/null || echo "0 0 0")
    echo "$el,$LT,$TOT,$AN,$(gs moved_to_ltram),$(gs moved_to_dram),$(ps_ clean),$(ps_ data),$(ps_ dirty)" >> "$CSV"
    n=$(grep -c "Trial Time" "$LOG" 2>/dev/null || echo 0)
    if [ "$n" -gt "$ntr" ]; then
        grep "Trial Time" "$LOG" | tail -n $(( n - ntr )) | awk -v e="$el" '{print e","$3}' >> "$TRI"
        ntr=$n
    fi
    awk -v e="$el" -v m="${RUNFOR:-1800}" 'BEGIN{exit !(e > m)}' && { echo "  stopping at ${RUNFOR:-1800}s"; break; }
    sleep 2
done
read LT TOT AN < <(python3 "$S/ltram_resident.py" $BG 2>/dev/null || echo "0 0 0")
kill $BG 2>/dev/null; wait $BG 2>/dev/null

A1=$(gs moved_to_ltram); B1=$(gs moved_to_dram); C1=$(wr cycles_used)
PROM=$(( A1 - A0 )); FAULT=$(( B1 - B0 ))
# Promoted pages end up in exactly one of three places: still resident on
# LtRAM, faulted back to DRAM because they were written, or simply freed. The
# third is pure waste -- an erase spent on a page that no longer exists.
WASTE=$(( PROM - FAULT - LT )); [ "$WASTE" -lt 0 ] && WASTE=0
{ echo "condition,$COND"; echo "kernel,$KERN"; echo "promoted,$PROM"
  echo "faulted_back,$FAULT"; echo "resident_at_end,$LT"
  echo "promoted_then_freed,$WASTE"
  echo "erase_cycles_used,$(( C1 - C0 ))"
  echo "trials_completed,$ntr"; } > "$OUT/$TAG-summary.csv"

printf '\n  promoted %s   faulted back %s   resident at end %s\n' "$PROM" "$FAULT" "$LT"
printf '  PROMOTED THEN FREED: %s pages (%s erases, ~%.0f s of erasing)\n' \
       "$WASTE" "$WASTE" "$(awk -v w="$WASTE" 'BEGIN{print w*0.0223}')"
printf '  trials completed %s\n  -> %s\n' "$ntr" "$OUT/$TAG-summary.csv"
