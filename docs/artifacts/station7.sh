#!/bin/bash
# station7.sh -- what does moving a working set onto NOR cost, iteration by iteration?
#
# THE SHAPE WE EXPECT, and why each part happens:
#   1. FAST      everything in DRAM
#   2. SLOW      migration in flight. Every moved_to_ltram page costs a ~16.4 ms sector
#                ERASE plus the DMA, serialised in the scan thread, while the
#                workload competes for the same bus.
#   3. MIDDLE    all pages resident, no more writes. Pure NOR read cost.
#
# Each POINT is ONE cold pass over the weights. --iters 1 so no pass shares a
# cache with itself, --flush 64 to scrub the 16 MiB shared LLC between passes.
# The scrub sits outside the timer by construction.
#
# NOTHING IS RE-ERASED. 64 MiB of weights is 16,384 pages against 65,536
# sectors, so every page gets its own sector and is written exactly once. A
# bulk pre-erase would not help: the driver erases each sector unconditionally
# immediately before programming it, so the erase cost is in phase 2 either way.
set -u
N=${N:-4096}; RUNS=${RUNS:-1500}; BATCH=${BATCH:-128}; FLUSH=${FLUSH:-64}
MM=$HOME/matmul; INSPECT=$HOME/ltram-inspect
KO=$HOME/nor_eci/nor_eci_fulltest_ltram.ko
S=/sys/kernel/ltram/stats; TP=/sys/kernel/ltram/target_pid
PB=/sys/module/ltram_policy/parameters/promote_batch; DBG=/sys/kernel/debug/ltram
OUT=/scratch/hushim/station7-$(date +%m%d-%H%M); mkdir -p "$OUT" || exit 9
PAGES=$((N*N*4/4096))
say(){ echo "[$(date +%H:%M:%S)] $*"; }
cleanup(){ echo 0 | sudo -n tee $TP >/dev/null 2>&1; }
trap cleanup EXIT INT TERM

say "=== preflight ==="; uname -r
[ -e $TP ] || { say "!! wrong kernel"; exit 3; }
[ "$(cat $TP)" = "0" ] || { say "!! target_pid already set"; exit 3; }
AVAIL=$(df -m / | awk 'NR==2{print $4}')
[ "${AVAIL:-0}" -lt 100 ] && { say "!! root has ${AVAIL} MB free -- refusing"; exit 3; }
say "weights $((N*N*4/1048576)) MiB = $PAGES pages of 65536 sectors -- each written exactly once"
say "output -> $OUT"

# ---------------------------------------------------------- A: DRAM baseline
say "=== phase A: DRAM baseline (no backend, no policy) ==="
sudo -n $MM --n $N --iters 1 --runs $RUNS --verify --flush $FLUSH --print-ranges \
     > "$OUT/A-dram.log" 2>&1
say "phase A exit $?  $(grep -c '^POINT' "$OUT/A-dram.log") points"
grep -E "^RESULT|^DIGEST" "$OUT/A-dram.log"

# ---------------------------------------------------------- B: migrate to NOR
if ! lsmod | grep -q nor_eci; then
        say "=== loading backend ==="
        sudo -n insmod $KO provide_ops=1 test=0 || { say "!! insmod failed"; exit 5; }
        sleep 3
fi
echo $BATCH | sudo -n tee $PB >/dev/null
say "promote_batch $(cat $PB)  (erase-bound: ~16.4 ms/sector, serialised in the scan thread)"

say "=== phase B: same workload, policy attached ==="
sudo -n $MM --n $N --iters 1 --runs $RUNS --verify --flush $FLUSH --print-ranges \
     > "$OUT/B-nor.log" 2>&1 &
SUDOPID=$!
for i in $(seq 1 90); do grep -q "^TSTART" "$OUT/B-nor.log" 2>/dev/null && break; sleep 1; done
PID=$(pgrep -x matmul | head -1)
[ -n "${PID:-}" ] || { say "!! workload died"; tail -20 "$OUT/B-nor.log"; exit 6; }
R=$(awk '/^RANGE weights/{printf "%s %s %s", $2,$3,$5}' "$OUT/B-nor.log")
echo $PID | sudo -n tee $TP >/dev/null
say "attached pid $PID; sampling residency alongside the timings"

# Sampler: absolute epoch so it lines up with matmul's TSTART with no guesswork.
echo "# epoch on_flash pages_in_use moved_to_ltram moved_to_dram writes_ok" > "$OUT/residency.curve"
( while [ -d /proc/$PID ]; do
        LT=$(sudo -n $INSPECT $PID $R 2>/dev/null | awk '/^weights/{print $4}')
        SS=$(sudo -n cat $S 2>/dev/null)
        echo "$(date +%s.%N) ${LT:-0} $(awk '/pages_in_use/{print $2}'<<<"$SS") $(awk '/^moved_to_ltram/{print $2}'<<<"$SS") $(awk '/^moved_to_dram/{print $2}'<<<"$SS") $(sudo -n cat $DBG/writes_ok 2>/dev/null)" >> "$OUT/residency.curve"
        sleep 3
  done ) &
SAMPLER=$!

for t in $(seq 1 400); do
        [ -d /proc/$PID ] || break
        [ $((t % 20)) -eq 0 ] && say "  $(tail -1 "$OUT/residency.curve" | awk '{print $2}') / $PAGES on flash, $(grep -c '^POINT' "$OUT/B-nor.log") points"
        sleep 5
done
wait $SUDOPID; RC=$?
kill $SAMPLER 2>/dev/null
echo 0 | sudo -n tee $TP >/dev/null
sudo -n cat $S > "$OUT/stats.after"
say "phase B exit $RC  $(grep -c '^POINT' "$OUT/B-nor.log") points"

{
echo "============ STATION 7: COST OF LIVING ON NOR ============"
echo "N=$N  $((N*N*4/1048576)) MiB = $PAGES pages   runs=$RUNS   promote_batch=$BATCH   flush=${FLUSH} MiB"
echo
A_DIG=$(awk '/^DIGEST/{print $2}' "$OUT/A-dram.log"); B_DIG=$(awk '/^DIGEST/{print $2}' "$OUT/B-nor.log")
echo "DRAM baseline : $(awk '/^RESULT/{print $3}' "$OUT/A-dram.log") s mean   digest $A_DIG"
echo "NOR run       : $(awk '/^RESULT/{print $3}' "$OUT/B-nor.log") s mean   digest $B_DIG   exit $RC"
[ "$A_DIG" = "$B_DIG" ] && echo "DIGEST MATCHES -- timings are meaningful" || echo "!! DIGEST MISMATCH -- timings mean nothing"
echo
echo "-- phase A (DRAM), first and last 3 --"; grep '^POINT' "$OUT/A-dram.log" | head -3; grep '^POINT' "$OUT/A-dram.log" | tail -3
echo "-- phase B (NOR), first 3 / slowest 3 / last 3 --"
grep '^POINT' "$OUT/B-nor.log" | head -3
grep '^POINT' "$OUT/B-nor.log" | sort -k3 -g -r | head -3
grep '^POINT' "$OUT/B-nor.log" | tail -3
echo
echo "-- final residency --"; tail -1 "$OUT/residency.curve"
echo "-- counters --"; cat "$OUT/stats.after"
echo "=========================================================="
} | tee "$OUT/VERDICT.txt"
sync; say "done -> $OUT"
