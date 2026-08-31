#!/bin/bash
# qos_matrix.sh -- five conditions, identical settings, one long run each.
#
# The three curves in fig9/fig9b were not measured the same way: the DRAM
# control was 1 x 90 s from a different script, idle NOR was 4 x 60 s pooled,
# and erasing NOR was 2 x 60 s pooled. Worse, the erasing phases ran with
# target_pid = 0, which is the branch in lt_erase_work_fn that sets the requeue
# delay to ZERO -- so the engine erased as fast as the device allowed and the
# "28.8 ms interval" was an outcome, not a setting.
#
# Here every phase holds target_pid pointed at a SLEEPER: a process with no
# promotable anonymous pages. That does two things at once. It stops promotion,
# so the reader is undisturbed, and it puts the engine on the erase_poll_ms
# branch, so erase spacing is a knob we set rather than whatever the hardware
# happens to sustain. Every phase then differs in exactly two bits: which
# medium the working set is on, and whether the engine is running.
#
#   phase 1  DRAM   working set never promoted, engine off
#   phase 2  NOR    working set resident on flash, engine off
#   phase 3  NOR    working set resident on flash, engine on at erase_poll_ms
#
# One continuous measurement per phase. No pooling, no averaging.
set -u
[ "$(id -u)" = 0 ] || exec sudo -E "$0" "$@"
# /scratch is root-owned on NFS, so a new top-level directory cannot be created
# there under root_squash -- but an existing per-user one can be written. Try the
# invoking user, then any existing writable directory, then give up to /tmp.
scratch_dir(){
    local d
    for d in "${SCRATCH:-}" "/scratch/${SUDO_USER:-$(id -un)}/ltram" \
             $(ls -d /scratch/*/ 2>/dev/null | head -20 | sed 's|$|ltram|'); do
        [ -z "$d" ] && continue
        mkdir -p "$d" 2>/dev/null && [ -w "$d" ] && { echo "$d"; return 0; }
    done
    echo /tmp
}
SCRATCH=$(scratch_dir)
PREFLIGHT=$(dirname "$0")/preflight.sh
[ -x "$PREFLIGHT" ] && { "$PREFLIGHT" --quiet || { echo "!! preflight failed"; exit 1; }; }

DBG=/sys/kernel/debug/ltram; PAR=/sys/module/ltram_policy/parameters
REAL_HOME=$(getent passwd "${SUDO_USER:-$(id -un)}" | cut -d: -f6)
MM=${MM:-$REAL_HOME/matmul}
PIN=${PIN:-47}; NN=${NN:-2896}; HOLD=${HOLD:-240}; POLL=${POLL:-30}
OUT=${OUT:-$SCRATCH/qos3}
mkdir -p "$OUT"

ps_(){ awk -v k="$1" '$1==k{print $2; exit}' $DBG/pagestate; }
setp(){ echo "$2" > $PAR/$1; }
setw(){ echo "$1" > $PAR/erase_high_water; echo "$2" > $PAR/erase_low_water; }
eng_off(){ setw 0 0; }
eng_on(){  setw 65536 65535; sleep 0.3; setw 8192 2048; }   # force the latch, then defaults
irqsnap(){ awk -v c="CPU$PIN" 'NR==1{for(i=1;i<=NF;i++) if($i==c) k=i+1; next}
                               k&&NF>=k{n=$1; sub(/:$/,"",n); print n, $k}' /proc/interrupts; }

PB0=$(cat $PAR/promote_batch); D0=$(cat $PAR/wear_days); P0=$(cat $PAR/erase_poll_ms)
HW0=$(cat $PAR/erase_high_water); LW0=$(cat $PAR/erase_low_water)
SLEEPER=""
cleanup(){ pkill -x matmul 2>/dev/null; echo 0 > /sys/kernel/ltram/target_pid 2>/dev/null
           [ -n "$SLEEPER" ] && kill $SLEEPER 2>/dev/null
           [ -n "${CHURNER:-}" ] && kill $CHURNER 2>/dev/null
           setp promote_batch $PB0; setp wear_days $D0; setp erase_poll_ms $P0; setw $HW0 $LW0; }
trap cleanup EXIT INT TERM

NPAGES=$(( NN * NN * 4 / 4096 ))
echo "  $(( NN * NN * 4 / 1048576 )) MiB working set, ${HOLD}s per phase, erase_poll_ms=$POLL"
if [ "$(ps_ clean)" -lt $NPAGES ]; then
    echo "  recycling to $NPAGES clean"; setw 65536 65535
    for i in $(seq 1 3000); do [ "$(ps_ clean)" -ge $NPAGES ] && break
        [ "$(ps_ dirty)" -eq 0 ] && break; sleep 1; done
fi
setp erase_poll_ms $POLL; setp promote_batch 1; setp wear_governor 1; setp wear_days 379

# The sleeper: target_pid must be non-zero in EVERY phase, so the erase spacing
# branch is taken and the setting is identical throughout. It owns nothing the
# scanner can promote, so nothing migrates because of it.
sleep 100000 & SLEEPER=$!

# The write generator. Once the reader is fully resident there is nothing left
# to promote, so a "reads plus writes" phase needs a SECOND workload to supply
# the programs. Pointing target_pid at it means its pages migrate -- that is the
# write traffic -- while the reader, which is not targeted, is left alone. Its
# working set is sized to keep promoting for a whole phase rather than finishing
# early and going quiet halfway through the measurement.
CHURN_N=${CHURN_N:-6500}          # ~161 MiB, ~41,300 pages: at ~140/s that is
                                  # ~295 s, so it is still promoting at the end
                                  # of a 240 s phase rather than going quiet
CHURNER=""
start_churner(){
    [ -n "$CHURNER" ] && return
    taskset -c $(( PIN - 1 )) $MM --n $CHURN_N --iters 1 --runs 100000 --chase \
        > "$SCRATCH/churn.log" 2>&1 &
    CHURNER=$!            # taskset/nice exec, so this IS the matmul pid
    sleep 3
}

L=$SCRATCH/qos3.log
eng_off
taskset -c $PIN nice -n -20 $MM --n $NN --iters 1 --runs 100000 --chase --chase-hist \
    --slow-ns 5000 --print-ranges --phys --resid-every 50 > $L 2>&1 &
BG=$!
for i in $(seq 1 900); do grep -q "^TSTART" $L 2>/dev/null && break; sleep 0.1; done
echo $SLEEPER > /sys/kernel/ltram/target_pid

: > $OUT/marks.txt
phase(){   # $1 = name
    local h0 h1 c0 c1 iv cl0 cl1
    irqsnap > /tmp/q0.$$; c0=$(awk '/^ctxt/{print $2}' /proc/stat)
    cl0=$(ps_ clean); h0=$(grep -c "^HIST" $L); da0=$(ps_ data)
    sleep $HOLD
    h1=$(grep -c "^HIST" $L); cl1=$(ps_ clean); da1=$(ps_ data)
    c1=$(awk '/^ctxt/{print $2}' /proc/stat); irqsnap > /tmp/q1.$$
    iv=$(awk -v a="$h0" -v b="$h1" '/^CTX/{n++; if(n>a&&n<=b) s+=$4} END{print s+0}' $L)
    echo "$1 $h0 $h1" >> $OUT/marks.txt
    # A promotion takes one clean -> data; an erase takes one dirty -> clean.
    # So d_clean = erases - promotions, and clean-delta ALONE is the net, which
    # reads as zero in any phase doing both -- nor_write_erase did ~1,001 erases
    # and reported +2. erases = d_clean + d_data.
    printf "  %-16s %5d passes  writes %+7d (%5.1f/s)  erases %+7d (%5.1f/s)  invol %d\n" \
        "$1" "$(( h1 - h0 ))" "$(( da1 - da0 ))" \
        "$(awk -v a=$da0 -v b=$da1 -v t=$HOLD 'BEGIN{printf "%.1f",(b-a)/t}')" \
        "$(( (da1 - da0) + (cl1 - cl0) ))" \
        "$(awk -v a=$cl0 -v b=$cl1 -v c=$da0 -v d=$da1 -v t=$HOLD \
              'BEGIN{printf "%.1f",((b-a)+(d-c))/t}')" "$iv"
    join -j1 /tmp/q0.$$ /tmp/q1.$$ | awk -v p="$1" '{d=$3-$2; if(d>0) print p,$1,d}' >> $OUT/irq.txt
    rm -f /tmp/q0.$$ /tmp/q1.$$
}

echo "  1/5 dram             DRAM, no writes, no erases"
phase dram

echo "  filling the reader onto flash..."
echo $BG > /sys/kernel/ltram/target_pid   # the reader; $! is its real pid
eng_on
for i in $(seq 1 9000); do
    R=$(grep "^RESID" $L | tail -1 | awk '{print $4}'); R=${R:-0}
    awk -v r="$R" 'BEGIN{exit !(r >= 99.0)}' && break
    kill -0 $BG 2>/dev/null || break; sleep 0.5
done
echo $SLEEPER > /sys/kernel/ltram/target_pid
eng_off; sleep 3
echo "  reader at ${R}% resident"

echo "  2/5 nor_read         NOR, no writes, no erases"
phase nor_read

eng_on
echo "  3/5 nor_erase        NOR, no writes, erases at ${POLL} ms"
phase nor_erase
eng_off; sleep 2

# From here target_pid is the churner, so its pages migrate and the programs
# that migration performs are the write traffic. The reader is not targeted and
# is already resident, so nothing about it changes.
start_churner
echo "$CHURNER" > /sys/kernel/ltram/target_pid
sleep 2
echo "  4/5 nor_write        NOR, writes (churner promoting), no erases"
phase nor_write

eng_on
echo "  5/5 nor_write_erase  NOR, writes and erases at ${POLL} ms"
phase nor_write_erase
eng_off
echo $SLEEPER > /sys/kernel/ltram/target_pid

kill $BG 2>/dev/null; wait $BG 2>/dev/null
grep "^SLOW" $L > $OUT/slow.txt; cp $L $OUT/full.log
python3 - "$OUT" <<'PY'
import sys, collections, csv, os
d=sys.argv[1]; NSUB=8
def edges(b):
    o,sb=divmod(b,NSUB)
    if o==0: return 0,0
    lo=1<<(o-1)
    if o<4: return lo,(1<<o)-1
    w=lo//NSUB; return lo+sb*w, lo+(sb+1)*w-1
spans=[(l.split()[0],int(l.split()[1]),int(l.split()[2])) for l in open(d+"/marks.txt")]
tot=collections.defaultdict(lambda: collections.defaultdict(int)); n=0
for l in open(d+"/full.log"):
    if not l.startswith("HIST "): continue
    n+=1
    for g,lo,hi in spans:
        if lo<n<=hi:
            for i,c in enumerate(l.split()[4:]):
                c=int(c)
                if c: tot[g][i]+=c
            break
rows=[("condition","bucket_lo_ns","bucket_hi_ns","count")]
for g in ("dram","nor_read","nor_erase","nor_write","nor_write_erase"):
    for b,c in sorted(tot[g].items()):
        lo,hi=edges(b); rows.append((g,lo,hi,c))
with open(d+"/qos.csv","w",newline="") as f: csv.writer(f).writerows(rows)
def pct(m,q):
    t=sum(m.values()); s=0
    for b in sorted(m):
        s+=m[b]
        if s>=q*t: return edges(b)[1]
    return 0
def fmt(v): return f"{v/1e6:.1f} ms" if v>=1e6 else (f"{v/1000:.1f} us" if v>=1000 else f"{v} ns")
print(f"\n  {'condition':<17}{'reads':>14}{'p50':>9}{'p99.9':>10}{'p99.99':>10}"
      f"{'p99.999':>11}{'p99.9999':>11}{'max':>11}")
for g in ("dram","nor_read","nor_erase","nor_write","nor_write_erase"):
    m=tot[g]
    if not m: continue
    print(f"  {g:<17}{sum(m.values()):>14,}{fmt(pct(m,.5)):>9}{fmt(pct(m,.999)):>10}"
          f"{fmt(pct(m,.9999)):>10}{fmt(pct(m,.99999)):>11}{fmt(pct(m,.999999)):>11}"
          f"{fmt(pct(m,1.0)):>11}")
PY
echo "  wrote $OUT/qos.csv"
rm -f $L
