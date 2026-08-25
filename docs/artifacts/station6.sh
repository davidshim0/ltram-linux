#!/bin/bash
# station6.sh -- does a write to a flash-resident page fault back to DRAM correctly?
#
# THE HAZARD. A store to the NOR read window is silently discarded by the
# hardware: no fault, no error, no signal. So if a flash-backed page is ever
# left writable, the write evaporates and nothing notices. Every test before
# this one only ever READ, and --protect-weights turned any stray write into a
# SIGSEGV, so this path has never been exercised on real data.
#
# THE SEQUENCE.
#   1. fill 32 MiB with pattern A, then stop writing to it
#   2. read it in a loop -- the policy sees read-mostly and promotes it to NOR.
#      No mprotect, no hints: the region stays rw the whole time, because we
#      have to be able to write to it in step 3.
#   3. when residency is high, touch the go file; the program writes pattern B
#      over every page. Each write to a flash-resident page must take a write
#      fault, be copied back to DRAM by wp_page_copy(), and land there.
#   4. read back and check every word.
#
# THE VERDICT. Words still reading pattern A are writes the hardware ate.
# Words reading neither A nor B are corruption in the copy-back. The pattern is
# position-dependent, so a page restored from the WRONG physical location fails
# too, not just one that never took the write.
set -u
MB=${MB:-32}; BATCH=${BATCH:-32}; TARGET_PCT=${TARGET_PCT:-90}
WB=$HOME/ltram-writeback; INSPECT=$HOME/ltram-inspect
KO=$HOME/nor_eci/nor_eci_fulltest_ltram.ko
S=/sys/kernel/ltram/stats; TP=/sys/kernel/ltram/target_pid
PB=/sys/module/ltram_policy/parameters/promote_batch; DBG=/sys/kernel/debug/ltram
GO=/scratch/hushim/GO
OUT=/scratch/hushim/station6-$(date +%m%d-%H%M); mkdir -p "$OUT" || exit 9
PAGES=$((MB*256))

say(){ echo "[$(date +%H:%M:%S)] $*"; }
st(){ sudo -n cat $S; }
wok(){ sudo -n cat $DBG/writes_ok 2>/dev/null || echo -1; }
cleanup(){ echo 0 | sudo -n tee $TP >/dev/null 2>&1; rm -f $GO; say "detached"; }
trap cleanup EXIT INT TERM

say "=== preflight ==="
uname -r; grep -o 'PREEMPT_DYNAMIC.*' /proc/version
[ -e $TP ] || { say "!! wrong kernel"; exit 3; }
[ "$(cat $TP)" = "0" ] || { say "!! target_pid already set"; exit 3; }
pgrep -x ltram-writeback >/dev/null && { say "!! already running"; exit 3; }
AVAIL=$(df -m / | awk 'NR==2{print $4}')
[ "${AVAIL:-0}" -lt 100 ] && { say "!! root has only ${AVAIL} MB free -- refusing"; exit 3; }
rm -f $GO
say "region ${MB} MiB = ${PAGES} pages; want ${TARGET_PCT}% resident before the write"
say "output -> $OUT"

if ! lsmod | grep -q nor_eci; then
        say "=== loading backend ==="
        sudo -n insmod $KO provide_ops=1 test=0 || { say "!! insmod failed"; exit 5; }
        sleep 3
fi
sudo -n dmesg | tail -5 > "$OUT/insmod.log"
echo $BATCH | sudo -n tee $PB >/dev/null
say "promote_batch $(cat $PB); writes_ok before $(wok)"
st > "$OUT/stats.before"

say "=== starting the workload (fills, then READS ONLY) ==="
sudo -n $WB --mb $MB --go $GO --maxwait 1800 > "$OUT/wb.log" 2>&1 &
SUDOPID=$!
for i in $(seq 1 60); do grep -q "^RANGE data" "$OUT/wb.log" 2>/dev/null && break; sleep 2; done
PID=$(pgrep -x ltram-writeback | head -1)
[ -n "${PID:-}" ] || { say "!! workload died during fill"; cat "$OUT/wb.log"; exit 6; }
R=$(awk '/^RANGE data/{printf "%s %s %s", $2,$3,$5}' "$OUT/wb.log")
say "pid $PID   range: $R"
echo $PID | sudo -n tee $TP >/dev/null
say "attached; waiting for promotion"

echo "# t on_flash in_use moved_to_dram wr_ok" > "$OUT/promote.curve"
NEED=$((PAGES*TARGET_PCT/100)); BEST=0
for t in $(seq 1 120); do
        [ -d /proc/$PID ] || { say "!! workload exited early"; break; }
        LT=$(sudo -n $INSPECT $PID $R 2>/dev/null | awk '/^data/{print $4}')
        LT=${LT:-0}; SS=$(st)
        echo "$((t*10)) $LT $(awk '/pages_in_use/{print $2}'<<<"$SS") $(awk '/^moved_to_dram/{print $2}'<<<"$SS") $(wok)" >> "$OUT/promote.curve"
        [ "$LT" -gt "$BEST" ] && BEST=$LT
        [ $((t % 3)) -eq 0 ] && say "  t+$((t*10))s  on flash $LT / $PAGES"
        [ "$LT" -ge "$NEED" ] && { say "  $LT / $PAGES resident (>= ${TARGET_PCT}%) -- releasing the write"; break; }
        sleep 10
done

say "=== residency immediately BEFORE the write ==="
sudo -n $INSPECT $PID $R 2>&1 | tee "$OUT/provenance.before"
st > "$OUT/stats.atwrite"
BEFORE_DEM=$(awk '/^moved_to_dram/{print $2}' "$OUT/stats.atwrite")

say "=== releasing the write ==="
touch $GO
# Detach the scanner so it cannot re-promote pages mid-write and confuse the result.
sleep 2; echo 0 | sudo -n tee $TP >/dev/null; say "scanner detached before the write lands"

for i in $(seq 1 90); do grep -qE "WRITEBACK:|TIMEOUT" "$OUT/wb.log" 2>/dev/null && break; sleep 5; done
if [ -d /proc/$PID ]; then
        say "=== residency AFTER the write (want ~0 -- everything moved_to_dram) ==="
        sudo -n $INSPECT $PID $R 2>&1 | tee "$OUT/provenance.after"
fi
wait $SUDOPID; RC=$?
st > "$OUT/stats.after"; sudo -n dmesg > "$OUT/dmesg.final"
AFTER_DEM=$(awk '/^moved_to_dram/{print $2}' "$OUT/stats.after")

{
echo "=============== STATION 6: WRITEBACK VERDICT ==============="
echo "region ${MB} MiB = ${PAGES} pages   promote_batch $BATCH   exit $RC"
echo
echo "-- peak residency before the write --"
echo "  $BEST / $PAGES pages on NOR ($((BEST*100/PAGES))%)"
grep "^data" "$OUT/provenance.before" 2>/dev/null
echo
echo "-- the write --"
grep -E "go seen|write took|STALE|WRONG|first bad|WRITEBACK" "$OUT/wb.log" 2>/dev/null
echo
echo "-- residency after (want ~0) --"
grep "^data" "$OUT/provenance.after" 2>/dev/null || echo "  (process already exited)"
echo
echo "-- demotions --"
echo "  moved_to_dram ${BEFORE_DEM:-?} -> ${AFTER_DEM:-?}  (delta $(( ${AFTER_DEM:-0} - ${BEFORE_DEM:-0} )), expect ~= pages that were on flash)"
echo
echo "-- counters --"
paste <(cat "$OUT/stats.before") <(awk '{print $2}' "$OUT/stats.after") | expand -t 22
echo
if   [ $RC -eq 44 ]; then echo "VERDICT: FAIL -- writes to flash-resident pages were LOST or corrupted."
elif [ $RC -eq 3 ];  then echo "VERDICT: INCONCLUSIVE -- never moved_to_ltram, so the write proved nothing."
elif [ "$BEST" -lt $((PAGES/10)) ]; then echo "VERDICT: INCONCLUSIVE -- only $BEST pages ever reached flash."
elif [ $RC -ne 0 ];  then echo "VERDICT: FAIL -- exit $RC."
else echo "VERDICT: PASS -- every page took the write and read back correctly,"
     echo "         with $BEST of $PAGES pages resident on NOR when the write was released."
fi
echo "==========================================================="
} | tee "$OUT/VERDICT.txt"
sync; say "done -> $OUT"
