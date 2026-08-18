#!/bin/sh
# cache-sweep.sh -- locate the cache knees of this machine by working-set size.
#
# WHY NOT CONSTANT ITERATIONS. Work per iteration is N^2, so holding --iters
# fixed makes total work scale as N^2: across 4 KiB .. 64 MiB that is a 16,384x
# spread, and the wall-clock numbers would be incomparable. What is held fixed
# here is TOTAL WORK -- iters x N^2 ~ ELEMS -- so every point does the same
# number of inner-loop elements, lands in the same time band, and the printed
# means are directly comparable without arithmetic. (ns/element is invariant
# either way; constant work is what keeps the timer resolution and the run
# lengths sane.)
#
# WHAT IT CAN AND CANNOT SEE. matmul's inner loop is a serial FMA dependency
# chain, ~7.2 cycles/element at 2.0 GHz, so memory is hidden wherever the cache
# keeps up. Expect a SMALL step at the L1 boundary (~9% on measurements so far)
# and a LARGE ramp once the working set passes L2. The L1 step is still ~600x
# the observed run-to-run noise, so it is resolvable -- this sweep is how the
# L1 size gets settled, since the Cavium boot stub reports L2 (16384 KB) and
# RCLK (2000 MHz) but not L1.
#
# The L2 is shared by all 48 cores. taskset pins the core; it cannot reserve
# cache. The machine must be otherwise idle or the >8 MiB points are noise.
#
# IS EACH POINT ISOLATED? Yes, and not because anything is flushed. Each point
# is a separate process, and every point FILLS its entire weight region before
# the clock starts -- the fill is the scrub. Lines left by the previous point
# survive only in sets the new W does not map to, are never re-referenced, and
# age out within the first iteration or two of the hundreds-to-millions this
# sweep runs. There is no unprivileged full-cache-invalidate on aarch64 to use
# instead (the same constraint that forces CVMCACHE with a physical address for
# the flash read-back verify), so a scrub buffer would be the only mechanism --
# and the fill already is one.
#
# Rather than assert that, TEST it: REVERSE=1 walks the sweep from 64 MiB down
# to 4 KiB. If any point were contaminated by its predecessor, ascending and
# descending would disagree. Agreement is evidence; a flush would only have been
# a claim.
#
# usage:  ./cache-sweep.sh [matmul-binary]
# env:    RUNS=3   CPU=0   REVERSE=1   SWEEP="N:iters ..." to override points

MATMUL=${1:-${MATMUL:-./matmul}}
RUNS=${RUNS:-3}
CPU=${CPU:-0}

# N:iters, chosen so 4*N*N lands on 4 KiB .. 64 MiB and iters*N*N ~ 1e9
SWEEP=${SWEEP:-"32:976562 45:493827 64:244141 78:164366 90:123457 110:82645 \
128:61035 156:41091 181:30524 256:15259 362:7631 512:3815 724:1908 1024:954 \
1448:477 1773:318 2048:238 2289:191 2508:159 2896:119 3547:79 4096:60"}
[ -x "$MATMUL" ] || { echo "no matmul at $MATMUL" >&2; exit 2; }

if [ -n "$REVERSE" ]; then
    SWEEP=$(printf '%s\n' $SWEEP | tac | tr '\n' ' ')
    echo "# descending: 64 MiB -> 4 KiB (isolation cross-check)"
fi

thp=/sys/kernel/mm/transparent_hugepage/enabled
if [ -r $thp ] && ! grep -q '\[never\]' $thp; then
    echo "WARNING: THP is $(cat $thp) -- points >= 2 MiB may be huge-page backed" >&2
    echo "         and points below cannot be, putting a TLB step in the middle" >&2
    echo "         of the sweep. Pin it: echo never | sudo tee $thp" >&2
fi
if command -v taskset >/dev/null 2>&1; then PIN="taskset -c $CPU"; else PIN=""
    echo "WARNING: no taskset -- core migration will blur the small points" >&2
fi

printf '%8s %6s %9s %9s %7s %9s %8s  %s\n' \
       WORKSET N ITERS MEAN_S SD_PCT NS_ELEM VS_PREV DIGEST
prev=""
for pair in $SWEEP; do
    N=${pair%%:*}; IT=${pair##*:}
    if ! out=$($PIN "$MATMUL" --n "$N" --iters "$IT" --runs "$RUNS" \
                    --verify --protect-weights 2>&1); then
        echo "FAILED at N=$N" >&2; printf '%s\n' "$out" >&2; exit 1
    fi
    mean=$(printf '%s\n' "$out" | awk '/^RESULT/{print $3}')
    sdp=$(printf  '%s\n' "$out" | awk '/^RESULT/{gsub(/[()%]/,"",$8); print $8}')
    dig=$(printf  '%s\n' "$out" | awk '/^DIGEST/{print substr($2,1,12)}')
    [ -n "$mean" ] || { echo "no RESULT line at N=$N" >&2; exit 1; }

    ns=$(awk -v m="$mean" -v i="$IT" -v n="$N" 'BEGIN{printf "%.3f", m*1e9/(i*n*n)}')
    ws=$(awk -v n="$N" 'BEGIN{w=4*n*n
             if (w<1048576) printf "%.0fK", w/1024; else printf "%.0fM", w/1048576}')
    if [ -n "$prev" ]; then
        rel=$(awk -v a="$ns" -v b="$prev" 'BEGIN{printf "%+.1f%%", 100*(a/b-1)}')
    else
        rel="--"
    fi
    printf '%8s %6d %9d %9s %7s %9s %8s  %s\n' \
           "$ws" "$N" "$IT" "$mean" "$sdp" "$ns" "$rel" "$dig"
    prev=$ns
done

echo
echo "NS_ELEM is the comparable column. VS_PREV is the step from the previous"
echo "working set: a knee is a sustained jump, not a single noisy point."
