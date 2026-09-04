#!/bin/bash
# Serialise the Kronecker graphs the size sweep needs. Generation is write-heavy
# by design, which is exactly why it must happen with the scanner detached and
# never inside a measured run.
set -u
W=/scratch/hushim/workloads
G=$W/gapbs/benchmark/graphs
mkdir -p "$G"

# Wait for any gapbs harness to finish. Match on comm, never on the command
# line -- a -f pattern matches this script's own launcher and deadlocks.
for i in $(seq 1 900); do
    n=$(ps -eo comm | grep -cx 'gapbs_ltram.sh\|pr\|bfs' || true)
    [ "$n" -eq 0 ] && break
    [ $(( i % 30 )) -eq 0 ] && echo "[$(date +%H:%M:%S)] waiting: $n harness process still up"
    sleep 2
done
n=$(ps -eo comm | grep -cx 'gapbs_ltram.sh\|pr\|bfs' || true)
[ "$n" -ne 0 ] && { echo "!! $n still running after 30 min, refusing to start"; exit 1; }
echo "[$(date +%H:%M:%S)] clear, generating"

for s in 20 21 23 24; do
    f="$G/kron${s}.sg"
    if [ -s "$f" ]; then echo "[$(date +%H:%M:%S)] kron${s}.sg exists, skipping"; continue; fi
    echo "[$(date +%H:%M:%S)] converter -g $s -b $f"
    t0=$(date +%s)
    "$W/gapbs/converter" -g "$s" -b "$f" || { echo "!! converter failed at scale $s"; exit 1; }
    echo "[$(date +%H:%M:%S)] kron${s}.sg done in $(( $(date +%s) - t0 ))s, $(du -h "$f" | cut -f1)"
done
echo "[$(date +%H:%M:%S)] ALL GRAPHS READY"
ls -la "$G"/*.sg | awk '{printf "  %6.0f MB  %s\n", $5/1048576, $NF}'
