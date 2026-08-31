# The base-case tail was the scheduler tick, and removing it removes the tail

Causal, not correlational. `nohz_full=47 isolcpus=47 rcu_nocbs=47 irqaffinity=0-46`
added to z08's netboot cmdline (gateway: `/srv/tftp/boot/grub/grub.cfg-C0A8C008`,
which `emg acquire` generates but which can be edited directly and persists).

| | tick on | nohz_full | |
|---|---|---|---|
| `arch_timer` on CPU47, per 60 s | 60,012 | ~0 | gone |
| slow events (>5 us), real load | 35,975 | **2** | 18,000x |
| slow events (>5 us), null load | 18,843 | **0** | all |

**The null run recorded zero accesses over 5 us in 450,363,392 accesses.** Not
"few" -- none. The real run kept 2 in 263,979,008, at 22.6 us and 13.4 us,
which is 7.6e-9 and unattributable at n=2.

## What this closes

The prediction was made before the reboot and held: identify the source by
phase and rate, remove it, and the population disappears. Three claims that
were inference are now measurement.

- The ~20 us population was the HZ=1000 scheduler tick. It is gone with the
  tick gone.
- It was never the memory. The null load -- same loop, same two clock reads,
  no memory access -- now shows a perfectly clean zero.
- The measurement floor above 5 us is now empty, so anything the flash runs
  show above that threshold is the device, with no subtraction required.

## What it does NOT close

The 2 survivors are too few to characterise. The 1.44 ms singleton seen once
in 158M accesses did not recur in 264M here. The seven 21-23 ms events in the
engine-off flash phase remain unexplained and are the next thread: they were
8x above the old noise floor and the floor is now zero.

## Caveat

`isolcpus=47` means the scheduler will not place work on CPU47 by itself.
`taskset` is the only route there now. And `emg acquire` regenerates the grub
config from scratch, so re-running it silently drops these parameters -- check
`/sys/devices/system/cpu/nohz_full` after any acquire.
