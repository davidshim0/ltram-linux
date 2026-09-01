# Discard. Measured against a 32-thread competitor.

A `pr` process left over from a smoke test ran at 2592% CPU from 02:01:13
until 03:23, saturating all 32 cores for the whole of this census. Load
average sat at 32 throughout.

`rw_census.py` setsid's its child so a stray Ctrl-C cannot reach it; that also
meant `timeout` killing the census left the workload running. Fixed in
46668b6 + follow-up: the child is now reaped on SIGTERM/SIGINT/SIGHUP and via
atexit, and run_census.sh refuses to start when a workload is already running.

The KV rows here are additionally wrong for two reasons that have nothing to
do with the contention:

  - the "zipfian" sampler was random() ** (1/(1-0.99)), an exponent of 100,
    not a zipf 0.99 skew;
  - the load generator was unthrottled, so it rewrote the whole keyspace in
    minutes. The numbers describe the generator, not the engine.

Kept only so the numbers are not silently replaced.

    redis      44.60% write-cold @ T=120,  0.25% cold
    memcached   7.66% write-cold @ T=120,  0.02% cold
