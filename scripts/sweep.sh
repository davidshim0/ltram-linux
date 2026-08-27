#!/bin/bash
# sweep.sh -- working-set sweep, 32 KiB to 1 GiB in powers of two.
#
# Emits one CSV row per (size, mode). RESUMABLE: rows already in the CSV are
# skipped, so a sweep killed at 2am can be restarted and will pick up where it
# stopped. That matters because the whole thing takes hours, most of it erase.
#
# WHY THE COST IS UNAVOIDABLE. Every page promoted must eventually be erased at
# ~16.4 ms. The sweep promotes about 197,000 pages in total, so roughly an hour
# is pure erasing no matter how the runs are arranged. It is 0.003% of the
# device's rated endurance, so the wear does not matter; the wall clock does.
#
#   ./sweep.sh [--out FILE] [--modes comp,dram_cold,dram_warm,nor_cold,nor_warm]
set -u
OUT=${OUT:-/scratch/hushim/sweep.csv}
MODES=comp,dram_cold,dram_warm,nor_cold,nor_warm
while [ $# -gt 0 ]; do case "$1" in
  --out) OUT=$2; shift;; --modes) MODES=$2; shift;;
  *) echo "usage: $0 [--out FILE] [--modes LIST]"; exit 2;; esac; shift; done

MM=$HOME/matmul; KO=$HOME/nor_eci/nor_eci_fulltest_ltram.ko
S=/sys/kernel/ltram/stats; P=/sys/kernel/debug/ltram/pagestate
TP=/sys/kernel/ltram/target_pid; PB=/sys/module/ltram_policy/parameters/promote_batch
HW=/sys/module/ltram_policy/parameters/erase_high_water
LW=/sys/module/ltram_policy/parameters/erase_low_water
say(){ echo "[$(date +%H:%M:%S)] $*"; }
g(){ awk -v k="$2" '$1==k{print $2; exit}' <<<"$1"; }
has(){ [[ ",$MODES," == *",$1,"* ]]; }
done_already(){ grep -q "^$1,$2," "$OUT" 2>/dev/null; }

# N for each power-of-two weight size. N = sqrt(bytes/4), so the alternate
# entries are the exact powers of two and the others are those times sqrt(2).
NS="90 128 181 256 362 512 724 1024 1448 2048 2896 4096 5793 8192 11585 16384"

# Plateau = the last 30% of passes, after residency has settled. Reporting a
# mean over the whole run would average the migration ramp into the answer,
# which is the single easiest way to publish a wrong number here.
plateau(){ grep "^POINT" "$1" | awk '{v[n++]=$3} END{
    if(!n){print "nan nan 0"; exit} s=int(n*0.7); c=0; sum=0
    for(i=s;i<n;i++){c++; sum+=v[i]}
    m=sum/c; q=0; for(i=s;i<n;i++) q+=(v[i]-m)^2
    printf "%.6f %.6f %d\n", m, (c>1?sqrt(q/(c-1)):0), c }'; }
resident(){ grep "^PHYS end    weights" "$1" 2>/dev/null | sed -n 's/.*LtRAM \([0-9]*\) .*/\1/p' | tail -1; }

[ -f "$OUT" ] || echo "n,mode,bytes,pages,mean_s,sd_s,samples,resident_pages,digest" > "$OUT"
mkdir -p "$(dirname "$OUT")" 2>/dev/null

for N in $NS; do
    BYTES=$((N*N*4)); PAGES=$((BYTES/4096))
    # Fewer passes at the large sizes: one pass at N=16384 is seconds, and the
    # point of a long run is a plateau, not a big sample count.
    if   [ $PAGES -le 4096 ];  then R=300
    elif [ $PAGES -le 32768 ]; then R=200
    else                            R=120; fi
    say "===== N=$N  $((BYTES/1024)) KiB  $PAGES pages  runs=$R ====="

    if has comp && ! done_already "$N" comp; then
        # Compute floor: every row pinned to row 0, so the inner loop is L1
        # resident. EXACT only while one row fits in L1D (32 KiB, so N<=8192);
        # above that it carries real traffic and becomes an upper bound on
        # compute. The plot marks where that happens.
        L=/tmp/sw-$N-comp.log
        sudo -n $MM --n $N --iters 1 --runs $R --flush 32 --compute-only > $L 2>&1
        read M SD C < <(plateau $L)
        echo "$N,comp,$BYTES,$PAGES,$M,$SD,$C,0," >> "$OUT"; say "  comp      $M s"
    fi

    if has dram_cold && ! done_already "$N" dram_cold; then
        L=/tmp/sw-$N-dc.log
        sudo -n $MM --n $N --iters 1 --runs $R --flush 32 --verify > $L 2>&1
        read M SD C < <(plateau $L); D=$(awk '/^DIGEST/{print $2}' $L)
        echo "$N,dram_cold,$BYTES,$PAGES,$M,$SD,$C,0,$D" >> "$OUT"; say "  dram_cold $M s"
    fi

    if has dram_warm && ! done_already "$N" dram_warm; then
        L=/tmp/sw-$N-dw.log
        sudo -n $MM --n $N --iters 1 --runs $R --verify > $L 2>&1
        read M SD C < <(plateau $L); D=$(awk '/^DIGEST/{print $2}' $L)
        echo "$N,dram_warm,$BYTES,$PAGES,$M,$SD,$C,0,$D" >> "$OUT"; say "  dram_warm $M s"
    fi

    for MODE in nor_cold nor_warm; do
        has $MODE || continue; done_already "$N" $MODE && continue
        [ "$MODE" = nor_cold ] && FL="--flush 32" || FL=""
        lsmod | grep -q nor_eci || { sudo -n insmod $KO provide_ops=1 test=0 \
             inline_erase=0 verify_erased=1 || exit 5; sleep 2; }
        echo 512 | sudo -n tee $PB >/dev/null
        L=/tmp/sw-$N-$MODE.log
        sudo -n $MM --n $N --iters 1 --runs $R $FL --verify --print-ranges --phys --hold 5 > $L 2>&1 &
        BG=$!
        for i in $(seq 1 120); do grep -q "^RANGE" $L 2>/dev/null && break; sleep 1; done
        PID=$(pgrep -x matmul | head -1)
        [ -n "${PID:-}" ] && echo $PID | sudo -n tee $TP >/dev/null
        wait $BG
        echo 0 | sudo -n tee $TP >/dev/null
        read M SD C < <(plateau $L); D=$(awk '/^DIGEST/{print $2}' $L); RES=$(resident $L)
        echo "$N,$MODE,$BYTES,$PAGES,$M,$SD,$C,${RES:-0},$D" >> "$OUT"
        say "  $MODE  $M s   resident ${RES:-0}/$PAGES"

        # Recycle before the next point. Promoted pages are DIRTY now, and the
        # pool is 65,536 sectors, so without this the sweep runs out of clean
        # sectors somewhere around the 128 MiB point and every later size
        # quietly measures partial residency.
        CLEAN=$(g "$(sudo -n cat $P)" clean)
        if [ "${CLEAN:-0}" -lt 40000 ]; then
            say "  clean=$CLEAN, draining before the next size"
            echo 65536 | sudo -n tee $HW >/dev/null; echo 65535 | sudo -n tee $LW >/dev/null
            for i in $(seq 1 2400); do
                [ "$(g "$(sudo -n cat $P)" dirty)" = "0" ] && break; sleep 1
            done
            echo 8192 | sudo -n tee $HW >/dev/null; echo 2048 | sudo -n tee $LW >/dev/null
            say "  drained"
        fi
    done
done
say "sweep complete -> $OUT"
