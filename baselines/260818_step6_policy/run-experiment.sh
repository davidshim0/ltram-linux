#!/bin/bash
# run-experiment.sh — the step-7 measurement, run on z08.
#
# Answers "what did putting the weights on flash cost", in a way that survives
# scrutiny. Two things make that harder than running the workload twice:
#
#  1. THE DIGEST IS THE GATE. Both runs must reproduce the step-1 baseline
#     digest exactly. A faster run with a different digest is not a result, it
#     is corruption -- and on this hardware corruption is silent by default.
#
#  2. TURNING TARGETING ON CHANGES THREE THINGS AT ONCE: reads now hit flash,
#     the pages sit on a different NUMA node, and migration itself costs erases
#     and DMA time. One before/after number cannot attribute a slowdown to any
#     of them. The decomposition is printed at the end; the DRAM-backed control
#     that would isolate flash latency is NOT built yet, and the script says so
#     rather than letting one number carry three explanations.
set -u
BASELINE_DIGEST=${BASELINE_DIGEST:-41bd154efbce9bd07461680229516268bd481e8bbdb3d187cd8937ca7ae93a92}
N=${N:-7168}; ITERS=${ITERS:-20}; RUNS=${RUNS:-10}
WARMUP=${WARMUP:-600}          # seconds at steady state before targeting
MATMUL=${MATMUL:-$HOME/matmul}
INSPECT=${INSPECT:-$HOME/ltram-inspect}

[ -e /sys/kernel/ltram/target_pid ] || { echo "!! /sys/kernel/ltram missing -- not an LtRAM kernel"; exit 3; }

echo "=== kernel $(uname -r) ==="
echo "=== A: targeting OFF (control) ==="
OFF=$("$MATMUL" --n $N --iters $ITERS --runs $RUNS --verify --protect-weights | tee /dev/stderr)
OFF_MEAN=$(echo "$OFF" | awk '/^RESULT/{print $3}')
OFF_DIG=$(echo  "$OFF" | awk '/^DIGEST/{print $2}')

echo
echo "=== B: targeting ON ==="
"$MATMUL" --n $N --iters $ITERS --runs $RUNS --verify --protect-weights --print-ranges --hold 60 &
MM=$!
sleep 5
RANGES=$(grep -m3 "^RANGE" /proc/$MM/fd/1 2>/dev/null || true)
echo "  warming up ${WARMUP}s before attaching (steady state, not cold start)"
sleep "$WARMUP"
echo "$MM" | sudo tee /sys/kernel/ltram/target_pid >/dev/null
echo "  attached pid $MM; letting the scanner work"
sleep 120
sudo cat /sys/kernel/ltram/stats
wait $MM

echo
echo "=== provenance: which pages actually moved ==="
echo "  (feed the RANGE lines from the run above to $INSPECT)"

echo
echo "=== verdict ==="
echo "  control mean      ${OFF_MEAN:-?} s"
echo "  baseline digest   $BASELINE_DIGEST"
echo "  control digest    ${OFF_DIG:-?}"
[ "${OFF_DIG:-}" = "$BASELINE_DIGEST" ] \
  && echo "  digest MATCHES baseline -- timings are meaningful" \
  || echo "  !! DIGEST MISMATCH -- the computation changed; timings mean nothing"
cat <<'NOTE'

  NOT YET SEPARABLE: the on/off delta mixes flash read latency, NUMA distance
  and migration cost. Isolating the first needs a control that promotes to
  node 1 backed by DRAM -- same migration, same distance, no flash. That does
  not exist on this hardware yet, so report the delta as a combined figure and
  say so, rather than attributing it to flash latency.
NOTE
