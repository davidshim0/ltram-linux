#!/bin/bash
# How does LtRAM behave as the working set outgrows the pool?
#
#   sudo ~/gapbs_sweep.sh              scales 20 22 23 24, condition A, pr
#   SCALES="20 22" sudo ~/gapbs_sweep.sh
#
# The pool is 256 MB / 65,536 pages. The ladder is chosen against that:
#
#   scale 20  ~138 MB RSS  0.54x pool   the whole working set is eligible
#   scale 21  ~277 MB      1.08x        crossover: just barely does not fit
#   scale 22  ~558 MB      2.2x
#   scale 23  ~1.11 GB     4.3x
#   scale 24  ~2.2 GB      8.6x         only 12% can ever be resident
#
# Scale 20 is the control for policy error rather than capacity pressure: with
# room for everything, anything faulted back or freed after promotion is the
# policy being wrong, not the pool being full.
#
# Condition A only. A is the performance measurement and it is the one with a
# DRAM baseline phase; B answers a different question (lifecycle waste) and does
# not need to be swept over size.
set -u
[ "$(id -u)" = 0 ] || exec sudo -E "$0" "$@"
S=${SCRIPTS:-/scratch/hushim/ltram/scripts}
G=${LTRAM_W:-/scratch/hushim/workloads}/gapbs/benchmark/graphs
SCALES=${SCALES:-"20 21 22 23 24"}
KERN=${KERN:-pr}
ROOT=${ROOT:-/scratch/hushim/ltram/baselines/$(date +%y%m%d)_gapbs_sweep}
# Wear levelling off by default here. This sweep asks how placement behaves as
# the working set outgrows the pool; throttling promotion to 37 pages/s would
# mean 30 minutes of every run is just the pool filling, and the settled phase
# -- the number we actually quote -- would be the short part of the run.
export WEAR_GOVERNOR=${WEAR_GOVERNOR:-0} SCAN_INTERVAL_MS=${SCAN_INTERVAL_MS:-0}
export RUNFOR=${RUNFOR:-900} BASE=${BASE:-180}

for s in $SCALES; do
    [ -s "$G/kron${s}.sg" ] || { echo "!! missing $G/kron${s}.sg -- run gen_graphs.sh first"; exit 1; }
done
mkdir -p "$ROOT"
echo "sweep: scales [$SCALES], kernel $KERN, RUNFOR=${RUNFOR}s BASE=${BASE}s"
echo "       wear_governor=$WEAR_GOVERNOR scan_interval_ms=$SCAN_INTERVAL_MS -> $ROOT"

# The first scale needs a clean pool as much as the later ones do; whatever ran
# before this sweep left its sectors dirty.
echo; echo "draining before the first scale"
"$S/gapbs_ltram.sh" drain

for s in $SCALES; do
    echo; echo "======== scale $s ========"
    OUT="$ROOT/s$s" SCALE=$s "$S/gapbs_ltram.sh" A "$KERN"
    # Every size must start on a clean pool, or it measures the previous size's
    # dirty sectors instead of its own working set. ~24 min when the pool is full.
    "$S/gapbs_ltram.sh" drain
done

echo; echo "======== sweep summary ========"
printf "%-7s %10s %10s %10s %10s %10s %10s\n" \
       scale dram_s ltram_s slow_% peak_pct promoted freed
for s in $SCALES; do
    f="$ROOT/s$s/A-$KERN-summary.csv"
    v(){ awk -F, -v k="$1" '$1==k{print $2}' "$f" 2>/dev/null; }
    printf "%-7s %10s %10s %10s %10s %10s %10s\n" "$s" \
      "$(v dram_median_s)" "$(v ltram_settled_median_s)" "$(v slowdown_pct)" \
      "$(v peak_ltram_pct)" "$(v promoted)" "$(v promoted_then_freed)"
done
echo "-> $ROOT"
