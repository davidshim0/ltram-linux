#!/bin/bash
# gate2.sh — everything step 2 must satisfy, including the sub-check it was missing.
echo "########## IDENTITY"
uname -r; grep -o '#[0-9]* SMP[^)]*' /proc/version
echo; echo "########## A. the zone: spanned/present 65536, MANAGED 0"
sed -n '/Node 1, zone *LtRAM/,/^Node /p' /proc/zoneinfo | grep -E 'pages free|spanned|present|managed|start_pfn'
echo; echo "########## B. nothing leaked onto flash during boot"
sudo cat /sys/kernel/debug/ltram/stray_allocs
sudo cat /sys/kernel/debug/ltram/start_pfn /sys/kernel/debug/ltram/end_pfn
echo; echo "########## C. the allocator still REFUSES node 1"
numactl --hardware | grep -E 'node 1|distances' 
numactl --membind=1 -- true >/dev/null 2>&1; echo "membind=1 exit=$?  (nonzero = correct)"
echo; echo "########## D. NEW: is the window reachable from kernel context?"
# NOTE: do NOT clear the ring buffer here. An earlier version ran "dmesg -C" to
# isolate the module output and destroyed the boot log with it -- the very evidence
# section E exists to capture. Filter instead of clearing.
sudo insmod ~/nor_eci/nor_eci_fulltest_ltram.ko test=32 cvm_ok=1 st_wait=2 num_sectors=64
sleep 25
sudo dmesg | grep -E 'fulltest|Unable to handle|translation fault|BUG|WARNING' | tail -25
sudo rmmod nor_eci_fulltest 2>&1 | tail -2
echo; echo "########## E. boot log"
sudo dmesg | grep -iE 'ltram|NUMA balancing|NODE_DATA' | head -20
