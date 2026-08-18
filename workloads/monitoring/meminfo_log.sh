#!/bin/sh
# Usage:
#   ./meminfo_log.sh > meminfo_log.txt        # start collecting (blocks)
#   ./meminfo_log.sh --end meminfo_log.txt    # stop + print summary

INTERVAL=0.1
PIDFILE=/tmp/meminfo_log.pid

collect() {
    echo $$ > "$PIDFILE"
    echo "ts_s,anon_kb,buffers_kb,pagetables_kb,cache_kb,mapped_kb,slab_unreclaimable_kb,kernel_stack_kb,vmalloc_kb,shmem_kb,anon_hugepages_kb"
    trap 'rm -f "$PIDFILE"; exit 0' TERM INT
    while true; do
        export TS="$EPOCHREALTIME"
        awk -F': ' '
        /^AnonPages/     { anon=$2 }
        /^Buffers/       { buf=$2 }
        /^PageTables/    { pt=$2 }
        /^Cached/        { cache=$2 }
        /^Mapped/        { mapped=$2 }
        /^SUnreclaim/    { sun=$2 }
        /^KernelStack/   { ks=$2 }
        /^VmallocUsed/   { vm=$2 }
        /^Shmem/         { shmem=$2 }
        /^AnonHugePages/ { ahp=$2 }
        END { printf "%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
              ENVIRON["TS"], anon+0, buf+0, pt+0, cache+0, mapped+0,
              sun+0, ks+0, vm+0, shmem+0, ahp+0 }
        ' /proc/meminfo
        sleep "$INTERVAL"
    done
}

summarize() {
    awk -F',' '
    NR==1 {
        for (i=2; i<=NF; i++) cols[i]=$i
        ncols=NF; next
    }
    NF < 2 { next }
    NR==2  { ts_first=$1 }
    {
        ts_last=$1; nrows++
        for (i=2; i<=ncols; i++) {
            v=$i+0
            if (nrows==1 || v < mins[i]) mins[i]=v
            if (v > maxs[i])             maxs[i]=v
            avgs[i]+=v
        }
    }
    END {
        dur=ts_last-ts_first
        printf "Duration : %ds   (%d samples)\n\n", dur, nrows
        printf "%-26s %10s %10s %10s\n", "metric","min","avg","max"
        printf "%s\n", "--------------------------------------------------------"
        for (i=2; i<=ncols; i++)
            printf "%-26s %10d %10d %10d\n", cols[i], mins[i], avgs[i]/nrows, maxs[i]
    }' "$1"
}

case "$1" in
    --end)
        kill "$(cat "$PIDFILE")" 2>/dev/null
        sleep 0.2
        summarize "$2"
        ;;
    *)
        collect
        ;;
esac
