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
while [ $# -gt 0 ]; do case "$1" in
  --reps) REPS=$2; shift;; --out) OUT=$2; shift;; --sizes) SIZES=$2; shift;;
  *) echo "usage: $0 [--reps N] [--out FILE] [--sizes \"N N N\"]"; exit 2;; esac; shift; done

MM=$HOME/matmul; KO=$HOME/nor_eci/nor_eci_fulltest_ltram.ko
S=/sys/kernel/ltram/stats; P=/sys/kernel/debug/ltram/pagestate
TP=/sys/kernel/ltram/target_pid; PB=/sys/module/ltram_policy/parameters/promote_batch
HW=/sys/module/ltram_policy/parameters/erase_high_water
LW=/sys/module/ltram_policy/parameters/erase_low_water
say(){ echo "[$(date +%H:%M:%S)] $*"; }
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

lsmod | grep -q nor_eci || sudo -n insmod $KO provide_ops=1 test=0 \
    inline_erase=0 verify_erased=1 || exit 4
[ -f "$OUT" ] || echo "rep,n,mode,bytes,pages,mean_s,sd_s,ns_per_line,resident,clean_before,erases_during,dirty_before" > "$OUT"

for rep in $(seq 1 $REPS); do
for N in $SIZES; do
    BYTES=$((N*N*4)); PAGES=$((BYTES/4096)); LINES=$((BYTES/128))
    # sweep.sh's own rule, so the plateau is taken over the same sample and
    # these numbers can be put beside the ones in the figures.
    if [ $PAGES -le 32768 ]; then R=200; else R=120; fi
    say "===== rep $rep/$REPS  N=$N  $((BYTES/1048576)) MiB  $PAGES pages  runs=$R ====="

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
    echo 512 | sudo -n tee $PB >/dev/null

    L=/tmp/rl-dram.log
    sudo -n $MM --n $N --iters 1 --runs $R --flush 32 --verify > $L 2>&1
    read M SD < <(plateau $L)
    series $L "r${rep}-N${N}-dram"
    NSL=$(awk -v m="$M" -v l="$LINES" 'BEGIN{printf "%.1f", m*1e9/l}')
    echo "$rep,$N,dram_cold,$BYTES,$PAGES,$M,$SD,$NSL,0,$CB,0,$DB" >> "$OUT"
    say "  dram $M s   $NSL ns/line"
    rm -f $L

    E0=$(gs erases_done)
    L=/tmp/rl-nor.log
    sudo -n $MM --n $N --iters 1 --runs $R --flush 32 --verify --print-ranges --phys --hold 5 > $L 2>&1 &
    BG=$!
    for i in $(seq 1 180); do grep -q "^RANGE" $L 2>/dev/null && break; sleep 1; done
    PID=$(pgrep -x matmul | head -1); [ -n "${PID:-}" ] && echo $PID | sudo -n tee $TP >/dev/null
    wait $BG; echo 0 | sudo -n tee $TP >/dev/null
    read M SD < <(plateau $L)
    series $L "r${rep}-N${N}-nor"
    RES=$(grep "^PHYS end    weights" $L | sed -n 's/.*LtRAM \([0-9]*\) .*/\1/p' | tail -1)
    ED=$(( $(gs erases_done) - E0 ))
    NSL=$(awk -v m="$M" -v l="$LINES" 'BEGIN{printf "%.1f", m*1e9/l}')
    echo "$rep,$N,nor_cold,$BYTES,$PAGES,$M,$SD,$NSL,${RES:-0},$CB,$ED,$DB" >> "$OUT"
    PCT=$(awk -v m="$M" -v v="$SD" 'BEGIN{printf "%.2f", (m>0?100*v/m:0)}')
    say "  nor  $M s   $NSL ns/line   sd ${PCT}%   resident ${RES:-0}/$PAGES   erases $ED"
    awk -v p="$PCT" 'BEGIN{exit !(p>2)}' && \
        say "  !! sd ${PCT}% -- this run never settled, the mean is not a latency"
    [ "$ED" -gt 0 ] && say "  !! the engine ran during the measurement -- this point is contaminated"
    rm -f $L

    echo 8192 | sudo -n tee $HW >/dev/null; echo 2048 | sudo -n tee $LW >/dev/null
done
done
say "done -> $OUT"
