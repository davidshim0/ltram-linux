#!/bin/bash
# The motivation measurement: how much memory is write-cold but not cold.
#
#   sudo ./scripts/run_census.sh --pilot            # 10 min each, all five
#   sudo ./scripts/run_census.sh --pilot pagerank   # just one
#   sudo ./scripts/run_census.sh --full
#
# Root is needed for /sys/kernel/mm/page_idle/bitmap, which is the only way to
# read the ACCESSED bit. Without it the write-cold column still works and the
# cold column -- the comparison the whole figure rests on -- does not.
#
# One workload at a time, deliberately. Two at once share the page cache and
# the memory controller, and the census attributes pages by pid, so a shared
# file page would be counted for whichever process was looked at first.
set -u
R="$(cd "$(dirname "$0")/.." && pwd)"; W="$R/workloads"
MODE=pilot
case "${1:-}" in --pilot) shift;; --full) MODE=full; shift;; esac
case "$MODE" in
  pilot) S=120; RUN=600;;
  full)  S=120; RUN=3600;;
esac
OUT="${CENSUS_OUT:-$R/baselines/$(date +%y%m%d)_census/$MODE}"
mkdir -p "$OUT"
THREADS=${THREADS:-16}
GRAPH="$W/gapbs/benchmark/graphs/kron22.sg"
ALL="pagerank bfs llama redis memcached"
WANT="${*:-$ALL}"

if [ "$(id -u)" != 0 ]; then
  cat <<'W'
!! Not root. The run will still work, with one leg missing:
     write-cold  measured per page via soft-dirty, every T          -- unaffected
     cold        falls back to smaps Referenced: aggregate only,
                 so it is reported at T = S and no other T, and it
                 cannot tell this process's accesses from another's
                 on shared pages.
   For cold at every T, run this on a host where you have root.
W
fi
[ -s "$GRAPH" ] || { echo "!! missing $GRAPH -- run workloads/build_motivation.sh"; exit 1; }

stale=$(pgrep -f "gapbs/(pr|bfs|cc|sssp|tc|bc)|llama-cli|kv_load.py|redis-server|memcached/memcached" | grep -v "^$$\$" | wc -l)
if [ "$stale" -gt 0 ]; then
  echo "!! $stale workload process(es) already running -- they will compete for every"
  echo "   core and change what this run measures. Kill them first:"
  pgrep -af "gapbs/(pr|bfs|cc|sssp|tc|bc)|llama-cli|kv_load.py|redis-server|memcached/memcached" | cut -c1-100 | sed 's/^/     /'
  exit 1
fi

say(){ printf '\n\033[1m== %s\033[0m\n' "$*"; }
census(){ "$R/scripts/rw_census.py" --interval "$S" --run "$RUN" "$@"; }
have(){ [[ " $WANT " == *" $1 "* ]]; }

# Server workloads: the census follows the SERVER, the load generator runs
# beside it. --cmd would follow the client and measure the wrong footprint.
serve(){                     # serve <label> <ready-check> <server cmd...>
  local label=$1 ready=$2; shift 2
  # Server output goes to a log, never to stdout: this function's stdout IS
  # the pid, and redis-server's banner would be captured along with it.
  "$@" >"$OUT/$label.server.log" 2>&1 & local sp=$!
  for _ in $(seq 60); do eval "$ready" >/dev/null 2>&1 && break; sleep 1; done
  echo "$sp"
}

if have pagerank; then say "pagerank on kron22"
  OMP_NUM_THREADS=$THREADS census --label "pagerank (kron22)" \
    --cmd "$W/gapbs/pr -f $GRAPH -n 100000 -i 20" --out "$OUT/pagerank.csv"
fi

if have bfs; then say "bfs on kron22"
  OMP_NUM_THREADS=$THREADS census --label "bfs (kron22)" \
    --cmd "$W/gapbs/bfs -f $GRAPH -n 100000" --out "$OUT/bfs.csv"
fi

if have llama; then say "llama.cpp, tinyllama 1.1B"
  # -n -1 --context-shift --ignore-eos: generates until killed, so the run
  # length is the census's decision and not the model's. Without --ignore-eos
  # it stops at the first EOS -- measured, it lasted 30 s. Weights are mmap'd
  # and never written: the cleanest write-cold case we have. 1,214 MiB
  # resident, 79 tok/s on 16 threads.
  census --label "llama.cpp (tinyllama 1.1B)" --cmd \
    "$W/llama.cpp/build/bin/llama-cli -m $W/models/tinyllama-1.1b-q4.gguf \
     -t $THREADS -c 4096 -n -1 -st --context-shift --ignore-eos --no-warmup \
     -p Write_a_long_technical_report_about_memory_systems." \
    --out "$OUT/llama.csv"
fi

# Both servers get the SAME generator: identical keys, value size, zipfian
# skew and read ratio, so the two censuses differ by the server and nothing
# else. (YCSB-C is built but unused for this -- its redis binding inserts two
# keys in 45 s here, on its own shipped spec.)
KEYS=${KEYS:-700000}; VSZ=${VSZ:-1400}          # ~1.0 GiB resident either way

if have redis; then say "redis, 95/5 zipfian, ~1 GiB"
  SP=$(serve redis "$W/redis-src/src/redis-cli ping" \
       "$W/redis-src/src/redis-server" --save '' --appendonly no --protected-mode no)
  "$R/scripts/kv_load.py" --proto redis --keys "$KEYS" --value "$VSZ" \
      --read-ratio 0.95 --threads 8 & LP=$!
  sleep 45                                     # let the load phase populate
  census --label "redis (95/5 zipfian)" --pid "$SP" --out "$OUT/redis.csv"
  kill $LP $SP 2>/dev/null; wait $SP 2>/dev/null
fi

if have memcached; then say "memcached, 95/5 zipfian, ~1 GiB"
  SP=$(serve memcached "true" "$W/memcached/memcached" -m 2048 -t 8 -u nobody)
  sleep 2
  "$R/scripts/kv_load.py" --proto memcached --keys "$KEYS" --value "$VSZ" \
      --read-ratio 0.95 --threads 8 & LP=$!
  sleep 45
  census --label "memcached (95/5 zipfian)" --pid "$SP" --out "$OUT/memcached.csv"
  kill $LP $SP 2>/dev/null
fi

say "plotting"
"$R/scripts/plot_census.py" "$OUT"/*.csv -o "$OUT"
echo "csv + figure in $OUT"
