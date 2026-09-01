# Superseded. Write-cold and cold both rise with T here, and they cannot.

Three rounds x 3 passes x 5 workloads, 2026-09-01 07:17-14:43. The decay
curves are directionally right and the KV results (redis, memcached) are
essentially unaffected -- but both columns were computed against a denominator
that moves.

  write-cold was 1 - dirty_now / resident_now. pagerank frees its 32 MiB score
  array between trials, so a ladder point landing mid-free drops the same
  ~8,203 pages from both terms and the ratio jumps UP with T.

  cold was 1 - Referenced / Rss_now. smaps counts only currently-resident
  pages, so a freed page takes its referenced state with it, same effect.

Fixed by fixing the population at t0, marking pages written once and
permanently, and taking a running maximum on Referenced. Kept because the
three churn-free workloads (llama, redis, memcached) agree with the re-run and
are a useful cross-check on the fix.
