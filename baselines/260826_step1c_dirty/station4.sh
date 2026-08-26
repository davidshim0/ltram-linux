#!/bin/bash
# station4.sh -- does a moved_to_ltram page actually LIVE on NOR, and read back correctly?
#
# Stations 1 and 2 (detect, isolate) were proven 2026-08-20 with the backend
# unloaded. This run staffs stations 3 and 4: the flash write, and repointing the
# page tables at the flash copy.
#
# THE CORRECTNESS PROOF IS BUILT INTO MATMUL. It re-checks the digest BETWEEN
# EVERY RUN and exits 44 the moment one differs. Every run after promotion is
# therefore an independent readback test of pages now living on flash. Exit 42
# means a write reached a protected weight page. Either is loud and unambiguous.
#
# WEAR: one moved_to_ltram page == one 4096-byte sector == one erase (PAGE_SIZE ==
# SECT_SZ, asserted in the backend). N=4096 gives 64 MiB of weights = 16,384
# pages = 16,384 erases over 16,384 DISTINCT sectors, each rated 100,000 cycles.
# This run costs each of those sectors 0.001% of its life.
#
# NO APPLICATION HINT. --protect-weights is deliberately absent: it mprotects
# the weights PROT_READ, so they arrive ALREADY write-protected and are promoted
# on first sighting, and the arm -> wait -> promote cycle never runs. It also
# gave a SIGSEGV net if anything wrote a flash-resident weight -- that net is
# gone, and station 6 (0 stale, 0 wrong over 7,517 pages) is why that is now an
# acceptable trade.
#
# WHY ROOT: unprivileged pagemap on this box reports every page absent, so --phys
# and the debugfs window read are both blind without it. Both phases run the same
# way so the digest comparison stays apples-to-apples.
#
# WHY /scratch: z08's root filesystem is 100% full (14 MB free). /scratch is NFS
# with 507 GB, and it also survives a hard reset of this machine.
set -u
N=${N:-4096}; ITERS=${ITERS:-60}; RUNS=${RUNS:-15}; BATCH=${BATCH:-32}
HOLD=${HOLD:-150}; MAXWAIT=${MAXWAIT:-2400}
MM=$HOME/matmul; INSPECT=$HOME/ltram-inspect
KO=$HOME/nor_eci/nor_eci_fulltest_ltram.ko
S=/sys/kernel/ltram/stats; TP=/sys/kernel/ltram/target_pid
PB=/sys/module/ltram_policy/parameters/promote_batch
DBG=/sys/kernel/debug/ltram
OUT=/scratch/hushim/station4-$(date +%m%d-%H%M); mkdir -p "$OUT" || exit 9

say(){ echo "[$(date +%H:%M:%S)] $*"; }
wok(){ sudo -n cat $DBG/writes_ok 2>/dev/null || echo -1; }
wfa(){ sudo -n cat $DBG/writes_failed 2>/dev/null || echo -1; }
# The scanner must never be left pointed at a dead pid, and the backend must never
# be removed while it is. Runs on every exit path, including a kill.
cleanup(){ echo 0 | sudo -n tee $TP >/dev/null 2>&1; say "target_pid cleared"; }
trap cleanup EXIT INT TERM

say "=== preflight ==="
uname -r; grep -o 'PREEMPT_DYNAMIC.*' /proc/version
[ -e $TP ] || { say "!! no /sys/kernel/ltram -- wrong kernel"; exit 3; }
[ "$(cat $TP)" = "0" ] || { say "!! target_pid already set -- refusing"; exit 3; }
pgrep -x matmul >/dev/null && { say "!! a matmul is already running -- refusing"; exit 3; }
lsmod | grep -q nor_eci && { say "!! backend already loaded -- refusing (want a clean status word)"; exit 3; }
say "weights $((N*N*4/1048576)) MiB = $((N*N*4/4096)) pages = $((N*N*4/4096)) erases"
say "output -> $OUT"
df -h /scratch | tail -1

# ---------------------------------------------------------------- phase 0
say "=== phase 0: DRAM control, no backend, no policy ==="
sudo -n $MM --n $N --iters $ITERS --runs 3 --verify --print-ranges --phys \
     > "$OUT/p0.log" 2>&1
P0=$?; DIG0=$(awk '/^DIGEST/{print $2}' "$OUT/p0.log"); MEAN0=$(awk '/^RESULT/{print $3}' "$OUT/p0.log")
say "phase 0 exit $P0  digest ${DIG0:-NONE}  mean ${MEAN0:-?} s"
[ $P0 -eq 0 ] || { say "!! DRAM control failed -- stopping BEFORE touching flash"; tail -30 "$OUT/p0.log"; exit 4; }

# ---------------------------------------------------------------- backend
say "=== loading the flash write backend ==="
sudo -n dmesg > "$OUT/dmesg.pre"; sudo -n dmesg -C
sudo -n insmod $KO provide_ops=1 test=0 || { say "!! insmod failed"; exit 5; }
sleep 3; sudo -n dmesg > "$OUT/insmod.log"
grep -iE "STATUS WORD|REGISTERED|st_wait" "$OUT/insmod.log" | tail -20
grep -q "REGISTERED" "$OUT/insmod.log" || { say "!! backend did not register the ops"; exit 5; }
say "st_wait fingerprint (736us => 169_phy200):"; grep -o "st_wait[^,]*" "$OUT/insmod.log" | tail -2

echo $BATCH | sudo -n tee $PB >/dev/null; say "promote_batch = $(cat $PB) pages/pass"
say "flash writes before: ok=$(wok) failed=$(wfa)"

# ---------------------------------------------------------------- phase 1
say "=== phase 1: same workload, policy attached, backend live ==="
sudo -n $MM --n $N --iters $ITERS --runs $RUNS --verify --print-ranges --phys --hold $HOLD \
     > "$OUT/p1.log" 2>&1 &
SUDOPID=$!
for i in $(seq 1 150); do grep -q "^RANGE result" "$OUT/p1.log" 2>/dev/null && break; sleep 2; done
PID=$(pgrep -x matmul | head -1)
[ -n "${PID:-}" ] || { say "!! matmul died during fill"; tail -30 "$OUT/p1.log"; exit 6; }
grep "^RANGE" "$OUT/p1.log" | tee "$OUT/ranges.txt"

sudo -n cat $S > "$OUT/stats.before"
echo $PID | sudo -n tee $TP >/dev/null
say "attached pid $PID"

echo "# t_s pages_in_use moved_to_ltram not_moved_this_pass moved_to_dram writes_ok writes_failed" > "$OUT/promotion.curve"
LAST=-1; FLAT=0
for t in $(seq 1 $((MAXWAIT/10))); do
        [ -d /proc/$PID ] || { say "matmul exited during polling"; break; }
        ST=$(sudo -n cat $S)
        INUSE=$(awk '/pages_in_use/{print $2}' <<<"$ST"); PROM=$(awk '/^moved_to_ltram/{print $2}' <<<"$ST")
        FAIL=$(awk '/not_moved_this_pass/{print $2}' <<<"$ST"); DEM=$(awk '/^moved_to_dram/{print $2}' <<<"$ST")
        echo "$((t*10)) $INUSE $PROM $FAIL $DEM $(wok) $(wfa)" >> "$OUT/promotion.curve"
        [ $((t % 3)) -eq 0 ] && say "t+$((t*10))s  in_use $INUSE  moved_to_ltram $PROM  failed $FAIL  moved_to_dram $DEM  wr_ok $(wok)"
        if [ "$INUSE" = "$LAST" ]; then FLAT=$((FLAT+1)); else FLAT=0; LAST=$INUSE; fi
        if [ $FLAT -ge 6 ] && [ "${INUSE:-0}" -gt 0 ]; then
                say "flash page count flat at $INUSE for 60 s -- promotion converged"; break
        fi
        sleep 10
done

if [ -d /proc/$PID ]; then
        say "=== provenance: where the pages physically are ==="
        ARGS=$(awk '/^RANGE/{printf "%s %s %s ", $2, $3, $5}' "$OUT/ranges.txt")
        sudo -n $INSPECT $PID $ARGS 2>&1 | tee "$OUT/provenance.txt"
fi
sudo -n cat $S > "$OUT/stats.after"

say "waiting for matmul to finish (it holds ${HOLD}s at the end)"
wait $SUDOPID; P1=$?
echo 0 | sudo -n tee $TP >/dev/null       # detach BEFORE anything else touches the module
say "phase 1 exit $P1"
DIG1=$(awk '/^DIGEST/{print $2}' "$OUT/p1.log"); MEAN1=$(awk '/^RESULT/{print $3}' "$OUT/p1.log")
sudo -n dmesg > "$OUT/dmesg.final"

# ---------------------------------------------------------------- verdict
{
echo "================ STATION 4 VERDICT ================"
echo "kernel $(uname -r)   N=$N iters=$ITERS runs=$RUNS promote_batch=$BATCH"
echo
echo "-- per-run timings: migration should read as a STEP, not a slope --"
echo "control:"; grep "^  run" "$OUT/p0.log"
echo "flash:";   grep "^  run" "$OUT/p1.log"
echo
echo "-- did any page physically move (matmul's own pfn census) --"
grep -E "^PHYS (verdict|MOVED)|DRAM->LtRAM|LtRAM->DRAM" "$OUT/p1.log" | head -30
echo
echo "-- flash residency at steady state --"
cat "$OUT/provenance.txt" 2>/dev/null || echo "  (not captured)"
echo
echo "-- flash write counters --"
echo "writes_ok $(wok)   writes_failed $(wfa)"
echo
echo "-- policy counters, before -> after --"
paste <(cat "$OUT/stats.before") <(awk '{print $2}' "$OUT/stats.after") | expand -t 24
echo
echo "control  digest $DIG0   mean $MEAN0 s   exit $P0"
echo "flash    digest $DIG1   mean $MEAN1 s   exit $P1"
echo
if   [ $P1 -eq 44 ]; then echo "VERDICT: FAIL -- digest changed BETWEEN RUNS. Flash readback is wrong."
elif [ $P1 -eq 42 ]; then echo "VERDICT: FAIL -- a write reached a protected weight page (SIGSEGV/SIGBUS)."
elif [ $P1 -ne 0 ];  then echo "VERDICT: FAIL -- matmul exited $P1."
elif [ -z "$DIG1" ]; then echo "VERDICT: INCONCLUSIVE -- no digest emitted."
elif [ "$DIG0" != "$DIG1" ]; then echo "VERDICT: FAIL -- digest differs between DRAM control and flash run."
elif [ "$(wok)" -le 0 ]; then echo "VERDICT: INCONCLUSIVE -- digest matches but writes_ok is $(wok); nothing reached flash."
else
  echo "VERDICT: PASS -- identical digest with pages resident on NOR."
  echo "         Every run after promotion re-read the weights from flash and got the same answer."
fi
echo "==================================================="
} | tee "$OUT/VERDICT.txt"
cp "$OUT/VERDICT.txt" "$HOME/station4-VERDICT.txt" 2>/dev/null
sync; say "done. everything in $OUT"
