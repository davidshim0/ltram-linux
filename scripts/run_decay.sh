#!/bin/bash
# Cold and write-cold across the whole T ladder, for every workload.
#
#   ./scripts/run_decay.sh [workload ...]
#
# See decay_census.py for why this is one pass per T-ladder rather than one
# run per T: clearing the bits once and reading repeatedly turns a curve that
# cost one run per point into a curve that costs one run, total.
#
# One process per workload for the whole sweep. Restarting between passes
# would pay the load and settle each time and measure a different cache state
# each time.
set -u
R="$(cd "$(dirname "$0")/.." && pwd)"; W="$R/workloads"
OUT="${DECAY_OUT:-$R/baselines/$(date +%y%m%d)_census/decay}"; mkdir -p "$OUT"
LADDER="${LADDER:-5,10,20,30,45,60,90,120,180,240,300,360,480,600}"
PASSES="${PASSES:-3}"
THREADS=${THREADS:-16}
GRAPH="$W/gapbs/benchmark/graphs/kron22.sg"
KEYS=${KEYS:-7000000}; VSZ=${VSZ:-140}; RATE=${RATE:-10000}
WANT="${*:-pagerank bfs llama redis memcached}"
have(){ [[ " $WANT " == *" $1 "* ]]; }
say(){ printf '\n\033[1m== %s\033[0m\n' "$*"; }

# A shell whose command line happens to contain the pattern is not a running
# workload. `pgrep -f` matches full command lines, so the launcher that carries
# this script's own text matches it -- that self-match has cost this project
# four separate incidents, including an 8-hour deadlock. Filter by comm.
stale=$(pgrep -f "gapbs/(pr|bfs|cc|sssp|tc|bc)|llama-cli|kv_load.py|redis-server|memcached/memcached" | while read -r p; do
  c=$(cat "/proc/$p/comm" 2>/dev/null) || continue
  case "$c" in bash|sh|dash|pgrep|grep) ;; *) echo "$p" ;; esac
done | wc -l)
if [ "$stale" -gt 0 ]; then
  echo "!! $stale workload process(es) already running -- they will compete for every"
  echo "   core and change what this run measures. Kill them first:"
  pgrep -af "gapbs/(pr|bfs|cc|sssp|tc|bc)|llama-cli|kv_load.py|redis-server|memcached/memcached" | grep -vE ' (bash|sh|dash) ' | cut -c1-100 | sed 's/^/     /'
  exit 1
fi

go(){                         # go <label> <file> <pid>
  local label=$1 f=$2 pid=$3
  "$R/scripts/decay_census.py" --pid "$pid" --ladder "$LADDER" \
      --passes "$PASSES" --label "$label" --out "$OUT/$f.csv"
  [ -s "$OUT/$f.csv" ] && column -s, -t "$OUT/$f.csv" | sed 's/^/    /'
}

if have pagerank; then say "pagerank"
  OMP_NUM_THREADS=$THREADS "$W/gapbs/pr" -f "$GRAPH" -n 5000000 -i 20 >/dev/null 2>&1 & P=$!
  sleep 45; go "pagerank (kron22)" pagerank $P; kill $P 2>/dev/null; sleep 3
fi
if have bfs; then say "bfs"
  OMP_NUM_THREADS=$THREADS "$W/gapbs/bfs" -f "$GRAPH" -n 5000000 >/dev/null 2>&1 & P=$!
  sleep 45; go "bfs (kron22)" bfs $P; kill $P 2>/dev/null; sleep 3
fi
if have llama; then say "llama.cpp"
  "$W/llama.cpp/build/bin/llama-cli" -m "$W/models/tinyllama-1.1b-q4.gguf" \
     -t $THREADS -c 4096 -n -1 -st --context-shift --ignore-eos --no-warmup \
     -p Write_a_long_technical_report_about_memory_systems. >/dev/null 2>&1 & P=$!
  sleep 60; go "llama.cpp (tinyllama 1.1B)" llama $P; kill $P 2>/dev/null; sleep 3
fi
kv(){                         # kv <label> <file> <proto> <port> <server cmd...>
  local label=$1 f=$2 proto=$3 port=$4; shift 4
  "$@" >"$OUT/$f.server.log" 2>&1 & local SP=$!
  sleep 3
  "$R/scripts/kv_load.py" --proto "$proto" --port "$port" --keys "$KEYS" \
      --value "$VSZ" --threads 8 --load-only
  echo "  settling 150s"; sleep 150
  "$R/scripts/kv_load.py" --proto "$proto" --port "$port" --keys "$KEYS" \
      --value "$VSZ" --no-load --read-ratio 0.95 --ops-per-sec "$RATE" \
      --threads 8 >/dev/null 2>&1 & local LP=$!
  sleep 15
  go "$label" "$f" $SP
  kill $LP $SP 2>/dev/null; sleep 3
}
if have redis; then say "redis"
  kv "redis (95/5 zipfian, $RATE ops/s)" redis redis 6379 \
     "$W/redis-src/src/redis-server" --save '' --appendonly no --protected-mode no
fi
if have memcached; then say "memcached"
  kv "memcached (95/5 zipfian, $RATE ops/s)" memcached memcached 11211 \
     "$W/memcached/memcached" -m 4096 -t 8 -u nobody
fi
say "plotting"
"$R/scripts/plot_census.py" "$OUT"/*.csv -o "$OUT"
