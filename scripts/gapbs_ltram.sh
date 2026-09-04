#!/bin/bash
# GAPBS on LtRAM, in two conditions that answer different questions.
#
#   sudo ~/gapbs_ltram.sh           both conditions, back to back, on pr
#   sudo ~/gapbs_ltram.sh A pr      selectivity: -f, attach AFTER the load
#   sudo ~/gapbs_ltram.sh B bfs     lifecycle:   -g, attach BEFORE the build
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
COND=${1:-both}; KERN=${2:-pr}
W=${LTRAM_W:-/scratch/hushim/workloads}
S=${SCRIPTS:-/scratch/hushim/ltram/scripts}
OUT=${OUT:-/scratch/hushim/ltram/baselines/$(date +%y%m%d)_gapbs}
DBG=/sys/kernel/debug/ltram; PAR=/sys/module/ltram_policy/parameters
KO=$HOME/nor_eci/nor_eci_fulltest_ltram.ko
SCALE=${SCALE:-22}; TRIALS=${TRIALS:-5000000}; ITERS=${ITERS:-20}
BASE=${BASE:-180}      # DRAM baseline seconds before attaching (condition A)
# Accesses to a CSR page per trial, for the latency back-calculation. MEASURED,
# not assumed: trial time is linear in -i at 0.189 s/iteration up to 5 and then
# flat (-i 5, 10 and 20 all give 0.952 s), so PageRank converges at 5 iterations
# and -i 20 is never reached. Using 20 here understated the implied latency 4x.
ACCESSES=${ACCESSES:-5}
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
SI0=$(cat $PAR/scan_interval_ms)
# A signal handler that only restores state is not enough: bash runs the
# handler and then RESUMES the script, so a ctrl-c or a kill puts the
# parameters back and carries straight on running the workload. Each signal
# trap must exit, and the flag stops the EXIT trap repeating the work.
CLEANED=0
cleanup(){ [ "$CLEANED" = 1 ] && return 0; CLEANED=1
           pkill -x "$KERN" 2>/dev/null
           echo 0 > /sys/kernel/ltram/target_pid 2>/dev/null
           setp promote_batch $PB0; setp wear_days $D0; setp wear_governor $G0
           setp scan_interval_ms $SI0
           setw $HW0 $LW0; }
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap cleanup EXIT

drain(){
    # Between conditions the pool must be clean again. A leaves its pages on
    # LtRAM and their sectors dirty; B starting on a dirty pool would measure
    # erase-gated migration -- fig8's phase 4, a 14x slowdown -- and the waste
    # number would be about erase contention rather than about promotion.
    echo "  draining the pool between conditions (clean $(ps_ clean), dirty $(ps_ dirty))"
    echo 0 > /sys/kernel/ltram/target_pid
    setw 65536 65535
    for i in $(seq 1 3000); do
        [ "$(ps_ dirty)" -eq 0 ] && break
        [ "$(ps_ clean)" -ge 60000 ] && break
        [ $(( i % 120 )) -eq 0 ] && echo "    clean $(ps_ clean), dirty $(ps_ dirty)"
        sleep 1
    done
    setw 8192 2048
    echo "  drained: clean $(ps_ clean), dirty $(ps_ dirty)"
}

run_one(){
    local COND=$1 KERN=$2 BG=""
    local KFLAGS=(); [ "$KERN" = pr ] && KFLAGS=(-i "$ITERS")
    mkdir -p "$OUT"
    local TAG="$COND-$KERN"
    CSV="$OUT/$TAG.csv"; TRI="$OUT/$TAG-trials.csv"; LOG="$OUT/$TAG.log"

    # Migration rate. The shipped default throttles promotion to ~37 pages/s so
    # the flash lasts wear_days; that is a lifetime policy, not a hardware limit.
    # With WEAR_GOVERNOR=0 the interval is scan_interval_ms directly, and since
    # each migration costs a flat ~3 ms of work, interval 0 runs the device at
    # its actual ceiling of ~333 pages/s. Use that when the question is "how
    # does placement behave", not "how long will the part last".
    setp promote_batch "${PROMOTE_BATCH:-1}"
    setp wear_governor "${WEAR_GOVERNOR:-1}"
    setp wear_days     "${WEAR_DAYS:-1826}"
    if [ "${WEAR_GOVERNOR:-1}" = 0 ]; then
        setp scan_interval_ms "${SCAN_INTERVAL_MS:-0}"
        RATE="ungoverned, interval ${SCAN_INTERVAL_MS:-0} ms + ~3 ms/page = ~$(awk -v i="${SCAN_INTERVAL_MS:-0}" 'BEGIN{printf "%.0f", 1000/(i+3)}') pages/s"
    else
        RATE="wear_days ${WEAR_DAYS:-1826}, ~37 pages/s"
    fi
    setw 8192 2048
    echo "  condition $COND, kernel $KERN, scale $SCALE, $RATE"

    A0=$(gs moved_to_ltram); B0=$(gs moved_to_dram); C0=$(wr cycles_used)
    echo "elapsed_s,ltram_pages,total_pages,anon_pages,moved_to_ltram,moved_to_dram,clean,data,dirty" > "$CSV"
    echo "elapsed_s,trial_s,phase" > "$TRI"
    ntr=0
    # Elapsed is measured from the ATTACH instant, so baseline trials carry a
    # negative time and the axis reads straight through the transition.
    # Count only lines that actually carry a time. A workload killed mid-write
    # leaves a partial line, and "Trial Time:" with no number becomes an empty
    # field that crashes the report.
    TRIALRE='/Trial Time/ && NF>=3 && $3+0>0'
    harvest(){
        local n; n=$(awk "$TRIALRE" "$LOG" 2>/dev/null | wc -l)
        if [ "$n" -gt "$ntr" ]; then
            awk "$TRIALRE" "$LOG" | tail -n $(( n - ntr )) \
                | awk -v e="$1" -v p="$2" '{print e","$3","p}' >> "$TRI"
            ntr=$n
        fi
    }

    if [ "$COND" = A ]; then
        [ -s "$GRAPH" ] || { echo "!! no $GRAPH -- build it with build_z08.sh graph"; exit 1; }
        stdbuf -oL -eL "$W/gapbs/$KERN" -f "$GRAPH" -n "$TRIALS" "${KFLAGS[@]}" > "$LOG" 2>&1 &
        BG=$!
        echo "  A: waiting for the load to finish (first Trial Time) before attaching"
        for i in $(seq 1 600); do grep -q "Trial Time" "$LOG" 2>/dev/null && break
                                  kill -0 $BG 2>/dev/null || break; sleep 0.5; done
        # The loop above exits on success AND on death. Tell them apart, or a
        # killed workload is reported as a run of zeros.
        kill -0 $BG 2>/dev/null || {
            echo "!! $KERN (pid $BG) died during the load -- not attaching."
            echo "!! last lines of $LOG:"; tail -5 "$LOG" | sed "s/^/!!   /"
            exit 1; }
        grep -q "Trial Time" "$LOG" 2>/dev/null || {
            echo "!! $KERN produced no trial in 300s -- graph too large, or -n too small."
            exit 1; }
    else
        stdbuf -oL -eL "$W/gapbs/$KERN" -g "$SCALE" -n "$TRIALS" "${KFLAGS[@]}" > "$LOG" 2>&1 &
        BG=$!
        echo "  B: attaching immediately -- the scanner sees the whole build"
        sleep 0.2
    fi
    # ---- phase 1: DRAM baseline. The policy is detached, so these are the
    # same binary on the same graph with every page in DRAM. Without this
    # window the migrating trial times have nothing to be slower *than*.
    # Condition B attaches before the build by definition and has no in-run
    # baseline; A's phase 1 is the reference for both, since once B's CSR is
    # built its trials are the same trials.
    if [ "$COND" = A ] && [ "$BASE" -gt 0 ]; then
        echo 0 > /sys/kernel/ltram/target_pid
        TB=$(date +%s.%N)
        echo "  phase 1: DRAM baseline, ${BASE}s, policy detached"
        while kill -0 $BG 2>/dev/null; do
            eb=$(awk -v a="$TB" -v b="$(date +%s.%N)" 'BEGIN{printf "%.1f", b-a}')
            harvest "-$(awk -v x="$eb" -v m="$BASE" 'BEGIN{printf "%.1f", m-x}')" dram
            awk -v e="$eb" -v m="$BASE" 'BEGIN{exit !(e > m)}' && break
            sleep 2
        done
        echo "  baseline: $ntr trials in DRAM"
    fi

    kill -0 $BG 2>/dev/null || { echo "!! $KERN died before attach, aborting"; exit 1; }
    echo $BG > /sys/kernel/ltram/target_pid
    T0=$(date +%s.%N)
    echo "  attached pid $BG at $(awk -v a="$T0" 'BEGIN{printf "%.1f", 0}')s"

    while kill -0 $BG 2>/dev/null; do
        now=$(date +%s.%N)
        el=$(awk -v a="$T0" -v b="$now" 'BEGIN{printf "%.1f", b-a}')
        read LT TOT AN < <(python3 "$S/ltram_resident.py" $BG 2>/dev/null || echo "0 0 0")
        echo "$el,$LT,$TOT,$AN,$(gs moved_to_ltram),$(gs moved_to_dram),$(ps_ clean),$(ps_ data),$(ps_ dirty)" >> "$CSV"
        harvest "$el" ltram
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
    python3 "$S/gapbs_report.py" "$TRI" "$CSV" "$ACCESSES" >> "$OUT/$TAG-summary.csv" \
        || echo "!! gapbs_report.py failed -- summary has no derived numbers"

    printf '\n  promoted %s   faulted back %s   resident at end %s\n' "$PROM" "$FAULT" "$LT"
    printf '  PROMOTED THEN FREED: %s pages (%s erases, ~%.0f s of erasing)\n' \
           "$WASTE" "$WASTE" "$(awk -v w="$WASTE" 'BEGIN{print w*0.0223}')"
    printf '  trials completed %s\n' "$ntr"
    [ "$ntr" -eq 0 ] && echo "!! NO TRIALS RECORDED -- this summary is meaningless, do not use it"
    awk -F, '$1~/^(dram_median_s|ltram_settled_median_s|slowdown_pct|slowdown_x|implied_ns_per_access|expected_ns_per_access|peak_ltram_pct)$/{printf "  %-24s %s\n",$1,$2}' \
        "$OUT/$TAG-summary.csv"
    printf '  -> %s\n' "$OUT/$TAG-summary.csv"
}

mkdir -p "$OUT"
case "$COND" in
    A|B) run_one "$COND" "$KERN" ;;
    drain) drain ;;
    both|BOTH|"")
        run_one A "$KERN"
        drain
        run_one B "$KERN"
        echo
        echo "  === A against B ==="
        printf "  %-22s %12s %12s\n" metric A B
        for m in promoted faulted_back resident_at_end promoted_then_freed \
                 erase_cycles_used trials_completed; do
            a=$(awk -F, -v k="$m" '$1==k{print $2}' "$OUT/A-$KERN-summary.csv" 2>/dev/null)
            b=$(awk -F, -v k="$m" '$1==k{print $2}' "$OUT/B-$KERN-summary.csv" 2>/dev/null)
            printf "  %-22s %12s %12s\n" "$m" "${a:--}" "${b:--}"
        done
        echo "  -> $OUT"
        ;;
    *) echo "usage: $0 [A|B|both|drain] [pr|bfs]"; exit 2 ;;
esac
