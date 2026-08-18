# Steps 4–7 — backend, migration hook, policy, and the measurement

**Built 2026-08-18. NOT YET BOOTED.** This image contains steps 2 through 6; step 7 is
userspace tooling shipped alongside it.

Kernel `6.8.0-ltram`, tag `step6-policy-2026-08-18` → `376d8dc743` on `ltram-arm64`.
45 ltram symbols. Pairs with bitstream `169_phy200`.

## What is in this image

| step | tag | what it adds |
|---|---|---|
| 2 | `step2-zone-2026-08-18` | node 1 + ZONE_LTRAM, allocator excluded, kernel state kept off flash |
| 3 | `step3-selftest-2026-08-18` | boot-time read self-test |
| 4 | `step4-backend-2026-08-18` | registerable flash write backend + debugfs `write_test` |
| 5 | `step5-migrate-2026-08-18` | migration routed through the driver, hooked **before** the mapping moves |
| 6 | `step6-policy-2026-08-18` | policy vtable, private page allocator, scanner, pid targeting |

Step 5 has no standalone image — its code is in this one. Rebuild from its tag if a
bisection ever needs it.

## Two things the design forced rather than chose

**The mover has its own page allocator.** ZONE_LTRAM is in no zonelist — that is how
residency is enforced — so buddy cannot hand out a flash page even when asked. The zone's
pages are never released to buddy (`managed` stays 0) and `mm/ltram_policy.c` owns them
through a bitmap. The constraint and the FREE/VALID/DIRTY lifecycle the design wants point
the same way.

**Write observation, not access observation.** There is no hardware dirty-bit management
on this CPU (measured: `ID_AA64MMFR1_EL1` HAFDBS = 0), so the dirty bit is set by the
fault handler. The only way to see the *next* write is to write-protect now and let it
fault. Clearing the young bit — the obvious reach — observes **access**, and this policy
is about **writes**: a read-heavy workload would look busy and never be promoted. Cost
scales with write traffic, not pages watched, which is why the scan interval is tunable.

## Two bugs caught in review, worth knowing they existed

**Migration accounting was inverted.** `migrate_pages()`'s last argument is
`ret_succeeded` — the number that *moved*. Read as failures, the counter would have
reported success while nothing moved: exactly the failure this subsystem exists to make
impossible.

**The re-arm cleared the wrong bit** (`young` instead of write-protecting), which would
have measured access rather than writes and never promoted a read-heavy workload.

## Using it

```bash
# attach a RUNNING process -- start it, let it reach steady state, then target it
echo $PID | sudo tee /sys/kernel/ltram/target_pid
sudo cat /sys/kernel/ltram/stats

# which pages actually moved (not just how many)
sudo ./ltram-inspect $PID weights 0xf81488a00000 205520896 result 0xf8149509c000 28672

# the off/on protocol, with the digest as the gate
./run-experiment.sh
```

Tunables: `ltram_policy.scan_interval_ms=1000`, `clean_passes_required=3`,
`promote_batch=32`. Defaults are conservative against the wear budget — 41.5 erases/s
sustained, two erases per wrong promotion — so the **batch cap matters more than the
interval**.

## Hardware gates — all still open

1. boots, `numactl -H` shows node 1 at 256 MB, `/proc/zoneinfo` `present 65536 managed 0`
2. boot leak `present − managed − free` ≤ 3 pages; no stray-allocation warnings
3. self-test line: *"window is readable from kernel context"*
4. `write_test` programs a page; verify three ways — kernel read cache-evicted, harness
   read, and the FPGA status word advancing (`beats +64, pages +16, erases +1`)
5. force one migration, read it back byte-equal; **then the negative test** — make
   `write_page()` return `-EIO` and assert the page is still in DRAM and still correct.
   That validates the hook *placement*, and matters more than the positive case
6. target the workload: weights promoted, **result buffer exactly zero**, and the weight
   contents still correct when read back cache-evicted
7. off/on runtimes with the digest matching the step-1 baseline

## What is deliberately not claimed

The on/off delta mixes **flash read latency, NUMA distance and migration cost**. Isolating
the first needs a control that promotes to node 1 backed by DRAM — same migration, same
distance, no flash — which does not exist on this hardware. Report the delta as a combined
figure and say so.
