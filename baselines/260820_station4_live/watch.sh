#!/bin/bash
# Patient collector: z08 is loaded enough that new ssh logins can take minutes.
# Retries forever with a generous budget and appends whatever it gets.
O=/local/home/hushim/ltram-linux/baselines/260820_station4_live
for i in $(seq 1 400); do
  echo "===== $(date +%H:%M:%S) attempt $i ====="
  timeout 300 ssh -o BatchMode=yes -o ConnectTimeout=30 zuestoll08 \
    'cat /proc/loadavg; cat /sys/kernel/ltram/target_pid;
     sudo -n cat /sys/kernel/ltram/stats 2>/dev/null | grep -E "pages_in_use|^promoted|promote_failed|^demoted";
     tail -3 ~/station5.log 2>/dev/null;
     ls /scratch/hushim/*/VERDICT.txt 2>/dev/null' 2>&1
  echo "rc=$?"
  sleep 120
done
