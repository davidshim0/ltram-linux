#!/bin/sh
# Profile a workload's file-page activity using /proc/vmstat snapshots.
# Outputs a CSV row to results/profile_<workload>.csv with deltas of the
# key counters.
#
# Usage:   profile_workload.sh <workload_label> <command...>
# Example: profile_workload.sh ycsbc /workloads/YCSB-C/ycsbc -P workloada
#
# Run inside the QEMU guest. The /mnt/results directory is shared via virtfs
# back to the host so the CSV survives the VM exit.

set -u

LABEL="$1"
shift
if [ -z "$LABEL" ] || [ $# -eq 0 ]; then
    echo "Usage: $0 <label> <workload command...>" >&2
    exit 1
fi

OUT="/mnt/results/profile_${LABEL}.csv"

# Extract the counters we care about from /proc/vmstat. Print one CSV row.
snap() {
    awk '
    BEGIN {
        order = "pgpgin pgpgout pgfault pgmajfault pgalloc_normal pgalloc_movable nr_file_pages nr_dirty nr_writeback workingset_refault_file nr_active_file nr_inactive_file nr_dirtied"
        n = split(order, fields, " ")
    }
    /^[a-z_]+ [0-9]+/ {
        vals[$1] = $2
    }
    END {
        for (i = 1; i <= n; i++) {
            v = (vals[fields[i]] ? vals[fields[i]] : 0)
            # The kernel counts pgpgin/pgpgout in 512-byte sectors, not KB:
            # blk-core.c submit_bio() does count_vm_events(PGPGIN/PGPGOUT,
            # bio_sectors(bio)), and bio_sectors = bi_size >> 9. Everything else
            # in this list is already in 4 KB pages. 8 sectors = 1 page, so
            # divide by 8 to put every CSV column in the same unit (4 KB pages).
            if (fields[i] == "pgpgin" || fields[i] == "pgpgout")
                v = int(v / 8)
            printf "%s%s", (i==1 ? "" : ","), v
        }
    }
    ' /proc/vmstat
}

# --- LtRAM placement via node stats -----------------------------------------
# LtRAM lives entirely on one NUMA node (default node 1), and that node has ONLY
# ZONE_LTRAM. So the node's own stats ARE the LtRAM stats -- no missing kernel
# counter required. node_snap() emits "key value" lines from that node's meminfo
# (residency + file/anon/dirty split, in kB) and numastat (cumulative count of
# allocations satisfied from the node = allocations that landed in LtRAM).
LTRAM_NODE=${LTRAM_NODE:-1}
NODE_DIR="/sys/devices/system/node/node${LTRAM_NODE}"

node_snap() {
    if [ -r "$NODE_DIR/meminfo" ]; then
        # lines look like: "Node 1 FilePages:   1234 kB" -> "FilePages 1234"
        awk '{ for (i=1;i<=NF;i++) if ($i ~ /:$/) { k=$i; sub(/:$/,"",k); print k, $(i+1) } }' \
            "$NODE_DIR/meminfo"
    fi
    [ -r "$NODE_DIR/numastat" ] && cat "$NODE_DIR/numastat"
}

# Make sure /mnt/results exists (the virtfs mount creates it, but be defensive)
mkdir -p /mnt/results

# Headers if new file
if [ ! -f "$OUT" ]; then
    cat > "$OUT" <<'EOF'
label,phase,t,pgpgin,pgpgout,pgfault,pgmajfault,pgalloc_normal,pgalloc_movable,nr_file_pages,nr_dirty,nr_writeback,workingset_refault_file,nr_active_file,nr_inactive_file,nr_dirtied
EOF
fi

# Clean baseline: sync dirty data, drop file caches
sync
echo 3 > /proc/sys/vm/drop_caches
sleep 1

# Snapshot before
T0=$(date +%s)
BEFORE=$(snap)
echo "${LABEL},before,${T0},${BEFORE}" >> "$OUT"
NODE_BEFORE="/tmp/.node_before_$$"; node_snap > "$NODE_BEFORE"

echo "=== Profiling ${LABEL}: $* ==="
echo "Starting at t=${T0}"

# Run the workload (timed)
"$@"
RC=$?

T1=$(date +%s)
sync   # flush any pending writes so writeback counters update
AFTER=$(snap)
echo "${LABEL},after,${T1},${AFTER}" >> "$OUT"
NODE_AFTER="/tmp/.node_after_$$"; node_snap > "$NODE_AFTER"

DUR=$((T1 - T0))
echo "Workload exit=${RC}, duration=${DUR}s"

# Methodology legend: what each counter means and exactly when the kernel
# increments it. Printed BEFORE the deltas so it reads as a lookup table for
# the numbers that follow. All values are in 4 KB pages (pgpgin/pgpgout were
# converted from KB in snap()). Counter sites are in mm/ as of the zone_ltram
# kernel; "delta" = after - before across the workload.
cat <<'EOF'

=== Counter legend (units: 4 KB pages; delta = after - before) ===

  -- Disk I/O (block layer; only counts traffic that actually hits the device) --
  pgpgin    Pages read IN from the backing block device (page cache misses
            that did real I/O + readahead). Counted in blk-core.c submit_bio()
            for REQ_OP_READ, in 512-byte sectors. Does NOT count cache hits.
            Converted sectors/8 -> pages here.
  pgpgout   Pages written OUT to the block device (REQ_OP_WRITE), in 512-byte
            sectors, same submit_bio() site. Dirty page-cache flushed to disk.
            Converted sectors/8 -> pages here.

  -- Faults (per-PTE; counts the work the fault handler did) --
  pgfault   ALL minor+major page faults (handle_mm_fault). Includes anonymous
            first-touch, COW write-protect faults, and file-page faults. This
            is the dominant signal for anonymous/in-RAM workloads.
  pgmajfault  Subset of pgfault that had to block on I/O (e.g. fault that
            triggered a disk read). High ratio => working set exceeds RAM.

  -- Allocation (per-zone; where new pages physically came from) --
  pgalloc_normal   Pages handed out from ZONE_NORMAL (node 0 DRAM tier).
  pgalloc_movable  Pages handed out from ZONE_MOVABLE.
            NOTE: /proc/vmstat has no pgalloc_ltram (the zone was never added to
            the vmstat label macros), so LtRAM allocs are absent from the
            columns above. They are captured separately below via node stats,
            since the LtRAM node hosts only ZONE_LTRAM.

  -- Page-cache residency (instantaneous gauge, not a rate) --
  nr_file_pages   Net change in total file-backed pages resident in the cache.
            Snapshot delta, so a small number can hide large churn (equal
            add+evict cancels out). For churn use the bpftrace tracer.

  -- Dirty / writeback state (the read-only-vs-writeable signal) --
  nr_dirty        Net change in pages currently flagged dirty (awaiting
            writeback) at snapshot time. A gauge, not cumulative.
  nr_writeback    Net change in pages currently under active writeback.
  workingset_refault_file   File pages that were evicted and then faulted
            back in (thrash indicator). 0 => the file working set fit in RAM.
  nr_active_file / nr_inactive_file   Net movement between the file LRU lists.
  nr_dirtied      CUMULATIVE count of dirtying events (account_page_dirtied):
            every time a clean page is first made dirty. Unlike nr_dirty this
            only goes up. This is the closest vmstat proxy for "a page that
            was read-only got written" -- but it is process/zone-blind, so it
            cannot tell you whether the dirtied page was one we put in LtRAM.

EOF

# Print human-readable deltas
echo "=== Deltas for ${LABEL} ==="
awk -F, '
    NR==1 { next }                             # header
    $1=="'"$LABEL"'" && $2=="before" { for (i=4; i<=NF; i++) b[i]=$i; names_set=0 }
    $1=="'"$LABEL"'" && $2=="after"  {
        # field name lookup
        split("pgpgin pgpgout pgfault pgmajfault pgalloc_normal pgalloc_movable nr_file_pages nr_dirty nr_writeback workingset_refault_file nr_active_file nr_inactive_file nr_dirtied", names, " ")
        for (i=4; i<=NF; i++) {
            d = $i - b[i]
            printf "  %-30s %+d\n", names[i-3], d
        }
    }
' "$OUT" | tail -20

# --- LtRAM placement deltas (node-stat based) -------------------------------
# Memory fields are kB in the source; shown here as 4 KB pages (kB/4) to match
# the rest of the report. numa_hit/local_node/other_node are cumulative
# allocation counts (the per-LtRAM-node "pgalloc" that vmstat lacks).
# Persist the LtRAM placement report to a sidecar so it survives the VM exit
# (the CSV holds only vmstat counters; this is the node-stat side).
NODE_OUT="/mnt/results/profile_${LABEL}.node"
{
    echo "# LtRAM placement for ${LABEL} (node ${LTRAM_NODE} = ZONE_LTRAM only)"
    echo "# memory deltas in 4 KB pages; numa_* are cumulative alloc counts"
    if [ -s "$NODE_BEFORE" ] && [ -s "$NODE_AFTER" ]; then
        awk '
            NR==FNR { b[$1]=$2; next }
            {
                key=$1; d=$2-b[key]
                if (key=="numa_hit"||key=="local_node"||key=="other_node"||key=="numa_miss"||key=="numa_foreign") {
                    printf "%-18s %+d  (allocs into LtRAM node)\n", key, d
                } else if (key=="MemUsed"||key=="FilePages"||key=="AnonPages"||key=="Active(file)"||key=="Inactive(file)"||key=="Dirty"||key=="Writeback"||key=="Mapped"||key=="Shmem") {
                    printf "%-18s %+d pages\n", key, int(d/4)
                }
            }
        ' "$NODE_BEFORE" "$NODE_AFTER"
        echo "---"
        echo "FilePages delta = read-only/file data placed in LtRAM (the target case)"
        echo "AnonPages delta = anonymous pages in LtRAM (read-only anon routing)"
        echo "Dirty/Writeback = LtRAM pages that got WRITTEN -> should stay ~0 if"
        echo "                  read-only placement is safe for this workload"
    else
        echo "(node ${LTRAM_NODE} stats unavailable -- no LtRAM node or sysfs not mounted)"
    fi
} | tee "$NODE_OUT" | sed 's/^/  /'
echo "  [saved $NODE_OUT]"
rm -f "$NODE_BEFORE" "$NODE_AFTER"

# Quick classification (file-page based; meaningless for anonymous/in-RAM
# workloads like redis where the action is in pgfault/pgalloc, not file I/O).
echo ""
echo "=== Quick read/write classification (file pages only) ==="
echo "  read-heavy : pgpgin >> pgpgout and nr_dirtied small"
echo "  write-heavy: pgpgout and nr_dirtied both grow"
echo "  See the counter legend above for what each number actually measures."

exit $RC
