#!/bin/bash
# ltram-test.sh -- the one test. Replaces check1a/station4/5/6/7.
#
# THERE IS NO --protect-weights ANYWHERE IN THIS FILE, and there never will be.
# It mprotects the weights PROT_READ, so they arrive ALREADY write-protected and
# are promoted on first sighting -- which means the arm -> wait -> promote cycle,
# the entire detection mechanism, never runs. It hid that from three separate
# runs before anyone noticed. The policy reads PTE state and has never looked at
# VMA flags, so the hint proves nothing it does not also conceal.
#
#   ./ltram-test.sh            no backend. Policy, allocator and state machine
#                              at full speed with NOTHING at risk: every flash
#                              write fails -ENODEV, so no sector is ever
#                              programmed and no page can reach DIRTY.
#   ./ltram-test.sh --flash    backend loaded. Pages genuinely land on NOR, and
#                              at exit they go VALID -> DIRTY.
set -u
N=4096; RUNS=15; ITERS=60; BATCH=32; HOLD=30; USE_FLASH=0; FLUSH=0
while [ $# -gt 0 ]; do case "$1" in
  --flash) USE_FLASH=1;; --n) N=$2; shift;; --runs) RUNS=$2; shift;;
  --iters) ITERS=$2; shift;; --batch) BATCH=$2; shift;; --flush) FLUSH=$2; shift;;
  *) echo "usage: $0 [--flash] [--n N] [--runs R] [--iters K] [--batch B] [--flush MB]"; exit 2;;
esac; shift; done

MM=$HOME/matmul; KO=$HOME/nor_eci/nor_eci_fulltest_ltram.ko
S=/sys/kernel/ltram/stats; P=/sys/kernel/debug/ltram/pagestate
TP=/sys/kernel/ltram/target_pid; PB=/sys/module/ltram_policy/parameters/promote_batch
OUT=/scratch/hushim/ltram-$(date +%m%d-%H%M%S); mkdir -p "$OUT" || exit 9
say(){ echo "[$(date +%H:%M:%S)] $*"; }
st(){ sudo -n cat $S; }
ps_(){ sudo -n cat $P; }
# EXACT first-field match. A regex of ^key also matches clean_buckets,
# clean_from_bucketlist and data_detail, which turns every assertion's
# arithmetic into multi-line garbage.
g(){ awk -v k="$2" '$1==k{print $2; exit}' <<<"$1"; }
cleanup(){ echo 0 | sudo -n tee $TP >/dev/null 2>&1; }
trap cleanup EXIT INT TERM

say "=== preflight ==="; uname -r
[ -e $TP ] || { say "!! not an LtRAM kernel"; exit 3; }
[ "$(cat $TP)" = "0" ] || { say "!! target_pid already set"; exit 3; }
pgrep -x matmul >/dev/null && { say "!! matmul already running"; exit 3; }
A=$(df -m / | awk 'NR==2{print $4}'); [ "${A:-0}" -lt 100 ] && { say "!! root has ${A} MB free"; exit 3; }
say "weights $((N*N*4/1048576)) MiB = $((N*N*4/4096)) pages   flash=$USE_FLASH   batch=$BATCH   scrub=${FLUSH}MB"
say "output -> $OUT"

if [ $USE_FLASH = 1 ]; then
        lsmod | grep -q nor_eci || { say "loading backend"; sudo -n insmod $KO provide_ops=1 test=0 || exit 5; sleep 2; }
        sudo -n dmesg | grep -iE "REGISTERED|STATUS WORD" | tail -2
else
        lsmod | grep -q nor_eci && { say "!! backend loaded but --flash not given; rmmod first"; exit 3; }
fi
echo $BATCH | sudo -n tee $PB >/dev/null

# --------------------------------------------------------------- control
say "=== control: DRAM only, policy detached ==="
FL=""; [ "$FLUSH" != 0 ] && FL="--flush $FLUSH"
# The scrub MUST be identical on both sides. A control that keeps its weights in
# LLC while the measured run is forced cold is not a comparison, it is two
# different experiments.
sudo -n $MM --n $N --iters $ITERS --runs 3 $FL --verify --print-ranges > "$OUT/control.log" 2>&1
CDIG=$(awk '/^DIGEST/{print $2}' "$OUT/control.log")
say "control digest ${CDIG:-NONE}  mean $(awk '/^RESULT/{print $3}' "$OUT/control.log") s"
[ -n "$CDIG" ] || { say "!! control failed"; tail -20 "$OUT/control.log"; exit 4; }

# --------------------------------------------------------------- measured
ps_ > "$OUT/before.pagestate"; st > "$OUT/before.stats"
say "=== measured: same workload, policy attached ==="
sudo -n $MM --n $N --iters $ITERS --runs $RUNS $FL --verify --print-ranges --phys --hold $HOLD \
     > "$OUT/run.log" 2>&1 &
SUDOPID=$!
for i in $(seq 1 120); do grep -q "^RANGE result" "$OUT/run.log" 2>/dev/null && break; sleep 1; done
PID=$(pgrep -x matmul | head -1)
[ -n "${PID:-}" ] || { say "!! workload died"; tail -20 "$OUT/run.log"; exit 6; }
echo $PID | sudo -n tee $TP >/dev/null; say "attached $PID"

echo "# t data dirty clean erasing moved_to_ltram moved_to_dram erases_done" > "$OUT/curve"
for t in $(seq 1 600); do
        [ -d /proc/$PID ] || { say "workload exited"; break; }
        PS=$(ps_); SS=$(st)
        echo "$((t*5)) $(g "$PS" data) $(g "$PS" dirty) $(g "$PS" clean) $(g "$PS" erasing) $(g "$SS" moved_to_ltram) $(g "$SS" moved_to_dram) $(g "$SS" erases_done)" >> "$OUT/curve"
        [ $((t % 12)) -eq 0 ] && say "  t+$((t*5))s  data $(g "$PS" data)  dirty $(g "$PS" dirty)  clean $(g "$PS" clean)  to_ltram $(g "$SS" moved_to_ltram)"
        # The workload holds at the end, so this catches the LAST sample before
        # it exits -- the moment every flash page it owns is still VALID. That
        # number is what must become 0.
        if grep -q "^holding" "$OUT/run.log" 2>/dev/null && [ -z "${PEAK:-}" ]; then
                PEAK=$(g "$PS" data); ps_ > "$OUT/atexit.pagestate"
                say "  workload is holding: $PEAK pages VALID, all of which must be released"
        fi
        sleep 5
done
echo 0 | sudo -n tee $TP >/dev/null
wait $SUDOPID; RC=$?

# POLL for the release; do not assume a fixed delay is enough.
#
# The last few folios sit in per-CPU folio batches: taken off the LRU (so no lru
# flag, and the batch holds the reference) and drained lazily when the batch
# fills or something else drains it. They come back, just not instantly. A
# fixed 5 s wait declared 10 of them a permanent leak on 2026-08-26 and cost
# hours of hypotheses about refcounts and migration failure paths -- the give-away
# was freed_via_backstop climbing between two reads of the same file.
#
# A test that fails on a healthy system is worse than no test.
# Poll on the SHAPE of the count, not a stopwatch. A fixed timeout cannot tell a
# slow drain from a held reference, and it got that exact call wrong: it reported
# 22 pages as "a reference taken and never dropped", and 16 of them drained after
# the loop gave up -- the system was erasing flat out, which slows every drain.
# A count that is still falling is draining, however slowly. Only a count that has
# not moved at all is evidence of a reference nobody will drop.
# A flat count is almost always folios parked in per-CPU lru_add batches, NOT a
# held reference. Confirmed 2026-08-26 by reading /proc/kpageflags on eight stuck
# pfns: ACTIVE set, LRU clear, refs 1, map 0 -- folio_add_lru() sets ACTIVE and
# queues the folio, and LRU is only set when the batch drains. On a 48-core box
# that has gone idle a CPU holding 8 of 15 slots never fills its batch, so nothing
# drains it and the count sits forever. compact_memory calls lru_add_drain_all(),
# which released all eight instantly.
# So force the drain before concluding anything. Four separate "stuck page"
# investigations died on this; the harness should settle it rather than report it.
DRAIN=0; FLAT=0; PREV=-1; POLLED=0; FORCED=0
: > "$OUT/drain.log"
for i in $(seq 1 300); do
        V=$(g "$(ps_)" data)
        POLLED=$i
        echo "$i ${V:-READ_FAILED} forced=$FORCED" >> "$OUT/drain.log"
        [ "${V:-x}" = "0" ] && { DRAIN=$i; break; }
        if [ "${V:-x}" = "$PREV" ]; then FLAT=$((FLAT+1)); else FLAT=0; fi
        PREV="${V:-x}"
        if [ "$FLAT" -ge 10 ] && [ "$FORCED" = 0 ]; then
                say "  count flat at $V -- forcing lru_add_drain_all"
                echo 1 | sudo -n tee /proc/sys/vm/compact_memory >/dev/null 2>&1
                FORCED=1; FLAT=0; sleep 2; continue
        fi
        # Only a count still flat AFTER a forced drain is evidence of anything.
        [ "$FLAT" -ge 30 ] && [ "$FORCED" = 1 ] && break
        sleep 1
done
# Say nothing alarming here. The loop only WAITS; whether anything is actually
# stuck is decided below, from the authoritative post-loop read. An earlier
# version shouted "this one is real" from here whenever the loop timed out --
# including once when a single read came back empty and the pages had in fact
# all been released, so the log and the verdict flatly contradicted each other.
# drain.log records what each poll saw, so a future timeout is diagnosable.
[ "$DRAIN" -gt 0 ] && say "all pages released after ${DRAIN}s" \
                   || say "drain poll timed out; the verdict below is authoritative"
ps_ > "$OUT/after.pagestate"; st > "$OUT/after.stats"
RDIG=$(awk '/^DIGEST/{print $2}' "$OUT/run.log")

# --------------------------------------------------------------- assertions
PS=$(cat "$OUT/after.pagestate"); SS=$(cat "$OUT/after.stats")
V=$(g "$PS" data); D=$(g "$PS" dirty); F=$(g "$PS" clean)
FB=$(g "$PS" clean_from_bucketlist); INV=$(awk '/^invariant/{print $2}' <<<"$PS")
E=$(awk '/^erasing/{$1="";sub(/^ /,"");print}' <<<"$PS")
TOL=$(g "$SS" moved_to_ltram); BS=$(g "$SS" freed_via_backstop); HK=$(g "$SS" freed_via_hook)
# Three terms, not four. Erasing is a marker on a page that is still counted
# in dirty, so adding it would double-count exactly one page.
SUM=$((V + D + F)); ACC=$((V + BS + HK))
FAIL=0
{
echo "================= LTRAM TEST ================="
echo "N=$N pages=$((N*N*4/4096))  runs=$RUNS  batch=$BATCH  flash=$USE_FLASH  exit=$RC"
echo
echo "-- correctness --"
echo "  control digest $CDIG"
echo "  run     digest $RDIG"
[ "$CDIG" = "$RDIG" ] && echo "  DIGEST MATCHES" || { echo "  !! DIGEST MISMATCH -- data was corrupted"; FAIL=1; }
echo
echo "-- state machine --"
echo "  data $V  dirty $D  clean $F      erasing: $E"
echo "  data+dirty+clean $SUM (want 65536)"; [ "$SUM" = "65536" ] || { echo "  !! a page was lost from every state"; FAIL=1; }
echo "  clean_from_bucketlist $FB (want $F)"; [ "$FB" = "$F" ] || { echo "  !! bucket lists disagree with the clean count"; FAIL=1; }
echo "  invariant $INV"; [ "$INV" = "ok" ] || FAIL=1
echo
echo "-- release at exit: every page the workload held must be released --"
echo "  VALID while it held        ${PEAK:-?}"
echo "  VALID after it exited      $V   (drain poll: ${DRAIN}s)"
if [ "${V:-1}" = "0" ]; then echo "  ALL RELEASED"
else echo "  !! $V pages still VALID after ${POLLED}s (lru_add_drain_all forced: $FORCED)."
     echo "     Flat AFTER a forced drain, so not a per-CPU folio batch. Check"
     echo "     /proc/kpageflags for these pfns: ACTIVE without LRU would mean"
     echo "     the batch theory after all; anything else is a held reference."
     echo "     pfns are in the data_detail block of after.pagestate."; FAIL=1; fi
echo
echo "-- page accounting: every promoted page holds data, or was freed --"
echo "  moved_to_ltram      $TOL"
echo "  still holding data  $V"
echo "  freed_via_backstop  $BS   (batch path: process exit, munmap, reclaim)"
echo "  freed_via_hook      $HK   (single-page path: folio_put -> __folio_put_small)"
echo "  valid+backstop+hook $ACC  (want $TOL)"
[ "$ACC" = "$TOL" ] && echo "  ACCOUNTING CLOSES" || { echo "  !! $((TOL-ACC)) promoted pages are unaccounted"; FAIL=1; }
[ "$V" -gt 0 ] && echo "  NOTE: $V pages are still VALID with no owner process -- a reference"
[ "$V" -gt 0 ] && echo "        that was taken and never dropped. Not lost, but unreclaimable."
echo
echo "-- timings --"; grep "^  run" "$OUT/run.log" | tail -4
echo
[ $FAIL = 0 ] && echo "VERDICT: PASS" || echo "VERDICT: FAIL"
echo "=============================================="
} | tee "$OUT/VERDICT.txt"
sync; say "done -> $OUT"
exit $FAIL
