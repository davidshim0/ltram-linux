# reference/ — kept for context, not on the build path

Nothing here is built or run by `scripts/`. It is material from the earlier
`ltram-policy-bench` arrangement that is worth being able to read, and that would
otherwise be lost when those directories are deleted.

| what | why it is kept |
|---|---|
| `ltram_module/` | The **earlier out-of-tree design**: a module exposing `/dev/ltram` that handled LtRAM→DRAM repatriation on write, built against a `zone_ltram` kernel branch providing `__GFP_LTRAM` and `ltram_migrate_to/from`. Superseded by the in-tree `mm/ltram.c` + `mm/ltram_policy.c`, which take a different approach — the zone is in no zonelist and the policy owns its pages through a private bitmap. Useful as a record of what the API looked like before, and why it changed. |
| `harness/nor_eci_fulltest.c` | Source of the FPGA test suite (`test=NN`) that every hardware gate is expressed in. The compiled module for `6.8.0-vanilla68` is `baselines/260818_step0_vanilla68/nor_eci_fulltest.ko`. The maintained copy lives on ba8 in `VivadoProjects/nor_eci_tools/`. |
| `overlay/`, `buildroot-config`, `linux-config` | The buildroot/VM path, from before the work ran on real hardware. Kept because a VM target may be wanted again for fast iteration on kernel logic that does not need flash. |
