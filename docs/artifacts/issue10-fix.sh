#!/bin/bash
# issue10-fix.sh -- does the new predicate detect read-mostly pages?
#
# Two phases, same binary, same size, same attach window. The only difference
# is --protect-weights, which is an EXPERIMENT CONTROL, not a kernel hint:
#   A  with it     -- apples-to-apples against the 2026-08-19 diagnostic run
#   B  without it  -- the transparency test. No application hint of any kind.
#
# The backend is deliberately NOT loaded. Nothing reaches flash, so promoted
# stays 0 by construction and promote_failed == sel_isolated. That is the point:
# this run isolates DETECTION from the flash write path, and it keeps the
# never-yet-exercised demotion path out of a run with no console attached.
set -u
cd "$HOME"
S=/sys/kernel/ltram/stats
OUT="$HOME/issue10-fix-$(date +%m%d-%H%M)"
mkdir -p "$OUT"

echo "kernel : $(uname -r)"
grep -o 'PREEMPT_DYNAMIC.*' /proc/version
echo "backend: $(lsmod | grep -c nor_eci) nor_eci module(s) loaded  (expect 0)"
echo "batch  : $(cat /sys/module/ltram_policy/parameters/promote_batch 2>/dev/null || echo '<not exported>')"
echo "output : $OUT"
echo

phase () {
        local name=$1; shift
        echo "================ phase $name ================"
        echo "argv: $*"
        sudo cat $S > "$OUT/$name.before"
        ./matmul "$@" > "$OUT/$name.matmul" 2>&1 &
        local mm=$!
        echo "matmul pid $mm, filling..."
        sleep 60
        if ! kill -0 $mm 2>/dev/null; then
                echo "!! matmul already exited -- see $OUT/$name.matmul"; cat "$OUT/$name.matmul"; return 1
        fi
        echo $mm | sudo tee /sys/kernel/ltram/target_pid >/dev/null
        echo "attached at $(date +%T); scanning for 240 s"
        sleep 240
        sudo cat $S > "$OUT/$name.after"
        echo 0 | sudo tee /sys/kernel/ltram/target_pid >/dev/null
        echo "detached at $(date +%T); waiting for matmul"
        wait $mm; echo "matmul exit status $?"
        echo
        echo "---- counters, delta over the 240 s attach ----"
        join -j1 <(sed 's/  */ /g' "$OUT/$name.before" | sort -k1,1) \
                 <(sed 's/  */ /g' "$OUT/$name.after"  | sort -k1,1) 2>/dev/null |
        awk '{d=$3-$2; printf "%-18s %14d\n", $1, d}' | sort
        echo
        grep -E "RESULT|DIGEST|mean|weights|result " "$OUT/$name.matmul" || true
        echo
}

phase A --n 7168 --iters 1000 --runs 1 --verify --protect-weights --print-ranges
phase B --n 7168 --iters 1000 --runs 1 --verify --print-ranges

echo "================ raw ================"
for f in "$OUT"/*.before "$OUT"/*.after; do echo "-- $f"; cat "$f"; done
