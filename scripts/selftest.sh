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
# The re-exec above makes $HOME /root, but matmul and the backend live in the
# INVOKING user's home. sudo leaves SUDO_USER set, so resolve through that.
REAL_HOME=$(getent passwd "${SUDO_USER:-$(id -un)}" | cut -d: -f6)
MM=$REAL_HOME/matmul
KO=$REAL_HOME/nor_eci/nor_eci_fulltest_ltram.ko
EC=/usr/local/sbin/ltram-erase-counts
F=/var/lib/ltram/erase_counts
EPF=/var/lib/ltram/wear_epoch
STAMP=/var/lib/ltram/selftest-pre.txt
PASS=0; FAIL=0; SKIP=0
ok(){   printf "  \033[32mPASS\033[0m %s\n" "$*"; PASS=$((PASS+1)); }
no(){   printf "  \033[31mFAIL\033[0m %s\n" "$*"; FAIL=$((FAIL+1)); }
skip(){ printf "  \033[33mSKIP\033[0m %s\n" "$*"; SKIP=$((SKIP+1)); }
hdr(){  printf "\n\033[1m%s\033[0m\n" "$*"; }
w(){ awk -v k="$1" '$1==k{print $2; exit}' $DBG/wear; }
ps_(){ awk -v k="$1" '$1==k{print $2; exit}' $DBG/pagestate; }
gs(){ awk -v k="$1" '$1==k{print $2; exit}' /sys/kernel/ltram/stats; }
setp(){ echo "$2" | sudo -n tee $PAR/$1 >/dev/null; }
near(){ awk -v a="$1" -v b="$2" -v t="$3" 'BEGIN{d=a-b; if(d<0)d=-d; exit !(d<=t)}'; }

[ -r $DBG/wear ] || { echo "no $DBG/wear -- is the wear-governor kernel booted?"; exit 1; }
# Fail loudly on version skew between this script and the kernel. When the
# budget fields were renamed erases_* -> cycles_*, an unshipped script read
# every one of them as empty: section A reported four failures with blank
# values, and D5 "passed" by comparing one empty string to another. A false
# pass is worse than a crash.
for f in cycles_total cycles_used cycles_left seconds_left interval_ms epoch; do
    grep -q "^$f " $DBG/wear || {
        echo "!! $DBG/wear has no '$f' field -- this script and the running"
        echo "   kernel disagree about the schema. Ship the matching selftest.sh."
        echo; sed 's/^/     /' $DBG/wear; exit 1; }
done
MODE=${1:-full}
if [ "$MODE" != "--quick" ] && [ "$MODE" != "--pre" ] && [ "$MODE" != "--post" ]; then
    [ -x "$MM" ] || { echo "no matmul at $MM -- sections E and G need it"; exit 1; }
    [ -f "$KO" ] || { echo "no backend module at $KO"; exit 1; }
fi

# Restore every knob we touch, however we leave.
E0=$(w epoch); D0=$(w service_days); C0=$(w cycles_per_sect); G0=$(cat $PAR/wear_governor)
LW0=$(cat $PAR/erase_low_water); HW0=$(cat $PAR/erase_high_water)
restore(){ setp wear_epoch $E0; setp wear_days $D0; setp wear_cycles $C0
           setp wear_governor $G0; setp erase_low_water $LW0; setp erase_high_water $HW0; }
# INT and TERM too, not just EXIT. A Ctrl-C part way through left matmul
# running in the background with target_pid still pointing at it -- the knobs
# came back but the workload did not, so the scanner kept promoting into a
# process nobody was measuring.
cleanup(){
    pkill -x matmul 2>/dev/null
    echo 0 > /sys/kernel/ltram/target_pid 2>/dev/null
    restore
}
trap cleanup EXIT INT TERM

# =====================================================================
# The reboot pair. Persistence is the one design that cannot be tested
# inside a single boot, because the thing being tested IS the boot.
NPG=$(( $(cat $DBG/end_pfn) - $(cat $DBG/start_pfn) ))
if [ "$MODE" = "--pre" ]; then
    hdr "PRE-REBOOT STAMP"
    [ -x $EC ] || { echo "  erase-count unit not installed -- nothing to test"; exit 1; }
    sudo -n $EC save >/dev/null || { echo "  save FAILED"; exit 1; }
    { echo "used $(w cycles_used)"
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
    N_USED=$(w cycles_used)
    [ "$NPG" = "$P_PG" ] && ok "R1 window is the same size ($NPG pages)"         || no "R1 window changed $P_PG -> $NPG; a blob from the old size is refused BY DESIGN"
    # A LOWER bound: the shutdown save may have caught erases after the stamp,
    # and a few sectors can be erased at boot before the restore lands.
    [ "${N_USED:-0}" -gt 0 ]         && ok "R2 counts survived the power cycle (used = $N_USED, not 0)"         || no "R2 used = 0 -- nothing was restored; check ExecStop ran"
    [ "${N_USED:-0}" -ge "$P_USED" ]         && ok "R3 restored count >= the stamp ($N_USED >= $P_USED)"         || no "R3 restored $N_USED is BELOW the stamp $P_USED -- more was lost than the shutdown delta"
    # The epoch is a date, not a measurement: it must come back bit-identical.
    # A kernel default that quietly reasserts itself changes seconds_left, and
    # the whole budget is cycles_left divided by that.
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

TOT=$(w cycles_total); USED=$(w cycles_used); LEFT=$(w cycles_left)
NP=$(( $(cat $DBG/end_pfn) - $(cat $DBG/start_pfn) ))
[ "$TOT" = "$((NP * C0))" ] && ok "A1 cycles_total = nr_pages x cycles ($NP x $C0)" \
    || no "A1 cycles_total $TOT != $((NP * C0))"
[ "$LEFT" = "$((TOT - USED))" ] && ok "A2 cycles_left = total - used" \
    || no "A2 cycles_left $LEFT != $((TOT - USED))"

# cycles_used must equal the sum of the per-sector counts, or the budget is
# being computed from a number that drifted away from the array it describes.
SUM=$(sudo -n cat $DBG/erase_counts | od -An -tu4 -j16 -v | tr -s ' ' '\n' | \
      grep -v '^$' | awk '{s+=$1} END{print s+0}')
[ "$SUM" = "$USED" ] && ok "A3 cycles_used matches the sum of erase_counts ($SUM)" \
    || no "A3 cycles_used $USED != sum of erase_counts $SUM -- the O(1) total drifted"

# interval = 1000 * seconds_left / cycles_left, the whole governor in one line
SL=$(w seconds_left); IV=$(w interval_ms)
EXP=$(awk -v s="$SL" -v l="$LEFT" 'BEGIN{printf "%d", 1000*s/l}')
near "$IV" "$EXP" 1 && ok "A4 interval_ms $IV = 1000 x seconds_left / cycles_left ($EXP)" \
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
    SZ=$(stat -c %s $F 2>/dev/null || echo 0)
    [ "$SZ" = "$((16 + 4*NP))" ] && ok "D1 blob is 16 + 4 x $NP = $SZ bytes" \
        || no "D1 blob is $SZ, want $((16 + 4*NP))"
    H=$(head -c16 $F | od -An -tx4 | tr -s ' ')
    echo "$H" | grep -q "4c544543" && ok "D2 header magic is LTEC" || no "D2 header: $H"
    # A blob whose header does not describe THIS kernel must be refused. That
    # is the guard the kernel actually implements: magic, version and nr_pages
    # are validated on the FIRST write and the whole transfer is rejected.
    #
    # This used to feed it a truncated blob and read dd's exit status, which
    # asked the wrong question twice over. The kernel's first write consumes
    # exactly the 16-byte header and returns 16, so dd sees a partial write,
    # reports 0+1 records out, and exits 0 -- while the body never went in and
    # lt_rebuild_buckets was never called. Nothing was accepted and nothing
    # was corrupted; the test just could not tell.
    cp $F /tmp/bad.blob
    printf '\x00\x00\x00\x00' | dd of=/tmp/bad.blob bs=1 seek=8 count=4 conv=notrunc 2>/dev/null
    if cat /tmp/bad.blob > $DBG/erase_counts 2>/dev/null; then
        no "D3 kernel ACCEPTED a blob whose header claims nr_pages=0"
    else ok "D3 kernel refuses a blob with a mismatched header"; fi
    rm -f /tmp/bad.blob
    dmesg 2>/dev/null | tail -30 | grep -q "erase-count blob rejected" \
        && ok "D3b kernel logged the rejection" || skip "D3b no rejection line in dmesg"
    sudo -n $EC restore >/dev/null 2>&1 && ok "D4 full blob restores" || no "D4 restore failed"
    near "$(w cycles_used)" "$USED" 200 \
        && ok "D5 cycles_used survives a restore ($(w cycles_used) vs $USED)" \
        || no "D5 cycles_used $(w cycles_used) != $USED after restore"
    if [ -f $EPF ]; then
        [ "$(cat $EPF)" = "$(w epoch)" ] && ok "D6 saved epoch matches the live parameter ($(w epoch))" || no "D6 saved epoch $(cat $EPF) != live $(w epoch)"
    else no "D6 no $EPF -- restore has never stamped the epoch"; fi
    DATA2=$(ps_ data); DIRTY2=$(ps_ dirty); CLEAN2=$(ps_ clean)
    [ $((DATA2 + DIRTY2 + CLEAN2)) = "$NP" ] \
        && ok "D7 invariant still holds after the restore tests" \
        || no "D7 $DATA2+$DIRTY2+$CLEAN2 != $NP -- a restore desynced the free lists"
else skip "D  erase-count unit not installed (run ~/ltram-systemd/install.sh)"; fi

[ "$MODE" = "--quick" ] && { hdr "RESULT"; echo "  $PASS passed, $FAIL failed, $SKIP skipped"; exit $((FAIL>0)); }

# =====================================================================
hdr "E. SCANNER -- rate, batch size, and the no-clean-sector stall"

lsmod | grep -q nor_eci || sudo -n insmod $KO \
    provide_ops=1 test=0 inline_erase=0 verify_erased=1 2>/dev/null
SCANPID=$(pgrep -f ltram_scan | head -1)

# One promotion per tick at the governed interval. 8 MiB = 2048 pages.
# At wear_days=40 the interval is ~0.5 ms, so this finishes in seconds
# instead of the 49 s the five-year budget would take.
# Promotions per second, sampled MID-RUN from a monotonic counter.
#
# The first version read the `data` page count before and after the matmul
# process, which measured almost nothing: the process exits at the end of the
# run, every page it promoted is freed on exit, and `data` falls back to zero
# before the second sample is taken. It reported 1.0/s at a 5 ms interval that
# should give 200/s, and called the governor broken.
#
# dst_allocated is cumulative and survives the exit. Sampling twice DURING the
# run also avoids the other trap: at a 1 ms interval a 2,048-page working set
# is fully promoted in two seconds, so a whole-run average measures the
# working set rather than the budget.
promo_rate(){   # $1 = wear_days
    setp wear_days $1
    local a0 a1 t0 t1
    sudo -n $MM --n 2896 --iters 1 --runs 4000 --print-ranges --phys > /tmp/st.log 2>&1 &
    local bg=$!
    for i in $(seq 1 200); do grep -q "^RANGE" /tmp/st.log 2>/dev/null && break; sleep 0.1; done
    local pid=$(pgrep -x matmul | head -1)
    [ -n "${pid:-}" ] && echo $pid | sudo -n tee /sys/kernel/ltram/target_pid >/dev/null
    sleep 3                                     # let the fill get going
    a0=$(gs dst_allocated); t0=$(date +%s.%N)
    sleep 6
    a1=$(gs dst_allocated); t1=$(date +%s.%N)
    kill $bg 2>/dev/null; wait $bg 2>/dev/null
    echo 0 | sudo -n tee /sys/kernel/ltram/target_pid >/dev/null
    awk -v a="$a0" -v b="$a1" -v x="$t0" -v y="$t1" 'BEGIN{printf "%.1f", (b-a)/(y-x)}'
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
setp erase_high_water 65536; setp erase_low_water 65535
# Wait for the engine to actually start before timing it. It was idle, so its
# worker is re-armed at erase_idle_ms (1000 ms) and will not notice the new
# watermark for up to a second -- a tenth of a short window, charged against
# the rate as if the device were slow.
for i in $(seq 1 30); do
    e=$(awk '/erases_done/{print $2}' /sys/kernel/ltram/stats); sleep 0.5
    [ "$(awk '/erases_done/{print $2}' /sys/kernel/ltram/stats)" != "$e" ] && break
done
ed0=$(awk '/erases_done/{print $2}' /sys/kernel/ltram/stats); tf0=$(date +%s.%N)
sleep 20
ed1=$(awk '/erases_done/{print $2}' /sys/kernel/ltram/stats); tf1=$(date +%s.%N)
RATE=$(awk -v a="$ed0" -v b="$ed1" -v x="$tf0" -v y="$tf1" 'BEGIN{printf "%.0f", (b-a)/(y-x)}')
if [ "$(ps_ dirty)" -gt 1500 ]; then
    # NOT the theoretical 1/16.4ms = 61/s. That ignores the idle gate, which
    # samples the device for 3 ms before committing each erase. The drains in
    # the measurement campaign ran at ~44/s sustained (60011 -> 54415 dirty in
    # 126 s), so that is the number this should be judged against.
    [ "$RATE" -ge 25 ] && ok "F1 idle engine erases at ${RATE}/s (campaign drains ran ~44/s)" \
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
