#!/bin/bash
# write-cold against T, measured with the LtRAM scanner instead of soft-dirty.
#
#   sudo ~/scan_census.sh pagerank
#   LADDER=5,10,30,60,120 PASSES=8 sudo ~/scan_census.sh bfs
#
# WHY NOT soft-dirty. It is a SOFTWARE bit set by the write-fault handler, and
# only x86, s390 and powerpc select HAVE_ARCH_SOFT_DIRTY -- arm64 has no spare
# PTE bit, 55-58 being DIRTY, SPECIAL, DEVMAP and PROT_NONE. On this board
# clear_soft_dirty() compiles to an empty function, so clear_refs=4 does
# nothing and every workload reads 100% write-cold.
#
# WHY THE SCANNER WORKS ANYWAY. It already implements the same mechanism with
# write permission as the bit, which is what HAFDBS=0 forced on it:
#
#   written = pte_write(p);            /* armed and not trapped since */
#   if (written) ptep_set_wrprotect(); /* re-arm, ONLY if written */
#
# The re-arm being conditional is what makes the aggregate a census. In any
# pass, a page written during the last interval is writable and counted in
# was_written, then re-armed; a page not written stays protected and counts
# clean. So
#
#   write_cold(T) = 1 - d(was_written) / d(ptes_examined),  T = scan_interval_ms
#
# and sweeping the interval sweeps T. One T per run -- the scanner keeps no
# per-page age, deliberately -- so this loops over the ladder for you.
set -u
[ "$(id -u)" = 0 ] || exec sudo -E "$0" "$@"
WL=${1:-pagerank}
W=${LTRAM_W:-/scratch/hushim/workloads}
OUT=${OUT:-/scratch/hushim/ltram/baselines/$(date +%y%m%d)_scancensus}
PAR=/sys/module/ltram_policy/parameters; DBG=/sys/kernel/debug/ltram
KO=$HOME/nor_eci/nor_eci_fulltest_ltram.ko
LADDER=${LADDER:-5,10,30,60,120,300}
PASSES=${PASSES:-8}
GRAPH="$W/gapbs/benchmark/graphs/kron22.sg"

gs(){ awk -v k="$1" '$1==k{print $2; exit}' /sys/kernel/ltram/stats; }
ps_(){ awk -v k="$1" '$1==k{print $2; exit}' $DBG/pagestate; }
setp(){ echo "$2" > $PAR/$1; }

[ -r $DBG/wear ] || { echo "!! no $DBG -- wrong kernel?"; exit 1; }
lsmod | grep -q nor_eci || insmod "$KO" provide_ops=1 test=0 inline_erase=0 verify_erased=1 \
    || { echo "!! cannot load the backend"; exit 1; }

G0=$(cat $PAR/wear_governor); S0=$(cat $PAR/scan_interval_ms)
P0=$(cat $PAR/scan_ptes_per_pass); B0=$(cat $PAR/promote_batch)
cleanup(){ [ -n "${BG:-}" ] && kill $BG 2>/dev/null
           echo 0 > /sys/kernel/ltram/target_pid 2>/dev/null
           setp wear_governor $G0; setp scan_interval_ms $S0
           setp scan_ptes_per_pass $P0; setp promote_batch $B0
           [ -n "${E0:-}" ] && { echo "$E0" > $PAR/erase_high_water
                                 echo "$E1" > $PAR/erase_low_water; } }
trap cleanup EXIT INT TERM

case "$WL" in
  pagerank) CMD=("$W/gapbs/pr" -f "$GRAPH" -n 5000000 -i 20) ;;
  bfs)      CMD=("$W/gapbs/bfs" -f "$GRAPH" -n 5000000) ;;
  *) echo "usage: $0 [pagerank|bfs]"; exit 2 ;;
esac
[ -x "${CMD[0]}" ] || { echo "!! no ${CMD[0]}"; exit 1; }

mkdir -p "$OUT"; CSV="$OUT/$WL.csv"
echo "T_sec,write_cold_pct,write_cold_sd,passes,ptes_examined,was_written,promoted" > "$CSV"

"${CMD[@]}" >/dev/null 2>&1 & BG=$!
sleep 45
RSS=$(awk '/^VmRSS/{print int($2/4)}' /proc/$BG/status)
echo "  $WL pid $BG, ~$RSS resident pages"

# One pass must cover the whole working set, or T does not mean what it says:
# a sweep that needs three passes to come back around observes a 3T window.
setp scan_ptes_per_pass $(( RSS + RSS / 2 ))
setp wear_governor 0            # governor off -> scan_interval_ms IS the interval
# promote_batch bounds the WALK, not just the promotions:
#
#   for (; addr < end && ctx->nr < promote_batch
#          && ctx->examined < scan_ptes_per_pass; ...)
#
# ctx->nr increments on every page CHOSEN, so promote_batch=1 stops the walk at
# the first clean page it finds. On a 94%-clean workload that is after one or
# two PTEs, and the first run of this script duly reported write-cold over
# 2-page samples: 83.04% +/- 32.83.
#
# So it has to be larger than the number of clean pages in a sweep. The way to
# have that without promoting them is to leave NO clean sectors: candidates are
# still chosen and walked past, and every migration fails.
setp promote_batch $(( RSS * 2 ))
echo $BG > /sys/kernel/ltram/target_pid

# The scan thread refuses to walk when there is nowhere to put a page:
#
#     if (!READ_ONCE(lt_clean_count)) { msleep(scan_stall_ms); continue; }
#
# so draining the pool does not make the scanner passive, it stops it. The
# first attempt at that measured 0 passes.
#
# But the check is at the TOP of the thread loop, not inside the walk. A pass
# that starts with a few clean sectors walks all of scan_ptes_per_pass,
# promotes until clean runs out, and fails the rest. So hold clean small and
# refilled: watermarks 64/1 turn the engine on at clean < 1 and off at 64, and
# 64 sectors refill in ~3 s against a minimum T of 5 s.
#
# Cost, stated rather than hidden: about 64 pages promoted per pass, so ~2-3%
# of a 141k-page working set across the whole ladder. The script reports the
# running total per T so it can be checked rather than assumed.
setw(){ echo "$1" > $PAR/erase_high_water; echo "$2" > $PAR/erase_low_water; }
E0=$(cat $PAR/erase_high_water); E1=$(cat $PAR/erase_low_water)
setw 64 1
echo "  clean held at 64 (engine on below 1); expect ~64 promotions per pass"
sleep 10
echo "  clean now $(ps_ clean), $(gs moved_to_ltram) promoted so far"

for T in ${LADDER//,/ }; do
    kill -0 $BG 2>/dev/null || { echo "  !! workload exited"; break; }
    setp scan_interval_ms $(( T * 1000 ))
    sleep $(( T * 2 ))                       # two passes to settle the arming
    e0=$(gs ptes_examined); w0=$(gs was_written); m0=$(gs moved_to_ltram)
    vals=()
    for p in $(seq 1 $PASSES); do
        pe=$(gs ptes_examined); pw=$(gs was_written)
        sleep $T
        de=$(( $(gs ptes_examined) - pe )); dw=$(( $(gs was_written) - pw ))
        # A pass that examined a handful of PTEs is not a sample of anything.
        # 1000 keeps the per-pass ratio meaningful without demanding a full sweep.
        [ "$de" -ge 1000 ] && vals+=( "$(awk -v a="$dw" -v b="$de" 'BEGIN{printf "%.2f", 100*(1-a/b)}')" )
    done
    e1=$(gs ptes_examined); w1=$(gs was_written); m1=$(gs moved_to_ltram)
    read M SD < <(printf '%s\n' "${vals[@]}" | awk '{v[n++]=$1; s+=$1}
        END{if(!n){print "nan nan"; exit} m=s/n; for(i=0;i<n;i++) q+=(v[i]-m)^2
            printf "%.2f %.2f\n", m, (n>1?sqrt(q/(n-1)):0)}')
    echo "$T,$M,$SD,${#vals[@]},$((e1-e0)),$((w1-w0)),$((m1-m0))" >> "$CSV"
    printf "  T=%-4ss  write-cold %6s%% +/- %-5s  (%s passes, %s promoted)\n" \
           "$T" "$M" "$SD" "${#vals[@]}" "$((m1-m0))"
done
kill $BG 2>/dev/null
echo; echo "  -> $CSV"
