#!/bin/bash
# Why is memcached the WORST workload in fig10, not the easiest?
#
#   ./scripts/mc_ablate.sh
#
# fig10 has memcached at 56% write-cold at 1 min and 0.2% by 5, below redis at
# every T -- the opposite of what the workload chapter predicted. There are
# five things in memcached that write without the client asking, and guessing
# which one is a good way to fix the wrong one. So: turn each off in turn and
# see which moves the number.
#
#   do_item_update relinking   a GET on a COLD item calls item_unlink_q(), which
#                              rewrites the prev and next NEIGHBOURS' pointers,
#                              then relinks into WARM and writes that head. The
#                              neighbours are arbitrary items elsewhere in the
#                              slab, so one read can dirty several unrelated
#                              pages. Disabled by no_lru_maintainer (which turns
#                              off segmentation with it) and by no_modern.
#   lru_maintainer_thread      background LRU rebalancing.   -o no_lru_maintainer
#   item_crawler_thread        background LRU crawl.          -o no_lru_crawler
#   slab_rebalance_thread      moves pages between classes.   -o no_slab_reassign
#   assoc_maintenance_thread   hash expansion.                -o no_hashexpand
#
# Each condition is measured the same way: load, settle, drive pure GETs, and
# count what fraction of resident pages went soft-dirty in a fixed window. The
# GET-only traffic is deliberate -- with zero client writes, anything dirty is
# memcached writing on its own behalf.
#
# ALSO reports write amplification: distinct pages dirtied per distinct item
# read. 1.0 means a read dirties only its own item's page. Much above 1 is the
# relinking hypothesis; near 0 with traffic means the background threads are
# doing it, not the read path.
set -u
cd "$(dirname "$0")/.."
W=workloads; MC="$W/memcached/memcached"; PORT=${PORT:-11311}
KEYS=${KEYS:-200000}; VSZ=${VSZ:-140}; WIN=${WIN:-20}; HOT=${HOT:-2000}
OUT=${OUT:-baselines/$(date +%y%m%d)_mcablate}; mkdir -p "$OUT"
[ -x "$MC" ] || { echo "!! no $MC"; exit 1; }

cat > "$OUT/.probe.py" <<'PY'
import sys, time, os, numpy as np
pid=int(sys.argv[1]); secs=float(sys.argv[2]); PAGE=4096
def regs():
    R=[]
    for l in open(f"/proc/{pid}/maps"):
        p=l.split(); n=p[5] if len(p)>5 else ""
        if n in ("[vsyscall]","[vvar]","[vdso]"): continue
        lo,hi=(int(x,16) for x in p[0].split("-")); R.append((lo,hi))
    return R
open(f"/proc/{pid}/clear_refs","w").write("4"); time.sleep(secs)
tot=d=0
with open(f"/proc/{pid}/pagemap","rb") as f:
    for lo,hi in regs():
        f.seek((lo//PAGE)*8); raw=f.read(((hi-lo)//PAGE)*8)
        if len(raw)<8: continue
        e=np.frombuffer(raw[:len(raw)//8*8],dtype=np.uint64)
        pm=(e&np.uint64(1<<63))!=0; sd=(e&np.uint64(1<<55))!=0
        tot+=int(pm.sum()); d+=int((pm&sd).sum())
print(f"{d} {tot}")
PY

CSV="$OUT/ablation.csv"
echo "condition,flags,idle_pct,hot_pct,full_pct,pages_per_item,resident_pages" > "$CSV"

run(){                                   # run <name> <extra -o flags...>
    local name=$1; shift
    local -a OPT=(); [ $# -gt 0 ] && OPT=(-o "$(IFS=,; echo "$*")")
    $MC -m 2048 -t 8 -p $PORT "${OPT[@]}" >"$OUT/$name.log" 2>&1 & local SP=$!
    sleep 2
    kill -0 $SP 2>/dev/null || { echo "  $name: memcached refused these flags"; return; }
    ./scripts/kv_load.py --proto memcached --port $PORT --keys $KEYS --value $VSZ \
        --threads 8 --load-only 2>/dev/null
    sleep 90                              # past the post-load LRU crawl
    read di ti < <(python3 "$OUT/.probe.py" $SP $WIN)          # A: idle
    ./scripts/kv_load.py --proto memcached --port $PORT --keys $HOT --no-load \
        --read-ratio 1.0 --threads 8 >/dev/null 2>&1 & local L=$!
    sleep 2; read dh th < <(python3 "$OUT/.probe.py" $SP $WIN); kill $L 2>/dev/null
    sleep 3
    ./scripts/kv_load.py --proto memcached --port $PORT --keys $KEYS --no-load \
        --read-ratio 1.0 --threads 8 >/dev/null 2>&1 & L=$!
    sleep 2; read df tf < <(python3 "$OUT/.probe.py" $SP $WIN); kill $L 2>/dev/null
    kill $SP 2>/dev/null; sleep 2
    # pages dirtied by the hot run, above idle, per distinct item in the hot set
    local amp=$(awk -v dh="$dh" -v di="$di" -v h="$HOT" 'BEGIN{printf "%.2f", (dh-di)/h}')
    printf "  %-16s idle %5.1f%%  hot(%s keys) %5.1f%%  full %5.1f%%  pages/item %s\n" \
        "$name" "$(awk -v a=$di -v b=$ti 'BEGIN{printf "%.1f",100*a/b}')" "$HOT" \
        "$(awk -v a=$dh -v b=$th 'BEGIN{printf "%.1f",100*a/b}')" \
        "$(awk -v a=$df -v b=$tf 'BEGIN{printf "%.1f",100*a/b}')" "$amp"
    echo "$name,$*,$(awk -v a=$di -v b=$ti 'BEGIN{printf "%.2f",100*a/b}'),$(awk -v a=$dh -v b=$th 'BEGIN{printf "%.2f",100*a/b}'),$(awk -v a=$df -v b=$tf 'BEGIN{printf "%.2f",100*a/b}'),$amp,$tf" >> "$CSV"
}

echo "== ablation: $KEYS keys, ${WIN}s windows, pure GETs (no client writes at all)"
run baseline
run no_crawler      no_lru_crawler
run no_maintainer   no_lru_maintainer
run no_slab_move    no_slab_reassign
run no_hashexpand   no_hashexpand
run no_modern       no_modern
run all_off         no_lru_crawler no_lru_maintainer no_slab_reassign no_hashexpand
rm -f "$OUT/.probe.py"
echo; echo "  -> $CSV"
