#!/bin/bash
# How does the COLD fraction move with T?
#
#   ./scripts/sweep_T.sh [workload ...]
#
# Write-cold comes at every multiple of S from one run, because soft-dirty is
# per page. Cold does not: without root the accessed bit is only readable in
# aggregate (smaps Referenced:), so a run answers cold at exactly T = S. To
# get cold as a function of T, run once per S.
#
# Measured cost of a sampling window on a 280 MiB process: clear_refs=4 is
# 0.7 ms, clear_refs=3 is 0.1 ms, smaps 0.8 ms, pagemap 1.5 ms. ~3 ms, linear
# in resident pages, so ~20 ms for a 1.9 GB redis. At S=15 s that is 0.1%.
# The floor on S is not the tooling; it is how many windows fit in the run.
#
# One process per workload for the whole sweep -- started once, then attached
# to at each S. Restarting per S would pay the load and settle four times and,
# worse, measure four different cache states.
set -u
R="$(cd "$(dirname "$0")/.." && pwd)"; W="$R/workloads"
OUT="${SWEEP_OUT:-$R/baselines/$(date +%y%m%d)_census/sweepT}"; mkdir -p "$OUT"
SS="${SS:-15 30 60 120}"
NW="${NW:-8}"
THREADS=${THREADS:-16}
GRAPH="$W/gapbs/benchmark/graphs/kron22.sg"
KEYS=${KEYS:-7000000}; VSZ=${VSZ:-140}; RATE=${RATE:-10000}
WANT="${*:-pagerank bfs llama redis memcached}"
have(){ [[ " $WANT " == *" $1 "* ]]; }
say(){ printf '\n\033[1m== %s\033[0m\n' "$*"; }

stale=$(pgrep -f "gapbs/(pr|bfs)|llama-cli|kv_load.py|redis-server|memcached/memcached" | grep -cv "^$$\$")
[ "$stale" = 0 ] || { echo "!! $stale workload process(es) running; kill them first"; exit 1; }

sweep(){                      # sweep <label> <pid>
  local label=$1 pid=$2
  for S in $SS; do
    local run=$(( S * NW ))
    printf '  S=%-4s %d windows, %ds ... ' "$S" "$NW" "$run"
    "$R/scripts/rw_census.py" --pid "$pid" --interval "$S" --run "$run" \
        --label "$label" --out "$OUT/${label%% *}-S$S.csv" 2>"$OUT/.err" \
      && awk -F, 'NR==2{printf "cold %s%%  write-cold %s%%\n", $5, $4}' \
             "$OUT/${label%% *}-S$S.csv" \
      || { echo "FAILED"; tail -2 "$OUT/.err"; }
  done
}

if have pagerank; then say "pagerank"
  OMP_NUM_THREADS=$THREADS "$W/gapbs/pr" -f "$GRAPH" -n 100000 -i 20 >/dev/null 2>&1 & P=$!
  sleep 45; sweep "pagerank (kron22)" $P; kill $P 2>/dev/null; sleep 3
fi
if have bfs; then say "bfs"
  OMP_NUM_THREADS=$THREADS "$W/gapbs/bfs" -f "$GRAPH" -n 100000 >/dev/null 2>&1 & P=$!
  sleep 45; sweep "bfs (kron22)" $P; kill $P 2>/dev/null; sleep 3
fi
if have llama; then say "llama.cpp"
  "$W/llama.cpp/build/bin/llama-cli" -m "$W/models/tinyllama-1.1b-q4.gguf" \
     -t $THREADS -c 4096 -n -1 -st --context-shift --ignore-eos --no-warmup \
     -p Write_a_long_technical_report_about_memory_systems. >/dev/null 2>&1 & P=$!
  sleep 60; sweep "llama.cpp (tinyllama 1.1B)" $P; kill $P 2>/dev/null; sleep 3
fi
kv(){                         # kv <label> <proto> <port> <server cmd...>
  local label=$1 proto=$2 port=$3; shift 3
  "$@" >"$OUT/$label.server.log" 2>&1 & local SP=$!
  sleep 3
  "$R/scripts/kv_load.py" --proto "$proto" --port "$port" --keys "$KEYS" \
      --value "$VSZ" --threads 8 --load-only
  echo "  settling 150s"; sleep 150
  "$R/scripts/kv_load.py" --proto "$proto" --port "$port" --keys "$KEYS" \
      --value "$VSZ" --no-load --read-ratio 0.95 --ops-per-sec "$RATE" \
      --threads 8 >/dev/null 2>&1 & local LP=$!
  sleep 15
  sweep "$label" $SP
  kill $LP $SP 2>/dev/null; sleep 3
}
if have redis; then say "redis"
  kv redis redis 6379 "$W/redis-src/src/redis-server" --save '' --appendonly no \
     --protected-mode no
fi
if have memcached; then say "memcached"
  kv memcached memcached 11211 "$W/memcached/memcached" -m 4096 -t 8 -u nobody
fi
rm -f "$OUT/.err"
say "csv in $OUT"
