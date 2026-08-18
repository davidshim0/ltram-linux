# Workload profiles

Each `<name>.work` file defines one workload for `run_profile.sh`. To **add a
workload, drop a new `<name>.work` file here** — no rootfs rebuild, no edits to
the runner or the init script. The files live in the 9p-shared `workloads/`
dir, so a new file is visible to the guest immediately.

## Format

A `.work` file is a POSIX-sh snippet that sets:

| Var     | Required | Meaning |
|---------|----------|---------|
| `LABEL` | yes      | Short name; output goes to `results/profile_<LABEL>.csv`. |
| `PREP`  | no       | Setup command run **unmeasured** before the snapshot window (e.g. start a server). |
| `RUN`   | yes      | The command `profile_workload.sh` wraps and measures. |

`PREP` and `RUN` are passed to `sh -c`, so env assignments, pipes, and `&&`
work. Keep paths absolute under `/mnt/workloads`.

## Example

```sh
# profiles/myworkload.work
LABEL=myworkload
PREP='ip link set lo up; my-server --daemonize'
RUN='numactl --membind=0 /mnt/workloads/mybench/run --size 1G'
```

## Run

```sh
sh /mnt/workloads/run_profile.sh            # all workloads
sh /mnt/workloads/run_profile.sh matmul     # just one
sh /mnt/workloads/run_profile.sh pagerank ycsbc   # a subset
```
