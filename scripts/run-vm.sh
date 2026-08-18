#!/bin/bash
# Check if at least one argument is provided
if [ "$#" -lt 1 ]; then
    echo "Usage: ./run-vm.sh [workload_name|interactive]"
    echo "Available workloads: matmul, gapbs, redis, llama, interactive"
    exit 1
fi

MODE=$1

if [ "$MODE" = "interactive" ]; then
    INTERACTIVE=1
    WORKLOAD=""
else
    INTERACTIVE=0
    WORKLOAD=$MODE
fi

set -e

# Dynamically find the directory where this script is located
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"

# Go up one level to find the main ltram-policy-bench root folder
BASE_DIR="$(dirname "$SCRIPT_DIR")"

# Set all paths relative to that root folder
KERNEL="${BASE_DIR}/linux/arch/x86/boot/bzImage"
ROOTFS="${BASE_DIR}/buildroot/output/images/rootfs.ext2"
RESULTS="${BASE_DIR}/results"
WORKLOADS="${BASE_DIR}/workloads"
INPUTS="${BASE_DIR}/inputs"

mkdir -p $RESULTS

# download the llama-bench and *.gguf files if they aren't present on a llama run
if [ "$WORKLOAD" = "llama" ]; then
    if [ ! -e "$WORKLOADS/llama/llama-bench" ] || ! ls "$INPUTS"/*.gguf >/dev/null 2>&1; then
        echo "[run-vm] llama artifacts missing -- staging via scripts/setup-llama.sh"
        "$SCRIPT_DIR/setup-llama.sh"
    fi
fi

# === Host NUMA pinning ===
# Active (Configuration A): pin everything to host node 0.
# Both guest tiers (Node 0 = DRAM, Node 1 = LtRAM) are backed by the same
# host socket, so there is no real latency asymmetry between them — only
# the SLIT distance=20 hint exposed to the guest kernel. Clean baseline
# for policy/correctness work.
NUMACTL="numactl --cpunodebind=0 --membind=0"
M0_HOST_OPTS=""
M1_HOST_OPTS=""

# Configuration B (uncomment when policy validation is done, and comment
# out Configuration A above):
# vCPUs stay on host node 0, m0 is bound to host node 0 (local), m1 is
# bound to host node 1 (remote). Accesses to the guest's LtRAM tier now
# incur real cross-socket DRAM latency. Useful for memory-sensitivity
# sweeps. Do NOT combine with --membind=0 — that would fight the
# per-backend host-nodes pinning.
# NUMACTL="numactl --cpunodebind=0"
# M0_HOST_OPTS=",host-nodes=0,policy=bind"
# M1_HOST_OPTS=",host-nodes=1,policy=bind"

$NUMACTL qemu-system-x86_64 \
  -enable-kvm \
  -cpu host \
  -smp 4 \
  -m 8G \
  \
  -object memory-backend-ram,id=m0,size=7936M${M0_HOST_OPTS} \
  -object memory-backend-ram,id=m1,size=256M${M1_HOST_OPTS} \
  -numa node,nodeid=0,memdev=m0,cpus=0-3 \
  -numa node,nodeid=1,memdev=m1 \
  -numa dist,src=0,dst=1,val=20 \
  -numa dist,src=1,dst=0,val=20 \
  -kernel "$KERNEL" \
  -drive file="$ROOTFS",format=raw,if=virtio \
  -append "root=/dev/vda rw console=ttyS0 nokaslr numa=on ltram_workload=$WORKLOAD interactive=$INTERACTIVE" \
  \
  -virtfs local,path="$WORKLOADS",mount_tag=workloads,security_model=passthrough \
  -virtfs local,path="$RESULTS",mount_tag=results,security_model=none \
  -virtfs local,path="$INPUTS",mount_tag=inputs,security_model=passthrough \
  \
  -nographic \
  -serial mon:stdio \
  -no-reboot # -s -S

if [[ $INTERACTIVE -eq 0 ]]; then
  # also try to plot the monitoring graphs
  python3 $WORKLOADS/monitoring/meminfo_plot.py $WORKLOAD
fi
