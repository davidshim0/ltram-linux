#!/bin/bash
# probe_stalls.sh -- localise the base-case tail without assuming a cause.
#
# DRAM only. No flash, no promotion, no erase engine: this is the base case,
# and until it is understood the NOR numbers rest on an unmeasured floor.
#
# Two runs, identical except one has the memory access removed:
#   real  timestamped dependent loads
#   null  same loop, same two clock reads, no load at all
#
# Then: are the stalls periodic in time, positional in the loop, and do they
# survive with the memory access gone?
set -u
[ "$(id -u)" = 0 ] || exec sudo -E "$0" "$@"   # nice -n -20 needs root
SCRATCH=${SCRATCH:-/scratch/${SUDO_USER:-$(id -un)}/ltram}
mkdir -p "$SCRATCH" 2>/dev/null || SCRATCH=/tmp   # z08 root is 4.4 GB and full

# Refuse to measure on a machine that is not in a state where the result would
# mean anything. Costs milliseconds; has already caught a stale binary, a
# missing nohz_full, and a leftover run from a previous session.
PREFLIGHT=$(dirname "$0")/preflight.sh
if [ -x "$PREFLIGHT" ]; then
    "$PREFLIGHT" --quiet || { echo "!! preflight failed -- run $PREFLIGHT for detail"; exit 1; }
fi
SECS=${SECS:-60}
PIN=${PIN:-47}
# Resolve the invoking user's home: under sudo, $HOME is root's.
REAL_HOME=$(getent passwd "${SUDO_USER:-$(id -un)}" | cut -d: -f6)
MM=${MM:-$REAL_HOME/matmul}
N=${N:-2048}
THRESH=${THRESH:-5000}          # ns; record anything slower
OUT=${OUT:-$SCRATCH/stallprobe}
[ -x "$MM" ] || { echo "no binary at $MM"; exit 1; }
mkdir -p $OUT

run(){  # $1 = tag, $2... = extra flags
    local tag=$1; shift
    echo "  $tag: ${SECS}s on CPU$PIN ..."
    timeout $SECS taskset -c $PIN nice -n -20 \
        $MM --n $N --iters 1 --runs 100000 --chase --chase-hist \
            --slow-ns $THRESH "$@" > $OUT/$tag.log 2>&1
    printf "     %s SLOW records, %s passes\n" \
        "$(grep -c '^SLOW' $OUT/$tag.log)" "$(grep -c '^SECT' $OUT/$tag.log)"
}

echo "probe: N=$N ($(( N * N * 4 / 1048576 )) MiB), threshold ${THRESH} ns, pinned to CPU$PIN"
run real
run null --null-load
echo
# Attribution, now that localisation is done. Everything else has been excluded:
# the stalls are quantised to a single period, uniform in loop position, absent
# from gap/scrub, and they survive with the memory access removed. What remains
# is "some external source at 1 kHz", and this counts the candidates directly.
echo "  interrupt deltas on CPU$PIN over one ${SECS}s run:"
snap(){ awk -v cpu="CPU$PIN" '
    NR==1 { for (i=1;i<=NF;i++) if ($i==cpu) c=i+1; next }
    c && NF>=c { n=$1; sub(/:$/,"",n); print n, $c }' /proc/interrupts; }
snap > $OUT/irq.0
timeout $SECS taskset -c $PIN nice -n -20 \
    $MM --n $N --iters 1 --runs 100000 --chase --chase-hist > /dev/null 2>&1
snap > $OUT/irq.1
join -j1 $OUT/irq.0 $OUT/irq.1 \
  | awk -v t="$SECS" '{d=$3-$2; if (d>0) printf "    %-10s %9d  %7.0f/s\n", $1, d, d/t}' \
  | sort -k2 -nr | head -6
echo "    (irq 11 is arch_timer on this board; CONFIG_HZ=1000)"
echo
echo "wrote $OUT/real.log and $OUT/null.log"
