#!/bin/sh
# Run profile_workload.sh across one, several, or all workloads.
#
# Workloads are defined by profiles/<name>.work files (sourced shell snippets
# that set LABEL, optional PREP, and RUN). Adding a workload = drop a new
# .work file in profiles/. Nothing here or in the rootfs needs editing.
#
# Usage (inside the guest):
#   sh /mnt/workloads/run_profile.sh                # all workloads
#   sh /mnt/workloads/run_profile.sh all            # all workloads
#   sh /mnt/workloads/run_profile.sh matmul ycsbc   # just these
#
# Output: one CSV per workload at /mnt/results/profile_<LABEL>.csv (shared back
# to the host's results/ dir), plus profile_<LABEL>.ltram if the kernel exposes
# /sys/kernel/debug/ltram/stats.

set -u

HERE=/mnt/workloads
PROFILES="$HERE/profiles"
PROFILER="$HERE/profile_workload.sh"
RESULTS=/mnt/results

list_all() {
    for f in "$PROFILES"/*.work; do
        [ -e "$f" ] || continue
        b=${f##*/}
        echo "${b%.work}"
    done
}

# Default to every workload when no args are given.
if [ "$#" -eq 0 ] || [ "$1" = all ]; then
    set -- $(list_all)
fi

if [ "$#" -eq 0 ]; then
    echo "No workloads found in $PROFILES (expected <name>.work files)." >&2
    exit 1
fi

mkdir -p "$RESULTS"
ran=""

for name in "$@"; do
    f="$PROFILES/$name.work"
    if [ ! -f "$f" ]; then
        echo "!! unknown workload '$name'. Available: $(list_all | tr '\n' ' ')" >&2
        continue
    fi

    # Reset then load this workload's definition.
    LABEL="$name"; PREP=""; RUN=""
    . "$f"

    echo ""
    echo "############################################################"
    echo "# workload: $LABEL"
    echo "############################################################"

    if [ -n "$PREP" ]; then
        echo "[prep] (unmeasured) $PREP"
        sh -c "$PREP"
    fi

    # Measured window: profile_workload.sh syncs, drops caches, snapshots
    # /proc/vmstat before/after, and runs the command we hand it.
    sh "$PROFILER" "$LABEL" sh -c "$RUN"

    # If the kernel exposes LtRAM placement/repatriation counters, capture them
    # alongside the vmstat profile so each run has its LtRAM verdict next to it.
    if [ -r /sys/kernel/debug/ltram/stats ]; then
        cp /sys/kernel/debug/ltram/stats "$RESULTS/profile_${LABEL}.ltram"
        echo "[ltram] saved $RESULTS/profile_${LABEL}.ltram"
    fi

    ran="$ran $LABEL"
done

echo ""
echo "=== done. profiled:$ran ==="
echo "CSVs in $RESULTS (host: results/):"
for l in $ran; do
    echo "  profile_${l}.csv"
done
