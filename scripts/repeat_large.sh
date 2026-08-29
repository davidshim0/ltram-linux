#!/bin/bash
# repeat_large.sh -- is the NOR per-line jump above 64 MB real, or an artefact?
#
# The sweep measured NOR at 932-980 ns/line from 32 KB to 64 MB and then 1216
# at 128 MB and 1261 at 256 MB, while DRAM held 197-216 across the whole range.
# This repeats the three sizes either side of that step under conditions the
# sweep did not control.
#
# TWO THINGS THIS CONTROLS THAT THE SWEEP DID NOT
#
#   1. The pool starts FULL every time. The sweep drained only when clean fell
#      below 40,000, so a run could begin depleted, and a depleted pool is a
#      different experiment.
#
#   2. The erase engine is pinned OFF during every measurement. At 256 MB the
#      run promotes all 65,536 sectors, so clean reaches 0, crosses the low
#      watermark, and the engine turns on WHILE the workload reads. NOR cannot
#      serve a read mid-erase, so that would inflate NOR and leave DRAM alone,
#      which is the shape we are chasing. erase_high_water=0 makes the latch's
#      "off" test true unconditionally.
#
#      Note this only bites when DIRTY sectors are lying around: promotion
#      moves clean->data, never clean->dirty, so a run on a fully drained pool
#      gives the engine nothing to erase no matter where clean lands. The two
#      conditions are therefore the same condition, and the sweep had both --
#      it drained only below 40,000, so the large sizes ran with both a
#      depleted pool and a backlog of dirty sectors to chew through.
#
# Cost is dominated by erase and is not reducible: every promoted page must be
# blanked at ~16.4 ms. Ten repetitions of all three sizes promotes 1.15M pages,
# so about 5 hours, most of it draining.
#
#   ./repeat_large.sh [--reps 10] [--out FILE]
set -u
REPS=10; OUT=/scratch/hushim/large.csv; SIZES="4096 5793 8192"
WAIT=99.5; DO_DRAM=1; MODES="nor_cold"; WARM_ENGINE=0
while [ $# -gt 0 ]; do case "$1" in
  --reps)        REPS=$2; shift;;
  --out)         OUT=$2; shift;;
  --sizes)       SIZES=$2; shift;;
  --wait)        WAIT=$2; shift;;
  --modes)       MODES=$2; shift;;
  --warm-engine) WARM_ENGINE=1;;
  --nor-only)    DO_DRAM=0;;
  *) echo "usage: $0 [--reps N] [--out FILE] [--sizes \"N N N\"] [--wait PCT] [--nor-only] [--modes \"...\"] [--warm-engine]"; exit 2;; esac; shift; done

MM=$HOME/matmul; KO=$HOME/nor_eci/nor_eci_fulltest_ltram.ko
S=/sys/kernel/ltram/stats; P=/sys/kernel/debug/ltram/pagestate
TP=/sys/kernel/ltram/target_pid; PB=/sys/module/ltram_policy/parameters/promote_batch
HW=/sys/module/ltram_policy/parameters/erase_high_water
PAR_WG=/sys/module/ltram_policy/parameters/wear_governor
LW=/sys/module/ltram_policy/parameters/erase_low_water
say(){ echo "[$(date +%H:%M:%S)] $*"; }
restore_gov(){ [ -w $PAR_WG ] && echo 1 | sudo -n tee $PAR_WG >/dev/null; return 0; }
trap restore_gov EXIT
# Checkpoint the erase counts around every point. They only move during a
# drain, so this is where the interesting deltas happen. Silent no-op if the
# unit is not installed.
EC=/usr/local/sbin/ltram-erase-counts
ec_save(){ [ -x $EC ] && sudo -n $EC save >/dev/null 2>&1; return 0; }
SER=/scratch/hushim/series; mkdir -p $SER
# Keep the per-pass series. The mean alone cannot tell a run that is still
# decaying from one that is bimodal, and at 128 and 256 MB the NOR sd is 17%
# against 0.10% at 64 MB, so the mean there is an average over something that
# is still moving rather than a latency. 120 numbers per point.
series(){ grep "^POINT" "$1" | awk '{print NR, $3}' > "$SER/$2.txt"; }
ps_(){ sudo -n cat $P; }
g(){ awk -v k="$2" '$1==k{print $2; exit}' <<<"$1"; }
gs(){ awk -v k="$1" '$1==k{print $2; exit}' $S; }
plateau(){ grep "^POINT" "$1" | awk '{v[n++]=$3} END{
    if(!n){print "nan nan"; exit} s=int(n*0.7); c=0; t=0
    for(i=s;i<n;i++){c++; t+=v[i]} m=t/c; q=0
    for(i=s;i<n;i++) q+=(v[i]-m)^2
    printf "%.6f %.6f\n", m, (c>1?sqrt(q/(c-1)):0) }'; }

drain(){                      # no pid attached, so the engine runs at ~61/s
    echo 65536 | sudo -n tee $HW >/dev/null; echo 65535 | sudo -n tee $LW >/dev/null
    for i in $(seq 1 2400); do
        [ "$(g "$(ps_)" dirty)" = "0" ] && break
        [ $((i % 120)) -eq 0 ] && say "    draining, dirty=$(g "$(ps_)" dirty)"
        sleep 1
    done
}
engine_off(){ echo 0 | sudo -n tee $HW >/dev/null; echo 0 | sudo -n tee $LW >/dev/null; }
setw(){ echo "$1" | sudo -n tee $HW >/dev/null; echo "$2" | sudo -n tee $LW >/dev/null; }

lsmod | grep -q nor_eci || sudo -n insmod $KO provide_ops=1 test=0 \
    inline_erase=0 verify_erased=1 || exit 4
[ -f "$OUT" ] || echo "rep,n,mode,bytes,pages,mean_s,sd_s,ns_per_line,resident,clean_before,erases_during,dirty_before" > "$OUT"

for rep in $(seq 1 $REPS); do
for N in $SIZES; do
    BYTES=$((N*N*4)); PAGES=$((BYTES/4096)); NLINES=$((BYTES/128))   # NOT "LINES": bash resets that to the terminal height
    # sweep.sh's own rule, so the plateau is taken over the same sample and
    # these numbers can be put beside the ones in the figures.
    if [ $PAGES -le 32768 ]; then R=200; else R=120; fi
    say "===== rep $rep/$REPS  N=$N  $((BYTES/1048576)) MiB  $PAGES pages  runs=$R ====="

    NEEDS_POOL=0
    for M_ in $MODES; do case $M_ in nor_*) NEEDS_POOL=1;; esac; done

    if [ "$NEEDS_POOL" = 0 ]; then
        # DRAM only: no promotion, so no pool, no drain, no engine, no
        # target_pid. Draining 65,536 sectors for a run that never touches
        # flash is 18 minutes bought for nothing.
        CB=0; DB=0
        say "  DRAM only -- skipping the drain"
    else
    ec_save
    say "  draining to a full pool"
    # Shake out the per-CPU lru_add folio batches FIRST. Pages the previous
    # run released sit there with ACTIVE set and LRU clear, so they are never
    # freed, never become dirty, and the drain loop below cannot see them --
    # they just sit in DATA and the pool comes up short. Same thing that
    # produced four false "stuck page" diagnoses. compact_memory reaches
    # lru_add_drain_all().
    echo 1 | sudo -n tee /proc/sys/vm/compact_memory >/dev/null 2>&1 || true
    sleep 2
    drain
    echo 1 | sudo -n tee /proc/sys/vm/compact_memory >/dev/null 2>&1 || true
    sleep 2
    drain
    PS0=$(ps_); CB=$(g "$PS0" clean); DB=$(g "$PS0" dirty)
    if [ "${CB:-0}" -lt 65536 ]; then
        say "  !! clean=$CB, not 65536 -- something is still holding sectors"
    fi
    engine_off
    sleep 2                   # let the latch see the new watermark
    say "  pool clean=$CB dirty=$DB, erase engine pinned off"
    # promote_batch=512 with the WEAR GOVERNOR ON would be 512 promotions per
    # 24 ms tick -- 21,000/s against a 41.5/s budget. The governor exists to
    # pace the policy in service; these runs measure the MEDIUM, and a fill
    # paced at 41.5/s would take 26 minutes at 256 MB. So turn it off
    # explicitly and say so, rather than letting the two disagree silently.
    echo 512 | sudo -n tee $PB >/dev/null
    if [ -w $PAR_WG ]; then
        echo 0 | sudo -n tee $PAR_WG >/dev/null
        say "  wear governor OFF for measurement (fill rate, not service rate)"
    fi
    fi

    for MODE in $MODES; do
    case $MODE in *_warm) FL="";; *) FL="--flush 32";; esac

    if [ "${MODE#nor}" = "$MODE" ]; then
        # Anything that does not touch flash: no pool, no target_pid, no fill.
        # comp is the compute floor -- every row pinned to row 0 so the inner
        # loop stays in L1D. It computes the wrong answer on purpose, so it
        # refuses --verify.
        case $MODE in
          comp) EXTRA="--compute-only";;
          *)    EXTRA="--verify";;
        esac
        L=/tmp/rl-$MODE.log
        sudo -n $MM --n $N --iters 1 --runs $R $FL $EXTRA > $L 2>&1
        read M SD < <(plateau $L); series $L "r${rep}-N${N}-${MODE}"
        NSL=$(awk -v m="$M" -v l="$NLINES" 'BEGIN{printf "%.1f", m*1e9/l}')
        PCT=$(awk -v m="$M" -v v="$SD" 'BEGIN{printf "%.2f", (m>0?100*v/m:0)}')
        echo "$rep,$N,$MODE,$BYTES,$PAGES,$M,$SD,$NSL,0,$CB,0,$DB" >> "$OUT"
        say "  $MODE  $M s   $NSL ns/line   sd ${PCT}%"
        rm -f $L
        continue
    fi

    # Re-drain between modes: the first mode leaves the pool spent, and a
    # depleted pool is what produced every wrong number in this experiment.
    if [ "$MODE" != "${MODES%% *}" ]; then
        echo 1 | sudo -n tee /proc/sys/vm/compact_memory >/dev/null 2>&1 || true
        sleep 2; drain; engine_off; sleep 2
        say "  redrained for $MODE, clean=$(g "$(ps_)" clean)"
    fi

    HOLD=0
    if [ "$WARM_ENGINE" = 1 ]; then
        # Let the engine RUN during the fill, and only during the fill.
        #
        # Insurance, not a fix. The 87.5% plateau turned out to be the SCRUB
        # BUFFER: the fill did not call cache_scrub(), so 32 MiB of anonymous
        # read-mostly memory sat below the weights looking like the best
        # promotion candidate in the process, got promoted, and was then
        # written by the first timed scrub -- leaving 8,192 sectors dirty.
        # Fixed in matmul (5e897b780), where the fill now scrubs too.
        #
        # NOTHING EVER FAILED TO MIGRATE: dst_released is 0 for the whole
        # boot. The earlier "recycles failed migrations" reading was wrong.
        #
        # Leaving the engine on for the fill is still worth it: if anything
        # does go dirty while filling, it comes back rather than shrinking the
        # pool. Recycling during the TIMED run is the thing that must not
        # happen, since NOR cannot serve a read mid-erase -- hence the hold
        # window that pins it off before the clock starts.
        #
        # So: engine on now, matmul holds still for 8 s after the fill
        # reports done, and the engine goes off inside that window. STABLE is
        # raised because progress near the end arrives in fits as sectors
        # cycle back, and 20 quiet passes would call that a plateau.
        setw 8192 2048   # the normal watermarks: engine works, hysteresis intact
        HOLD=8; STABLE=60
        say "  erase engine LEFT ON for the fill (off again before the clock starts)"
    else
        STABLE=20
    fi

    E0=$(gs erases_done)
    L=/tmp/rl-nor.log
    sudo -n $MM --n $N --iters 1 --runs $R $FL --verify --print-ranges --phys \
        --wait-resident $WAIT --wait-timeout 1800 --wait-stable $STABLE \
        --wait-hold $HOLD > $L 2>&1 &
    BG=$!
    # Attach target_pid BEFORE the warmup gets going. matmul starts its
    # untimed fill immediately after printing RANGE, so a 1 s poll loses the
    # race and the fill sees nothing targeting it. 0.1 s, and matmul now
    # refuses to call 0% a plateau, so both ends are covered.
    for i in $(seq 1 1800); do grep -q "^RANGE" $L 2>/dev/null && break; sleep 0.1; done
    PID=$(pgrep -x matmul | head -1); [ -n "${PID:-}" ] && echo $PID | sudo -n tee $TP >/dev/null
    if [ "$WARM_ENGINE" = 1 ]; then
        # Wait for the fill to report done, then close the window it is
        # holding open for us. E0 is re-sampled AFTER the engine is off, so
        # erases_during counts only what leaked into the timed run.
        for i in $(seq 1 20000); do
            grep -q "^WARMUP hold" $L 2>/dev/null && break
            kill -0 $BG 2>/dev/null || break
            sleep 0.1
        done
        engine_off
        sleep 1
        E0=$(gs erases_done)
        say "  fill done, engine pinned off before the clock starts"
    fi
    wait $BG; echo 0 | sudo -n tee $TP >/dev/null
    read M SD < <(plateau $L)
    series $L "r${rep}-N${N}-${MODE}"
    RES=$(grep "^PHYS end    weights" $L | sed -n 's/.*LtRAM \([0-9]*\) .*/\1/p' | tail -1)
    ED=$(( $(gs erases_done) - E0 ))
    NSL=$(awk -v m="$M" -v l="$NLINES" 'BEGIN{printf "%.1f", m*1e9/l}')
    echo "$rep,$N,$MODE,$BYTES,$PAGES,$M,$SD,$NSL,${RES:-0},$CB,$ED,$DB" >> "$OUT"
    PCT=$(awk -v m="$M" -v v="$SD" 'BEGIN{printf "%.2f", (m>0?100*v/m:0)}')
    RPC=$(awk -v r="${RES:-0}" -v p="$PAGES" 'BEGIN{printf "%.1f", (p?100*r/p:0)}')
    WU=$(grep "^WARMUP done" $L | tail -1); [ -n "$WU" ] && say "  $WU"
    say "  $MODE  $M s   $NSL ns/line   sd ${PCT}%   resident ${RES:-0}/$PAGES (${RPC}%)   erases $ED"
    awk -v p="$PCT" 'BEGIN{exit !(p>2)}' && \
        say "  !! sd ${PCT}% -- this run never settled, the mean is not a latency"
    awk -v r="$RPC" 'BEGIN{exit !(r<99)}' && \
        say "  !! only ${RPC}% reached flash -- this is a BLEND of both media, not a NOR latency"
    [ "$ED" -gt 0 ] && say "  !! the engine ran during the measurement -- this point is contaminated"
    rm -f $L
    done

    echo 8192 | sudo -n tee $HW >/dev/null; echo 2048 | sudo -n tee $LW >/dev/null
    ec_save
done
done
say "done -> $OUT"
