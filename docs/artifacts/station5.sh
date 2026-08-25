#!/bin/bash
# station5.sh -- the two questions after "does it move to flash at all".
#
#   TEST 2  CAPACITY. Ask for more than the window holds. 324 MiB of weights
#           against a 256 MiB window. The allocator must refuse cleanly: pages
#           plateau at the window size, not_moved_this_pass climbs, the workload keeps
#           running on a DRAM/flash mix, and the digest still matches. A crash,
#           a stray allocation, or a wrong digest are all failures.
#
#   TEST 3  WRITEBACK. The one the hardware makes dangerous. A store to the NOR
#           read window is silently discarded -- no fault, no error. So a
#           flash-resident page that is ever allowed to become writable loses the
#           write with no diagnostic. ltram-writeback fills a region, gets it
#           moved_to_ltram by only reading it, then writes every page and checks. Words
#           that read back as the OLD pattern are writes the hardware ate.
#
# The backend stays loaded across both. Removing it while a target pid is set is
# the leading explanation for the one unexplained hard reset this project has had.
set -u
TP=/sys/kernel/ltram/target_pid; S=/sys/kernel/ltram/stats
DBG=/sys/kernel/debug/ltram; PB=/sys/module/ltram_policy/parameters/promote_batch
OUT=/scratch/hushim/station5-$(date +%m%d-%H%M); mkdir -p "$OUT" || exit 9
GO=/scratch/hushim/GO
say(){ echo "[$(date +%H:%M:%S)] $*"; }
inuse(){ awk '/pages_in_use/{print $2}' <(sudo -n cat $S); }
wok(){ sudo -n cat $DBG/writes_ok 2>/dev/null || echo -1; }
cleanup(){ echo 0 | sudo -n tee $TP >/dev/null 2>&1; rm -f $GO; }
trap cleanup EXIT INT TERM

say "waiting for the station-4 run to finish"
for i in $(seq 1 360); do
        pgrep -x matmul >/dev/null || { [ "$(cat $TP)" = "0" ] && break; }
        sleep 10
done
echo 0 | sudo -n tee $TP >/dev/null; sleep 5
say "starting from: pages_in_use $(inuse)  writes_ok $(wok)"
lsmod | grep -q nor_eci || { say "!! backend not loaded -- cannot continue"; exit 5; }
# Pages must have gone back to the bitmap when the last process exited.
LEAK=$(inuse); [ "$LEAK" -gt 100 ] && say "!! WARNING: $LEAK flash pages still in use with no target -- possible leak"

# ============================================================ TEST 2: CAPACITY
say "=== TEST 2: capacity -- 324 MiB of weights into a 256 MiB window ==="
sudo -n $HOME/matmul --n 9216 --iters 8 --runs 5 --verify --protect-weights --print-ranges \
     > "$OUT/t2.log" 2>&1 &
SP2=$!
for i in $(seq 1 200); do grep -q "^RANGE result" "$OUT/t2.log" 2>/dev/null && break; sleep 2; done
PID=$(pgrep -x matmul | head -1)
if [ -n "${PID:-}" ]; then
        grep "^RANGE" "$OUT/t2.log" | tee "$OUT/t2.ranges"
        sudo -n cat $S > "$OUT/t2.stats.before"
        echo $PID | sudo -n tee $TP >/dev/null; say "attached $PID"
        echo "# t in_use moved_to_ltram not_moved_this_pass moved_to_dram wr_ok" > "$OUT/t2.curve"
        FLAT=0; LAST=-1
        for t in $(seq 1 150); do
                [ -d /proc/$PID ] || { say "workload exited"; break; }
                ST=$(sudo -n cat $S)
                I=$(awk '/pages_in_use/{print $2}'<<<"$ST"); P=$(awk '/^moved_to_ltram/{print $2}'<<<"$ST")
                F=$(awk '/not_moved_this_pass/{print $2}'<<<"$ST"); D=$(awk '/^moved_to_dram/{print $2}'<<<"$ST")
                echo "$((t*10)) $I $P $F $D $(wok)" >> "$OUT/t2.curve"
                [ $((t % 6)) -eq 0 ] && say "  t+$((t*10))s in_use $I moved_to_ltram $P failed $F moved_to_dram $D"
                if [ "$I" = "$LAST" ]; then FLAT=$((FLAT+1)); else FLAT=0; LAST=$I; fi
                [ $FLAT -ge 9 ] && { say "  plateau at $I pages -- window is full"; break; }
                sleep 10
        done
        A=$(awk '/^RANGE/{printf "%s %s %s ", $2,$3,$5}' "$OUT/t2.ranges")
        [ -d /proc/$PID ] && sudo -n $HOME/ltram-inspect $PID $A > "$OUT/t2.provenance" 2>&1
        sudo -n cat $S > "$OUT/t2.stats.after"
fi
wait $SP2; T2=$?; echo 0 | sudo -n tee $TP >/dev/null
say "TEST 2 exit $T2  peak flash pages $(awk 'NR>1{if($2>m)m=$2}END{print m+0}' "$OUT/t2.curve" 2>/dev/null)"
cat "$OUT/t2.provenance" 2>/dev/null
sleep 10; say "after exit, pages_in_use $(inuse)  (should be ~0 -- pages returned to the bitmap)"

# =========================================================== TEST 3: WRITEBACK
say "=== TEST 3: writeback -- do flash-resident pages survive being written? ==="
rm -f $GO
sudo -n $HOME/ltram-writeback --mb 64 --go $GO --maxwait 900 > "$OUT/t3.log" 2>&1 &
SP3=$!
for i in $(seq 1 100); do grep -q "^RANGE data" "$OUT/t3.log" 2>/dev/null && break; sleep 2; done
PID=$(pgrep -x ltram-writeback | head -1)
if [ -n "${PID:-}" ]; then
        R=$(awk '/^RANGE data/{printf "%s %s %s", $2,$3,$5}' "$OUT/t3.log")
        sudo -n cat $S > "$OUT/t3.stats.before"
        echo $PID | sudo -n tee $TP >/dev/null; say "attached $PID; waiting for it to be moved_to_ltram"
        echo "# t in_use lt_pages moved_to_dram wr_ok" > "$OUT/t3.curve"
        BEST=0
        for t in $(seq 1 90); do
                [ -d /proc/$PID ] || break
                LT=$(sudo -n $HOME/ltram-inspect $PID $R 2>/dev/null | awk '/^data/{print $4}')
                D=$(awk '/^moved_to_dram/{print $2}' <(sudo -n cat $S))
                echo "$((t*10)) $(inuse) ${LT:-0} $D $(wok)" >> "$OUT/t3.curve"
                [ $((t % 3)) -eq 0 ] && say "  t+$((t*10))s  on flash ${LT:-0}/16384  moved_to_dram $D"
                [ "${LT:-0}" -gt "$BEST" ] && BEST=${LT:-0}
                # 90% resident is plenty to prove the point; do not wait for the tail.
                [ "${LT:-0}" -ge 14746 ] && { say "  ${LT} pages resident -- releasing the write"; break; }
                sleep 10
        done
        say "peak residency before the write: $BEST / 16384 pages"
        sudo -n cat $S > "$OUT/t3.stats.atwrite"
        touch $GO
        sleep 45
        [ -d /proc/$PID ] && sudo -n $HOME/ltram-inspect $PID $R > "$OUT/t3.provenance.after" 2>&1
fi
wait $SP3; T3=$?; echo 0 | sudo -n tee $TP >/dev/null
sudo -n cat $S > "$OUT/t3.stats.after"; sudo -n dmesg > "$OUT/dmesg.final"

# ==================================================================== VERDICT
{
echo "============ STATIONS 5 & 6 VERDICT ============"
echo "-- TEST 2: capacity (324 MiB asked of a 256 MiB window) --"
echo "exit $T2   peak flash pages $(awk 'NR>1{if($2>m)m=$2}END{print m+0}' "$OUT/t2.curve" 2>/dev/null) of 65536"
grep -E "^RESULT|^DIGEST" "$OUT/t2.log" 2>/dev/null
cat "$OUT/t2.provenance" 2>/dev/null
echo "counters:"; paste <(cat "$OUT/t2.stats.before") <(awk '{print $2}' "$OUT/t2.stats.after") 2>/dev/null | expand -t 24
if [ $T2 -eq 0 ]; then echo "TEST 2: PASS -- overflow handled without crash or corruption"
else echo "TEST 2: FAIL -- exit $T2"; fi
echo
echo "-- TEST 3: writeback (the silently-discarded-store hazard) --"
echo "exit $T3"
grep -E "STALE|WRONG|WRITEBACK|write took|first bad|go seen" "$OUT/t3.log" 2>/dev/null
echo "residency after the write (want ~0 -- everything moved_to_dram back to DRAM):"
cat "$OUT/t3.provenance.after" 2>/dev/null
echo "counters:"; paste <(cat "$OUT/t3.stats.before") <(awk '{print $2}' "$OUT/t3.stats.after") 2>/dev/null | expand -t 24
if   [ $T3 -eq 44 ]; then echo "TEST 3: FAIL -- writes to flash-resident pages were LOST or corrupted"
elif [ $T3 -eq 3 ];  then echo "TEST 3: INCONCLUSIVE -- never got moved_to_ltram, so the write proved nothing"
elif [ $T3 -ne 0 ];  then echo "TEST 3: FAIL -- exit $T3"
else echo "TEST 3: PASS -- every page took the write and read back correctly"; fi
echo "================================================"
} | tee "$OUT/VERDICT.txt"
cp "$OUT/VERDICT.txt" $HOME/station5-VERDICT.txt 2>/dev/null
sync; say "done -> $OUT"
