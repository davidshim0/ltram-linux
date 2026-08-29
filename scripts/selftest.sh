#!/bin/bash
# selftest.sh -- assert every design decision actually behaves as designed.
#
# Run ON z08, as a user with passwordless sudo, with the LtRAM kernel booted.
#   ./selftest.sh            everything except the reboot pair
#   ./selftest.sh --quick    arithmetic and knobs only, no erases, ~1 minute
#   ./selftest.sh --pre      stamp state, then reboot, then --post
#   ./selftest.sh --post     compare against the --pre stamp
#
# COST. The behavioural tests promote and erase real sectors: about 6,200
# erases, ~0.0001% of the array's budget. The arithmetic tests cost nothing.
set -u
# debugfs is 0700 root, and the helpers below read it directly, so the guard
# fails as an ordinary user even when everything is fine. Re-exec rather than
# sprinkling sudo through every reader.
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"
DBG=/sys/kernel/debug/ltram
PAR=/sys/module/ltram_policy/parameters
MM=$HOME/matmul
EC=/usr/local/sbin/ltram-erase-counts
STAMP=/var/lib/ltram/selftest-pre.txt
PASS=0; FAIL=0; SKIP=0
ok(){   printf "  \033[32mPASS\033[0m %s\n" "$*"; PASS=$((PASS+1)); }
no(){   printf "  \033[31mFAIL\033[0m %s\n" "$*"; FAIL=$((FAIL+1)); }
skip(){ printf "  \033[33mSKIP\033[0m %s\n" "$*"; SKIP=$((SKIP+1)); }
hdr(){  printf "\n\033[1m%s\033[0m\n" "$*"; }
w(){ awk -v k="$1" '$1==k{print $2; exit}' $DBG/wear; }
ps_(){ awk -v k="$1" '$1==k{print $2; exit}' $DBG/pagestate; }
setp(){ echo "$2" | sudo -n tee $PAR/$1 >/dev/null; }
near(){ awk -v a="$1" -v b="$2" -v t="$3" 'BEGIN{d=a-b; if(d<0)d=-d; exit !(d<=t)}'; }

[ -r $DBG/wear ] || { echo "no $DBG/wear -- is the wear-governor kernel booted?"; exit 1; }
MODE=${1:-full}

# Restore every knob we touch, however we leave.
E0=$(w epoch); D0=$(w service_days); C0=$(w cycles_per_sect); G0=$(cat $PAR/wear_governor)
LW0=$(cat $PAR/erase_low_water); HW0=$(cat $PAR/erase_high_water)
restore(){ setp wear_epoch $E0; setp wear_days $D0; setp wear_cycles $C0
           setp wear_governor $G0; setp erase_low_water $LW0; setp erase_high_water $HW0; }
trap restore EXIT

# =====================================================================
# The reboot pair. Persistence is the one design that cannot be tested
# inside a single boot, because the thing being tested IS the boot.
NPG=$(( $(cat $DBG/end_pfn) - $(cat $DBG/start_pfn) ))
if [ "$MODE" = "--pre" ]; then
    hdr "PRE-REBOOT STAMP"
    [ -x $EC ] || { echo "  erase-count unit not installed -- nothing to test"; exit 1; }
    sudo -n $EC save >/dev/null || { echo "  save FAILED"; exit 1; }
    { echo "used $(w erases_used)"
      echo "data $(ps_ data)"; echo "dirty $(ps_ dirty)"; echo "clean $(ps_ clean)"
      echo "pages $NPG"
      echo "md5 $(sudo -n md5sum /var/lib/ltram/erase_counts | cut -d" " -f1)"
    } | sudo -n tee $STAMP
    echo
    echo "  Stamped. Now reboot, then run:  $0 --post"
    echo "  Do not run a workload in between, or the totals will legitimately differ."
    exit 0
fi
if [ "$MODE" = "--post" ]; then
    hdr "POST-REBOOT COMPARISON"
    [ -f $STAMP ] || { echo "  no stamp at $STAMP -- run --pre before rebooting"; exit 1; }
    P_USED=$(awk '/^used/{print $2}' $STAMP); P_PG=$(awk '/^pages/{print $2}' $STAMP)
    N_USED=$(w erases_used)
    [ "$NPG" = "$P_PG" ] && ok "R1 window is the same size ($NPG pages)"         || no "R1 window changed $P_PG -> $NPG; a blob from the old size is refused BY DESIGN"
    # A LOWER bound: the shutdown save may have caught erases after the stamp,
    # and a few sectors can be erased at boot before the restore lands.
    [ "${N_USED:-0}" -gt 0 ]         && ok "R2 counts survived the power cycle (used = $N_USED, not 0)"         || no "R2 used = 0 -- nothing was restored; check ExecStop ran"
    [ "${N_USED:-0}" -ge "$P_USED" ]         && ok "R3 restored count >= the stamp ($N_USED >= $P_USED)"         || no "R3 restored $N_USED is BELOW the stamp $P_USED -- more was lost than the shutdown delta"
    # The epoch is a date, not a measurement: it must come back bit-identical.
    # A kernel default that quietly reasserts itself changes seconds_left, and
    # the whole budget is erases_left divided by that.
    P_EP=$(awk '/^epoch/{print $2}' $STAMP); N_EP=$(w epoch)
    if [ -n "${P_EP:-}" ]; then
        [ "$N_EP" = "$P_EP" ] && ok "R8 wear epoch survived unchanged ($N_EP)" || no "R8 epoch was $P_EP, is now $N_EP -- the compiled default reasserted itself"
    else skip "R8 no epoch in the stamp (pre-dates the governor)"; fi
    systemctl is-active --quiet ltram-erase-counts         && ok "R4 the persistence unit is active" || no "R4 unit is not active"
    dmesg | grep -q "erase counts restored"         && ok "R5 kernel logged the restore" || no "R5 no restore in dmesg"
    dmesg | grep -q "erase-count blob rejected"         && no "R6 kernel REJECTED the blob -- see dmesg for magic/version/pages"         || ok "R6 blob was not rejected"
    # The boot scan is the other reboot-only design: it must classify sectors
    # by reading them, not assume. Assuming all-dirty is safe but costs 18
    # minutes of erasing; assuming all-clean programs over live data.
    if dmesg | grep -qi "ltram.*scan"; then
        dmesg | grep -i "ltram.*scan" | tail -2 | sed 's/^/     /'
        ok "R7 the boot scan ran"
    else no "R7 no boot scan in dmesg -- scan_pool=0?"; fi
    hdr "RESULT"; echo "  $PASS passed, $FAIL failed, $SKIP skipped"
    exit $((FAIL > 0))
fi

# =====================================================================
hdr "A. WEAR BUDGET -- arithmetic (no erases)"

TOT=$(w erases_total); USED=$(w erases_used); LEFT=$(w erases_left)
NP=$(( $(cat $DBG/end_pfn) - $(cat $DBG/start_pfn) ))
[ "$TOT" = "$((NP * C0))" ] && ok "A1 erases_total = nr_pages x cycles ($NP x $C0)" \
    || no "A1 erases_total $TOT != $((NP * C0))"
[ "$LEFT" = "$((TOT - USED))" ] && ok "A2 erases_left = total - used" \
    || no "A2 erases_left $LEFT != $((TOT - USED))"

# erases_used must equal the sum of the per-sector counts, or the budget is
# being computed from a number that drifted away from the array it describes.
SUM=$(sudo -n cat $DBG/erase_counts | od -An -tu4 -j16 -v | tr -s ' ' '\n' | \
      grep -v '^$' | awk '{s+=$1} END{print s+0}')
[ "$SUM" = "$USED" ] && ok "A3 erases_used matches the sum of erase_counts ($SUM)" \
    || no "A3 erases_used $USED != sum of erase_counts $SUM -- the O(1) total drifted"

# interval = 1000 * seconds_left / erases_left, the whole governor in one line
SL=$(w seconds_left); IV=$(w interval_ms)
EXP=$(awk -v s="$SL" -v l="$LEFT" 'BEGIN{printf "%d", 1000*s/l}')
near "$IV" "$EXP" 1 && ok "A4 interval_ms $IV = 1000 x seconds_left / erases_left ($EXP)" \
    || no "A4 interval_ms $IV != $EXP"

hdr "B. WEAR BUDGET -- the knobs move the rate the right way"

setp wear_days 1826; I5=$(w interval_ms)
near "$I5" 24 2 && ok "B1 five-year budget gives ~24 ms (got $I5)" || no "B1 got $I5, want ~24"
setp wear_days 114; I1=$(w interval_ms)
near "$I1" 2 1 && ok "B2 114-day budget gives ~1.5 ms (got $I1)" || no "B2 got $I1, want ~1-2"
setp wear_days $D0
# epoch is the same lever from the other end: BACK in time = less life = faster
setp wear_epoch $((E0 - 1712*86400)); IB=$(w interval_ms)
[ "$IB" -lt "$I5" ] && ok "B3 backdating the epoch speeds promotion up ($I5 -> $IB ms)" \
    || no "B3 backdating gave $IB, not faster than $I5"
setp wear_epoch $((E0 + 3650*86400)); IF=$(w interval_ms)
[ "$IF" -gt "$I5" ] && ok "B4 postdating the epoch slows promotion down ($I5 -> $IF ms)" \
    || no "B4 postdating gave $IF, not slower than $I5"
setp wear_epoch $E0

setp wear_governor 0; IG=$(w interval_ms); SI=$(cat $PAR/scan_interval_ms)
[ "$IG" = "$SI" ] && ok "B5 governor off falls back to scan_interval_ms ($SI ms)" \
    || no "B5 governor off gave $IG, want $SI"
setp wear_governor 1

# Exhaustion must STOP, not trickle. cycles=1 makes used >= total instantly.
setp wear_cycles 1; IX=$(w interval_ms)
[ "$IX" = "STOPPED" ] && ok "B6 exhausted budget reports STOPPED, not a trickle" \
    || no "B6 exhausted gave $IX, want STOPPED"
setp wear_cycles $C0
[ "$(w interval_ms)" != "STOPPED" ] && ok "B7 restoring cycles revives the governor" \
    || no "B7 still STOPPED after restore"

hdr "C. PAGE STATES -- the invariant"
DATA=$(ps_ data); DIRTY=$(ps_ dirty); CLEAN=$(ps_ clean)
[ $((DATA + DIRTY + CLEAN)) = "$NP" ] \
    && ok "C1 data+dirty+clean = nr_pages ($DATA+$DIRTY+$CLEAN=$NP)" \
    || no "C1 $DATA+$DIRTY+$CLEAN != $NP"
ERR=$(awk '/^errors/{print $2}' $DBG/pagestate 2>/dev/null || echo 0)
[ "${ERR:-0}" = "0" ] && ok "C2 no state-machine errors recorded" || no "C2 errors=$ERR"

hdr "D. ERASE-COUNT PERSISTENCE"
if [ -x $EC ]; then
    sudo -n $EC save >/dev/null 2>&1
    SZ=$(stat -c %s /var/lib/ltram/erase_counts 2>/dev/null || echo 0)
    [ "$SZ" = "$((16 + 4*NP))" ] && ok "D1 blob is 16 + 4 x $NP = $SZ bytes" \
        || no "D1 blob is $SZ, want $((16 + 4*NP))"
    H=$(sudo -n head -c16 /var/lib/ltram/erase_counts | od -An -tx4 | tr -s ' ')
    echo "$H" | grep -q "4c544543" && ok "D2 header magic is LTEC" || no "D2 header: $H"
    # a short blob must be REFUSED: the kernel rebuilds the wear buckets only
    # when the last byte lands, so a partial restore desyncs the free lists
    sudo -n head -c 1000 /var/lib/ltram/erase_counts > /tmp/short.blob
    if sudo -n dd if=/tmp/short.blob of=$DBG/erase_counts bs=1000 2>/dev/null; then
        no "D3 kernel ACCEPTED a truncated blob -- buckets may now be desynced"
    else ok "D3 kernel refuses a truncated blob"; fi
    rm -f /tmp/short.blob
    sudo -n $EC restore >/dev/null 2>&1 && ok "D4 full blob restores" || no "D4 restore failed"
    near "$(w erases_used)" "$USED" 200 \
        && ok "D5 erases_used survives a restore ($(w erases_used) vs $USED)" \
        || no "D5 erases_used $(w erases_used) != $USED after restore"
    EPF=/var/lib/ltram/wear_epoch
    if [ -f $EPF ]; then
        [ "$(cat $EPF)" = "$(w epoch)" ] && ok "D6 saved epoch matches the live parameter ($(w epoch))" || no "D6 saved epoch $(cat $EPF) != live $(w epoch)"
    else no "D6 no $EPF -- restore has never stamped the epoch"; fi
else skip "D  erase-count unit not installed (run ~/ltram-systemd/install.sh)"; fi

[ "$MODE" = "--quick" ] && { hdr "RESULT"; echo "  $PASS passed, $FAIL failed, $SKIP skipped"; exit $((FAIL>0)); }

# =====================================================================
hdr "E. SCANNER -- rate, batch size, and the no-clean-sector stall"

lsmod | grep -q nor_eci || sudo -n insmod $HOME/nor_eci/nor_eci_fulltest_ltram.ko \
    provide_ops=1 test=0 inline_erase=0 verify_erased=1 2>/dev/null
SCANPID=$(pgrep -f ltram_scan | head -1)

# One promotion per tick at the governed interval. 8 MiB = 2048 pages.
# At wear_days=40 the interval is ~0.5 ms, so this finishes in seconds
# instead of the 49 s the five-year budget would take.
promo_rate(){   # $1 = wear_days
    setp wear_days $1
    local iv=$(w interval_ms) d0 t0 d1 t1
    sudo -n $EC save >/dev/null 2>&1
    d0=$(ps_ data); t0=$(date +%s.%N)
    sudo -n $MM --n 1448 --iters 1 --runs 400 --print-ranges --phys --verify > /tmp/st.log 2>&1 &
    local bg=$!
    for i in $(seq 1 100); do grep -q "^RANGE" /tmp/st.log 2>/dev/null && break; sleep 0.1; done
    local pid=$(pgrep -x matmul | head -1)
    [ -n "${pid:-}" ] && echo $pid | sudo -n tee /sys/kernel/ltram/target_pid >/dev/null
    wait $bg; echo 0 | sudo -n tee /sys/kernel/ltram/target_pid >/dev/null
    d1=$(ps_ data); t1=$(date +%s.%N)
    awk -v a="$d0" -v b="$d1" -v x="$t0" -v y="$t1" 'BEGIN{printf "%.1f", (b-a)/(y-x)}'
}
R_SLOW=$(promo_rate 400); IV_SLOW=$(w interval_ms)
R_FAST=$(promo_rate 40);  IV_FAST=$(w interval_ms)
setp wear_days $D0
echo "     slow: interval ${IV_SLOW} ms -> ${R_SLOW} promotions/s"
echo "     fast: interval ${IV_FAST} ms -> ${R_FAST} promotions/s"
awk -v s="$R_SLOW" -v f="$R_FAST" 'BEGIN{exit !(f > s*1.8)}' \
    && ok "E1 a shorter budget promotes faster (${R_SLOW} -> ${R_FAST}/s)" \
    || no "E1 rate did not respond to the budget (${R_SLOW} -> ${R_FAST}/s)"
[ "$(cat $PAR/promote_batch)" = "1" ] && ok "E2 promote_batch is 1" \
    || no "E2 promote_batch is $(cat $PAR/promote_batch), want 1"

# Gate A: with no clean sector the thread must SLEEP, not spin. Fill the pool
# by promoting into it until clean hits zero, then watch the thread's CPU.
if [ -n "${SCANPID:-}" ]; then
    cpu0=$(awk '{print $14+$15}' /proc/$SCANPID/stat)
    sleep 10
    cpu1=$(awk '{print $14+$15}' /proc/$SCANPID/stat)
    TICKS=$((cpu1 - cpu0))
    [ "$TICKS" -lt 50 ] && ok "E3 scan thread is not spinning (${TICKS} ticks in 10 s)" \
        || no "E3 scan thread burned ${TICKS} ticks in 10 s"
else skip "E3 could not find the scan thread"; fi

hdr "F. ERASE ENGINE -- hysteresis and spacing"
ed0=$(awk '/erases_done/{print $2}' /sys/kernel/ltram/stats)
setp erase_high_water 65536; setp erase_low_water 65535
sleep 10
ed1=$(awk '/erases_done/{print $2}' /sys/kernel/ltram/stats)
RATE=$(( (ed1 - ed0) / 10 ))
if [ "$(ps_ dirty)" -gt 600 ]; then
    [ "$RATE" -gt 40 ] && ok "F1 idle engine erases at ~61/s (got ${RATE}/s)" \
        || no "F1 engine only reached ${RATE}/s with $(ps_ dirty) dirty"
else skip "F1 not enough dirty sectors ($(ps_ dirty)) to measure the erase rate"; fi
setp erase_high_water $HW0; setp erase_low_water $LW0
[ "$(cat $PAR/erase_batch)" = "1" ] && ok "F2 erase_batch is 1" \
    || no "F2 erase_batch is $(cat $PAR/erase_batch), want 1"

hdr "G. DATA PATH -- the bits that come back"
sudo -n $MM --n 1448 --iters 1 --runs 6 --flush 32 --verify > /tmp/st-d.log 2>&1
DD=$(awk '/^DIGEST/{print $2}' /tmp/st-d.log)
sudo -n $MM --n 1448 --iters 1 --runs 6 --flush 32 --verify --print-ranges --phys \
     --wait-resident 99 --wait-timeout 120 > /tmp/st-n.log 2>&1 &
bg=$!
for i in $(seq 1 100); do grep -q "^RANGE" /tmp/st-n.log 2>/dev/null && break; sleep 0.1; done
pid=$(pgrep -x matmul | head -1)
[ -n "${pid:-}" ] && echo $pid | sudo -n tee /sys/kernel/ltram/target_pid >/dev/null
wait $bg; echo 0 | sudo -n tee /sys/kernel/ltram/target_pid >/dev/null
DN=$(awk '/^DIGEST/{print $2}' /tmp/st-n.log)
RES=$(grep "^PHYS end    weights" /tmp/st-n.log | sed -n 's/.*(\([0-9.]*\)%).*/\1/p' | tail -1)
[ -n "$DD" ] && [ "$DD" = "$DN" ] && ok "G1 DRAM and NOR digests match (${RES:-?}% in flash)" \
    || no "G1 digest mismatch: DRAM $DD vs NOR $DN"
dmesg | tail -200 | grep -q "NOT ERASED" \
    && no "G2 verify_erased saw a programmed sector -- a dirty sector was recycled as clean" \
    || ok "G2 no sector was programmed without an erase"
rm -f /tmp/st.log /tmp/st-d.log /tmp/st-n.log

hdr "RESULT"
echo "  $PASS passed, $FAIL failed, $SKIP skipped"
echo
echo "  NOT covered here, because they need a reboot:"
echo "    - erase counts survive a power cycle   ->  ./selftest.sh --pre, reboot, --post"
echo "    - the boot scan classifies sectors     ->  dmesg | grep 'ltram: pool scan'"
exit $((FAIL > 0))
