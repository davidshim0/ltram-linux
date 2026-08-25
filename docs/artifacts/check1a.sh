#!/bin/bash
# 1a validation: hammer lt_take/lt_give_back and assert the shadow survives.
#
# NO APPLICATION HINT. --protect-weights is deliberately absent: it mprotects
# the region PROT_READ, so pages arrive ALREADY write-protected and are promoted
# on first sighting, and the arm -> wait -> promote cycle -- the whole detection
# mechanism -- never runs. Expect was_written to jump from ~3 to ~one per page in
# the region on the first sweep, then fall away as pages go quiet.
#
# No backend loaded on purpose. Every migration then FAILS, so each allocation
# is followed immediately by a free -- thousands of state transitions per
# second, which is a far harder test of the bookkeeping than pages that get
# allocated once and sit there. At the end free MUST be back to 65536 exactly.
set -u
P=/sys/kernel/debug/ltram/pagestate; S=/sys/kernel/ltram/stats
TP=/sys/kernel/ltram/target_pid
OUT=/scratch/hushim/check1a-$(date +%m%d-%H%M); mkdir -p "$OUT"
say(){ echo "[$(date +%H:%M:%S)] $*"; }
cleanup(){ echo 0 | sudo -n tee $TP >/dev/null 2>&1; }
trap cleanup EXIT INT TERM

say "backend loaded: $(lsmod | grep -c nor_eci) (want 0)"
say "=== BEFORE ==="; sudo -n cat $P | tee "$OUT/before"

sudo -n $HOME/matmul --n 2048 --iters 200 --runs 12 --verify --print-ranges \
     > "$OUT/mm.log" 2>&1 &
SP2=$!
for i in $(seq 1 60); do grep -q "^RANGE result" "$OUT/mm.log" 2>/dev/null && break; sleep 1; done
PID=$(pgrep -x matmul | head -1)
[ -n "${PID:-}" ] || { say "!! matmul died"; cat "$OUT/mm.log"; exit 6; }
echo $PID | sudo -n tee $TP >/dev/null; say "attached $PID"

for t in 1 2 3 4 5 6; do
        sleep 15
        [ -d /proc/$PID ] || break
        say "t+$((t*15))s  $(sudo -n cat $P | awk '/^free /{f=$2}/^valid/{v=$2}/^invariant/{i=$2}END{print "free",f,"valid",v,"invariant",i}')"
done
say "=== DURING (attached, transitions in flight) ==="; sudo -n cat $P | tee "$OUT/during"
sudo -n cat $S > "$OUT/stats.during"

echo 0 | sudo -n tee $TP >/dev/null; say "detached"
wait $SP2; say "matmul exit $?"
sleep 5
say "=== AFTER (all pages returned) ==="; sudo -n cat $P | tee "$OUT/after"
sudo -n cat $S > "$OUT/stats.after"

FREE=$(awk '/^free /{print $2}' "$OUT/after")
INV=$(awk '/^invariant/{print $2}' "$OUT/after")
echo
echo "================ 1a VERDICT ================"
echo "free after run : $FREE  (want 65536)"
echo "invariant      : $INV"
grep -E "^moved_to_ltram|not_moved_this_pass|pages_in_use|freed_via_backstop|^sweeps|scan_cursor|ptes_examined|^chosen" "$OUT/stats.after"
if [ "$FREE" = "65536" ] && [ "$INV" = "ok" ]; then
        echo "VERDICT: PASS -- every allocated page came back and all five assertions hold"
else
        echo "VERDICT: FAIL"; fi
echo "==========================================="
