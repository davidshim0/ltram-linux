#!/bin/bash
# preflight.sh -- is this machine in a state where a measurement means anything?
#
# The failure mode this exists for is not a crash. It is a run that completes,
# produces plausible numbers, and measured the wrong thing: a stale matmul that
# ignores a flag, a kernel booted without nohz_full so every tail is timer, a
# backend loaded with inline_erase=1 so every erase is charged twice. All three
# have happened here, and none of them announced themselves.
#
#   ./preflight.sh              check and report
#   ./preflight.sh --quiet      print only failures; for use inside other scripts
#   ./preflight.sh --fix        also do what can be fixed without a reboot
#
# Exit codes are the interface:
#   0  ready
#   1  something is wrong that a reboot will NOT fix (stale binary, module, procs)
#   2  needs a reboot -- and says whether the boot config is already correct
#
# Other scripts should start with:
#   "$(dirname "$0")/preflight.sh" --quiet || exit 1
set -u
PIN=${PIN:-47}
REAL_HOME=$(getent passwd "${SUDO_USER:-$(id -un)}" | cut -d: -f6)
MM=${MM:-$REAL_HOME/matmul}
GW=${GW:-hushim@enzian-gateway.inf.ethz.ch}
GRUBCFG=${GRUBCFG:-/srv/tftp/boot/grub/grub.cfg-C0A8C008}
WANT_ARGS="nohz_full=$PIN isolcpus=$PIN rcu_nocbs=$PIN irqaffinity=0-$((PIN-1))"

QUIET=0; FIX=0
for a in "$@"; do case "$a" in --quiet) QUIET=1;; --fix) FIX=1;; esac; done
FAIL=0; REBOOT=0
ok(){   [ $QUIET = 1 ] || printf "  \033[32mok\033[0m    %s\n" "$*"; }
bad(){  printf "  \033[31mFAIL\033[0m  %s\n" "$*"; FAIL=1; }
need(){ printf "  \033[33mBOOT\033[0m  %s\n" "$*"; REBOOT=1; }
note(){ [ $QUIET = 1 ] || printf "        %s\n" "$*"; }

[ $QUIET = 1 ] || echo "preflight: CPU$PIN, $MM"

# ---- 1. CPU isolation -------------------------------------------------------
NF=$(cat /sys/devices/system/cpu/nohz_full 2>/dev/null)
IS=$(cat /sys/devices/system/cpu/isolated  2>/dev/null)
[ "$NF" = "$PIN" ] && ok "nohz_full=$PIN"  || need "nohz_full is [${NF:-empty}], want $PIN"
[ "$IS" = "$PIN" ] && ok "isolated=$PIN"   || need "isolated is [${IS:-empty}], want $PIN"

# ---- 2. nothing left over from a previous run -------------------------------
# pgrep -c PRINTS 0 and EXITS 1 when nothing matches, so "|| echo 0" appends a
# second zero and the variable becomes "0\n0", which every numeric test then
# rejects. Same trap grep -c set for this project once already. Capture the
# output and ignore the status.
STALE=$(pgrep -c -f '[m]atmul|[s]weep_|[s]elftest\.sh|[p]robe_stalls' 2>/dev/null)
STALE=${STALE:-0}
case "$STALE" in ''|*[!0-9]*) STALE=0;; esac
if [ "$STALE" -eq 0 ]; then ok "no stale measurement processes"
elif [ $FIX = 1 ]; then
    pkill -9 -f '[m]atmul|[s]weep_|[s]elftest\.sh|[p]robe_stalls' 2>/dev/null
    sleep 1; ok "killed $STALE stale process(es)"
else
    bad "$STALE stale process(es) running -- they will corrupt this run (use --fix)"
    pgrep -af '[m]atmul|[s]weep_|[s]elftest\.sh|[p]robe_stalls' | sed 's/^/        /'
fi

# ---- 3. does isolation actually WORK? --------------------------------------
# Config says what was asked for; this measures what happens. A busy task on the
# isolated CPU should see ~0 timer interrupts, not ~1000/s.
tick(){ awk -v c="CPU$PIN" 'NR==1{for(i=1;i<=NF;i++) if($i==c) k=i+1; next}
                            $1=="11:"{print $k}' /proc/interrupts; }
if [ "${STALE:-0}" -ne 0 ] && [ $FIX = 0 ]; then
    note "skipping the timer test: another task is on CPU$PIN, which brings the"
    note "tick back legitimately. Clear it (--fix) and re-run."
    T0=0; T1=0; RATE=0
else
T0=$(tick); taskset -c $PIN timeout 2 bash -c 'while :; do :; done' 2>/dev/null; T1=$(tick)
RATE=$(( (${T1:-0} - ${T0:-0}) / 2 ))
fi
if [ "${STALE:-0}" -ne 0 ] && [ $FIX = 0 ]; then :
elif [ "$RATE" -le 50 ]; then ok "timer on CPU$PIN under load: ${RATE}/s"
else need "timer on CPU$PIN under load: ${RATE}/s -- isolation not in effect"; fi

# ---- 3. is a reboot enough, or does the boot config need editing first? -----
if [ $REBOOT = 1 ]; then
    MISS=""
    for a in $WANT_ARGS; do grep -qw -- "$a" /proc/cmdline || MISS="$MISS $a"; done
    if [ -n "$MISS" ]; then note "running cmdline is missing:$MISS"
    else note "the running cmdline already has every argument -- the isolation"
         note "failure above is not a boot problem; look at the note beside it."; fi
    if OUT=$(ssh -o ConnectTimeout=10 -o BatchMode=yes -q "$GW" "cat $GRUBCFG" 2>/dev/null); then
        GMISS=""
        for a in $WANT_ARGS; do case "$OUT" in *"$a"*) ;; *) GMISS="$GMISS $a";; esac; done
        if [ -z "$GMISS" ]; then
            note "boot config on the gateway IS correct -- a reboot is all you need:"
            note "    sudo ltram-reboot"
        else
            note "boot config on the gateway is ALSO missing:$GMISS"
            note "    emg acquire regenerates that file, so re-running it drops these."
            note "    Re-apply, then reboot:"
            note "    ssh $GW 'F=$GRUBCFG; sed \"s|vmlinuz  ip=|vmlinuz  $WANT_ARGS ip=|\" \$F > /tmp/g.\$\$ && cat /tmp/g.\$\$ > \$F'"
        fi
    else
        note "could not read the gateway config (kinit expired?); check it by hand"
    fi
fi

# ---- 4. the binary ----------------------------------------------------------
if [ ! -x "$MM" ]; then bad "no executable at $MM"
else
    M=""
    for f in chase chase-hist slow-ns null-load resid-every phys print-ranges wait-resident; do
        "$MM" --help 2>&1 | grep -q -- "--$f" || M="$M --$f"
    done
    [ -z "$M" ] && ok "matmul supports every flag the harness passes" \
                || bad "matmul is a stale build, missing:$M"
fi

# ---- 5. the backend ---------------------------------------------------------
if lsmod | grep -q nor_eci; then
    MOD=$(lsmod | awk '/nor_eci/{print $1; exit}')
    IE=$(cat /sys/module/$MOD/parameters/inline_erase 2>/dev/null)
    ok "backend loaded ($MOD)"
    # inline_erase defaults to 1 in the source but every load path passes 0.
    # At 1 the write path erases a sector the background worker already blanked:
    # two physical erases per counted one, and half the endurance budget.
    [ "$IE" = "0" ] && ok "inline_erase=0" || bad "inline_erase=$IE -- want 0 (doubles erases)"
else
    if [ $FIX = 1 ] && [ -x /usr/local/sbin/ltram-load-backend ]; then
        /usr/local/sbin/ltram-load-backend >/dev/null 2>&1 && ok "backend loaded by --fix" \
            || bad "backend not loaded and --fix could not load it"
    else
        bad "backend not loaded (try --fix, or ltram-load-backend)"
    fi
fi
[ -r /sys/kernel/debug/ltram/pagestate ] && ok "ltram debugfs present" \
    || bad "no /sys/kernel/debug/ltram/pagestate (run as root?)"

# ---- 7. the endurance ledger ------------------------------------------------
[ -s /var/lib/ltram/erase_counts ] && ok "erase-count ledger present" \
    || bad "no /var/lib/ltram/erase_counts -- the endurance record is the one thing not reproducible"

[ $QUIET = 1 ] || echo
if   [ $FAIL = 1 ];   then [ $QUIET = 1 ] || echo "NOT READY -- fix the FAIL lines above"; exit 1
elif [ $REBOOT = 1 ]; then [ $QUIET = 1 ] || echo "NEEDS REBOOT -- see the BOOT lines above"; exit 2
else [ $QUIET = 1 ] || echo "ready"; exit 0; fi
