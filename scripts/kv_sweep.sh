#!/bin/bash
# redis and memcached across the YCSB parameter space.
#
#   ./scripts/kv_sweep.sh              both sweeps, redis
#   WL=memcached ./scripts/kv_sweep.sh
#
# TWO SWEEPS, because they answer different questions.
#
# SKEW, at YCSB-B's 95/5. uniform, 0.5, 0.8, 0.99, 1.2. Measured concentration
# on 200k keys: uniform puts 3% of traffic on the top 1% of keys, 0.5 puts 8%,
# 0.8 puts 28%, 0.99 puts 52%. YCSB's default is 0.99 and it is what everyone
# reports against, so it is the point to quote; the rest say how much the
# answer depends on that choice.
#
# READ RATIO, at theta=0.99. YCSB-C (100/0), B (95/5), A (50/50). C is the
# interesting one: a workload with NO application writes at all. If its pages
# still come out write-hot, that is the lookup path writing metadata --
# measured at 95.1% of redis pages dirtied by pure GETs -- and it is the
# cleanest demonstration that "read-only workload" does not mean "read-only
# pages".
#
# x86 only: this needs soft-dirty, which arm64 does not have. decay_census
# refuses there rather than reporting 100%.
set -u
cd "$(dirname "$0")/.."
WL=${WL:-redis}
KEYS=${KEYS:-7000000}; VSZ=${VSZ:-140}; RATE=${RATE:-10000}
LADDER=${LADDER:-60,120,300,600}; PASSES=${PASSES:-2}
OUT=${OUT:-baselines/$(date +%y%m%d)_kvsweep}; mkdir -p "$OUT"
W=workloads

case "$WL" in
  redis)     SRV=("$W/redis-src/src/redis-server" --save '' --appendonly no --protected-mode no); PORT=6379 ;;
  memcached) SRV=("$W/memcached/memcached" -m 4096 -t 8); PORT=11211 ;;
  *) echo "usage: WL=[redis|memcached] $0"; exit 2 ;;
esac
[ -x "${SRV[0]}" ] || { echo "!! no ${SRV[0]}"; exit 1; }

one(){                                   # one <tag> <dist> <theta> <readratio>
    local tag=$1 dist=$2 th=$3 rr=$4
    echo; echo "== $tag  ($WL, dist=$dist theta=$th read=$rr)"
    "${SRV[@]}" >"$OUT/$tag.server.log" 2>&1 & local SP=$!
    sleep 3
    ./scripts/kv_load.py --proto "$WL" --port $PORT --keys "$KEYS" --value "$VSZ" \
        --threads 8 --load-only || { kill $SP; return; }
    # A bulk load leaves memcached's LRU maintainer crawling every slab: 98.6%
    # of pages dirtied in the 15 s after, 0.1% once settled.
    echo "  settling 150s"; sleep 150
    ./scripts/kv_load.py --proto "$WL" --port $PORT --keys "$KEYS" --value "$VSZ" \
        --no-load --dist "$dist" --zipf "$th" --read-ratio "$rr" \
        --ops-per-sec "$RATE" --threads 8 >/dev/null 2>&1 & local LP=$!
    sleep 15
    ./scripts/decay_census.py --pid $SP --ladder "$LADDER" --passes "$PASSES" \
        --label "$WL $tag" --out "$OUT/$tag.csv"
    kill $LP $SP 2>/dev/null; sleep 3
    [ -s "$OUT/$tag.csv" ] && column -s, -t "$OUT/$tag.csv" | sed 's/^/    /'
}

echo "### skew sweep, YCSB-B read ratio (95/5)"
one skew-uniform  uniform 0.99 0.95
one skew-t050     zipfian 0.5  0.95
one skew-t080     zipfian 0.8  0.95
one skew-t099     zipfian 0.99 0.95        # YCSB default -- also serves as ratio-B
one skew-t120     zipfian 1.2  0.95

echo; echo "### read-ratio sweep, theta 0.99"
one ratio-C-100r  zipfian 0.99 1.00        # YCSB-C: no application writes at all
one ratio-A-50r   zipfian 0.99 0.50        # YCSB-A

echo; echo "  -> $OUT"
