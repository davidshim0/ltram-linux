#!/bin/bash
# ~4 minute end-to-end check of the gapbs/LtRAM harness before committing to a
# 3 hour sweep. It does not measure anything useful; it proves each stage
# produces output at all. Every previous zero-filled run of this harness was
# caused by a stage failing silently, so each one is asserted here.
set -u
[ "$(id -u)" = 0 ] || exec sudo -E "$0" "$@"
S=${SCRIPTS:-/scratch/hushim/ltram/scripts}
OUT=${OUT:-/scratch/hushim/ltram/baselines/smoke_$(date +%y%m%d_%H%M%S)}
SCALE=${SCALE:-22}
export WEAR_GOVERNOR=${WEAR_GOVERNOR:-0} SCAN_INTERVAL_MS=${SCAN_INTERVAL_MS:-0}
export RUNFOR=${RUNFOR:-90} BASE=${BASE:-30}

echo "smoke: scale $SCALE, ${BASE}s baseline + ${RUNFOR}s attached, ungoverned"
echo

OUT="$OUT" SCALE=$SCALE "$S/gapbs_ltram.sh" A pr || { echo; echo "FAIL: the run aborted"; exit 1; }

SUM="$OUT/A-pr-summary.csv"; TRI="$OUT/A-pr-trials.csv"; CSV="$OUT/A-pr.csv"
v(){ awk -F, -v k="$1" '$1==k{print $2}' "$SUM" 2>/dev/null; }
fails=0
chk(){ # name, value, test
    if [ -n "$2" ] && awk -v x="$2" "BEGIN{exit !($3)}" 2>/dev/null; then
        printf "  PASS  %-26s %s\n" "$1" "$2"
    else
        printf "  FAIL  %-26s %s\n" "$1" "${2:-<missing>}"; fails=$(( fails + 1 ))
    fi
}
echo; echo "======== checks ========"
chk "dram trials recorded"   "$(v dram_trials)"            'x > 0'
chk "ltram trials recorded"  "$(v ltram_trials)"           'x > 0'
chk "dram median non-zero"   "$(v dram_median_s)"          'x > 0'
chk "settled median non-zero" "$(v ltram_settled_median_s)" 'x > 0'
chk "pages promoted"         "$(v promoted)"               'x > 0'
chk "resident on LtRAM"      "$(v resident_at_end)"        'x > 0'
chk "peak residency %"       "$(v peak_ltram_pct)"         'x > 0'
chk "regression fitted"      "$(v fit_points)"             'x >= 8'
chk "implied ns/access"      "$(v implied_ns_per_access)"  'x != 0'

d=$(awk -F, '$3=="dram"'  "$TRI" 2>/dev/null | wc -l)
l=$(awk -F, '$3=="ltram"' "$TRI" 2>/dev/null | wc -l)
chk "trials csv: dram rows"  "$d" 'x > 0'
chk "trials csv: ltram rows" "$l" 'x > 0'
chk "state csv rows"         "$(( $(wc -l < "$CSV" 2>/dev/null || echo 1) - 1 ))" 'x > 0'
chk "negative baseline time" "$(awk -F, '$1+0<0' "$TRI" 2>/dev/null | wc -l)" 'x > 0'

echo
if [ "$fails" -eq 0 ]; then
    echo "ALL CHECKS PASSED -- the sweep is safe to start."
    echo "  slowdown $(v slowdown_pct)% at $(v peak_ltram_pct)% residency"
    echo "  implied $(v implied_ns_per_access) ns/access vs ~900 ns measured in fig7"
    echo "  (a 90s smoke run barely settles; treat both as sanity, not results)"
else
    echo "$fails CHECK(S) FAILED -- do not start the sweep."
    echo "  summary: $SUM"
fi
echo "-> $OUT"
exit $(( fails > 0 ))
