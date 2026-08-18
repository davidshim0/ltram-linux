# Step 1 — matmul workload and baseline control

**2026-08-18.** The instrument every later measurement is compared against.

| | |
|---|---|
| git tag | `step1-matmul-2026-08-18` (repo `ba8:/local/home/hushim/ltram-kernel`) |
| kernel | `6.8.0-vanilla68` — baseline `vanilla68-2026-08-18` |
| bitstream | `169_phy200` (trail +0.433) |
| host | zuestoll08, 48 cores, 125 GiB |

## The measured baseline

```
matmul --n 7168 --iters 20 --runs 10 --verify --protect-weights
  weights  196.0 MiB  at 0xf81488a00000   (205,520,896 bytes)
  result    28.0 KiB  at 0xf8149509c000
  mean 6.836 s   sd 0.001 s   (0.01%)
  DIGEST 41bd154efbce9bd07461680229516268bd481e8bbdb3d187cd8937ca7ae93a92
```

**Every later run must reproduce that digest exactly.** A faster run with a different
digest is corruption, not a result.

## Why these numbers

**Variance 0.01%** — a 1% regression is 68 ms against 1 ms of noise, roughly 68x. The
gate in the plan asked for under 2%; this is two orders better, so the control can resolve
effects far smaller than the ones we expect.

**196 MiB of weights** fits the 256 MiB flash window with ~60 MiB of headroom, so the
whole weight region can be promoted without the policy having to choose a subset. Sized
deliberately: at exactly 256 MiB there would be no room for the zone's own overhead.

**The read-only claim is proven, not assumed.** `--protect-weights` mprotects W and
installs a handler that reports the offending offset and exits 42. It ran clean, so the
algorithm demonstrably never writes the weights.

## What this does not yet tell us

Nothing about flash. This is the DRAM control — the denominator. It also says nothing
about workloads whose read-only split is *observed* rather than structural; memcached,
YCSB, redis and llama each bring a tail of writes that matmul does not have, and that
tail is where promotion decisions get expensive.

## Reproducing

```bash
scp matmul zuestoll08:~/
ssh zuestoll08 './matmul --n 7168 --iters 20 --runs 10 --verify --protect-weights'
```
Rebuild with `make` in `workloads/matmul` (cross) or `make native` (local check).
