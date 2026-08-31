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
SECS=${SECS:-60}
PIN=${PIN:-47}
# Resolve the invoking user's home: under sudo, $HOME is root's.
REAL_HOME=$(getent passwd "${SUDO_USER:-$(id -un)}" | cut -d: -f6)
MM=${MM:-$REAL_HOME/matmul.probe}
N=${N:-2048}
THRESH=${THRESH:-5000}          # ns; record anything slower
OUT=${OUT:-/tmp/stallprobe}
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
echo "interrupt counts on CPU$PIN during the run were NOT sampled here on purpose:"
echo "this probe is meant to localise first and attribute second."
echo
echo "wrote $OUT/real.log and $OUT/null.log"
