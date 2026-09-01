#!/bin/bash
# The counterexample: a cache engine's READ path dirties the page it reads.
#
#   ./scripts/readpath_probe.sh [outdir]
#
# memcached bumps it->time and relinks the LRU on every GET; redis writes
# robj->lru on every lookupKey. Both fields live in the same page as the data,
# so "95% reads" at the application level is not 95% reads at the page level,
# and cold and write-cold collapse onto each other.
#
# Four conditions per engine, with idle measured BEFORE and AFTER the traffic.
# That bracket is the control: without it, the first idle reading was 98.6%,
# which is the post-bulk-load LRU maintainer crawl and not steady state at all.
set -u
R="$(cd "$(dirname "$0")/.." && pwd)"; W="$R/workloads"
OUT="${1:-$R/baselines/$(date +%y%m%d)_census/readpath}"; mkdir -p "$OUT"
KEYS=${KEYS:-200000}; VSZ=${VSZ:-1400}; HOT=${HOT:-1000}
WIN=${WIN:-15}; SETTLE=${SETTLE:-90}

stale=$(pgrep -f "llama-cli|kv_load.py|redis-server|memcached/memcached" | grep -cv "^$$\$")
[ "$stale" = 0 ] || { echo "!! $stale workload process(es) already running; kill them first"; exit 1; }

cat > "$OUT/.probe.py" <<'PY'
import sys, time, numpy as np
pid = int(sys.argv[1]); PAGE = 4096; SECS = float(sys.argv[4])
def regs():
    R = []
    for l in open(f"/proc/{pid}/maps"):
        p = l.split(); n = p[5] if len(p) > 5 else ""
        if n in ("[vsyscall]", "[vvar]", "[vdso]"): continue
        lo, hi = (int(x, 16) for x in p[0].split("-")); R.append((lo, hi))
    return R
open(f"/proc/{pid}/clear_refs", "w").write("4"); time.sleep(SECS)
tot = d = 0
with open(f"/proc/{pid}/pagemap", "rb") as f:
    for lo, hi in regs():
        f.seek((lo // PAGE) * 8); raw = f.read(((hi - lo) // PAGE) * 8)
        if len(raw) < 8: continue
        e = np.frombuffer(raw[:len(raw) // 8 * 8], dtype=np.uint64)
        pm = (e & np.uint64(1 << 63)) != 0; sd = (e & np.uint64(1 << 55)) != 0
        tot += int(pm.sum()); d += int((pm & sd).sum())
pct = 100.0 * d / max(tot, 1)
print(f"  {sys.argv[3]:<32s} {pct:5.1f}% written  ({d:,} of {tot:,} pages)")
open(sys.argv[5], "a").write(f"{sys.argv[2]},{sys.argv[3]},{d},{tot},{pct:.2f}\n")
PY

CSV="$OUT/readpath.csv"
echo "engine,condition,pages_written,pages_resident,pct_written" > "$CSV"

one(){                        # one <engine> <proto> <port> <server cmd...>
  local eng=$1 proto=$2 port=$3; shift 3
  "$@" >"$OUT/$eng.server.log" 2>&1 & local SP=$!
  sleep 3
  "$R/scripts/kv_load.py" --proto "$proto" --port "$port" --keys "$KEYS" \
      --value "$VSZ" --threads 8 --load-only 2>/dev/null
  echo "  (settling ${SETTLE}s after the bulk load)"; sleep "$SETTLE"
  echo "$eng, $KEYS keys x $VSZ B, ${WIN}s per condition:"
  python3 "$OUT/.probe.py" "$SP" "$eng" "A1. idle, no client traffic" "$WIN" "$CSV"
  for n in "$HOT" "$KEYS"; do
    local lab="B. GETs, $n hot keys only"
    [ "$n" = "$KEYS" ] && lab="C. GETs, all $KEYS keys"
    "$R/scripts/kv_load.py" --proto "$proto" --port "$port" --keys "$n" \
        --no-load --read-ratio 1.0 --threads 8 >/dev/null 2>&1 & local L=$!
    sleep 2; python3 "$OUT/.probe.py" "$SP" "$eng" "$lab" "$WIN" "$CSV"
    kill $L 2>/dev/null; sleep 3
  done
  python3 "$OUT/.probe.py" "$SP" "$eng" "A2. idle again" "$WIN" "$CSV"
  kill $SP 2>/dev/null; sleep 2
}

one memcached memcached 11299 "$W/memcached/memcached" -m 2048 -t 8 -p 11299
echo
one redis redis 6399 "$W/redis-src/src/redis-server" --save '' --appendonly no --port 6399
rm -f "$OUT/.probe.py"
echo; echo "wrote $CSV"
