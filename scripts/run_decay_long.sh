#!/bin/bash
# Cold and write-cold out to T = 60 minutes.
#
#   ./scripts/run_decay_long.sh
#
# The 600 s ladder ran out of range exactly where the interesting workloads
# were still moving: redis was at 7% and still falling at 10 min, memcached had
# already bottomed at 0.2%, and the three compute workloads were flat from 60 s
# on. So this goes to 60 min, and it runs redis and memcached FIRST -- if the
# night is cut short, the two that still had somewhere to go are the two that
# finished.
#
# Passes differ by workload, on measured grounds rather than taste. Spread
# across passes in the 600 s run: llama 0.04, redis 0.77, bfs 1.14, pagerank
# 2.40, memcached 1.74 with a max of 17.16. So the KV pair gets two passes and
# the compute three get one.
#
#   redis      2 passes   ~2h 4m
#   memcached  2 passes   ~2h 4m
#   pagerank   1 pass     ~1h 1m
#   bfs        1 pass     ~1h 1m
#   llama      1 pass     ~1h 1m
#                         -------
#                         ~7h 11m
set -u
cd "$(dirname "$0")/.."
BASE=baselines/$(date +%y%m%d)_census/long
LAD=60,120,180,300,600,900,1200,1800,2400,3000,3600     # 1..60 min, 11 points
mkdir -p "$BASE"; shopt -s nullglob

plot(){ local c=("$BASE"/*/*.csv); [ ${#c[@]} -gt 0 ] || return
        ./scripts/plot_census.py "${c[@]}" -o docs/figures || true
        cp docs/figures/fig10*.png "$BASE"/ 2>/dev/null || true; }

echo "############ 1/2  redis + memcached, 2 passes  ($(date +%H:%M)) ############"
LADDER=$LAD PASSES=2 DECAY_OUT="$BASE/kv" ./scripts/run_decay.sh redis memcached \
  || echo "!! kv leg aborted"
echo; echo "== figures after the KV leg ($(date +%H:%M)) =="; plot

echo; echo "############ 2/2  pagerank + bfs + llama, 1 pass  ($(date +%H:%M)) ############"
LADDER=$LAD PASSES=1 DECAY_OUT="$BASE/compute" ./scripts/run_decay.sh pagerank bfs llama \
  || echo "!! compute leg aborted"

echo; echo "== FINAL ($(date +%H:%M)) =="; plot
for f in "$BASE"/*/*.csv; do echo; echo "--- $f"; column -s, -t "$f"; done
