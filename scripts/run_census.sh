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
R="$(cd "$(dirname "$0")/.." && pwd)"
# On z08 the workloads are under /scratch, not in the repo: root is 4.4 GB with
# ~300 MB free and the graph alone is 522 MB.
W="${LTRAM_W:-$R/workloads}"
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
# Trace-shaped: millions of small objects at a moderate per-instance rate,
# which is the published memcached/Twitter shape. Our earlier 700k x 1400 B
# was the outlier. Note the offered rate cannot open a gap between cold and
# write-cold in these engines -- a read dirties the page, so it moves both
# together. These runs exist to quantify that, not to support the claim.
KEYS=${KEYS:-7000000}; VSZ=${VSZ:-140}; RATE=${RATE:-10000}
SETTLE=${SETTLE:-150}

kv(){                        # kv <name> <proto> <port> <server cmd...>
  local name=$1 proto=$2 port=$3; shift 3
  SP=$(serve "$name" "true" "$@")
  sleep 3
  echo "  loading $KEYS keys x $VSZ B ..."
  "$R/scripts/kv_load.py" --proto "$proto" --port "$port" --keys "$KEYS" \
      --value "$VSZ" --threads 8 --load-only || { kill $SP; return; }
  # A bulk load leaves memcached's LRU maintainer crawling every slab: it
  # dirtied 98.6% of pages in the 15 s after a load and 0.1% once settled.
  # Starting the census before that settles measures the crawl, not the load.
  echo "  settling ${SETTLE}s (post-load maintenance)"; sleep "$SETTLE"
  "$R/scripts/kv_load.py" --proto "$proto" --port "$port" --keys "$KEYS" \
      --value "$VSZ" --no-load --read-ratio 0.95 --ops-per-sec "$RATE" \
      --threads 8 >/dev/null 2>&1 & LP=$!
  sleep 15
  awk '/^VmRSS/{printf "  %s resident: %d MiB\n", "'"$name"'", $2/1024}' /proc/$SP/status
  census --label "$name (95/5 zipfian, $RATE ops/s)" --pid "$SP" --out "$OUT/$name.csv"
  kill $LP $SP 2>/dev/null; wait $SP 2>/dev/null
}

if have redis; then say "redis, 95/5 zipfian at $RATE ops/s"
  kv redis redis 6379 "$W/redis-src/src/redis-server" --save '' --appendonly no \
     --protected-mode no --maxmemory 0
fi

if have memcached; then say "memcached, 95/5 zipfian at $RATE ops/s"
  kv memcached memcached 11211 "$W/memcached/memcached" -m 4096 -t 8 -u nobody
fi

say "plotting"
"$R/scripts/plot_census.py" "$OUT"/*.csv -o "$OUT"
echo "csv + figure in $OUT"
