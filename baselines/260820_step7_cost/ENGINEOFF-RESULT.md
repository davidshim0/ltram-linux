# The idle-flash anomaly does not reproduce

The last sweep gave `engine_off` a p99.999 of 18.4 us and four events over
100 us, while `engine_spaced` -- actively erasing -- managed 1.7 us. With no
writes and no erases there is no mechanism for that, so the measurement was
suspect rather than the medium.

Ran the same OFF condition four times at different positions, with erasing
phases interleaved, instrumenting every phase identically.

| phase | stalls >5 us | worst |
|---|---|---|
| off_1, straight after the fill | 0 | - |
| off_2, after an erasing phase | 1 | 12.8 us |
| off_3 | 0 | - |
| off_4, immediately after off_3 | 0 | - |
| erasing_1 | 1,794 | 21.5 ms |
| erasing_2 | 1,805 | 22.6 ms |

**One stall across four measurements of the idle condition.** It does not track
position, does not track the condition, does not wander. It is absent.

## What differed

| | sweep_qos | this probe |
|---|---|---|
| involuntary ctx switches | 10 | 0, 0, 0, 0 |
| arch_timer on CPU47 | 2,378 (26/s) | 0 |

Something was runnable on CPU47 during that phase of the sweep and is not here.
That is the difference; the cause is not established, and n=1 on a
non-reproducing event is not worth chasing further.

**The sweep's engine_off row should not be used.** Idle flash is clean.

## What the erasing phases confirm

- clean 5 -> 2,085 in 60 s = 34.7 erases/s, so the hysteresis fix works: setting
  the watermarks alone had previously left the engine off and the phase measured
  nothing.
- 29.9 stalls/s, so 86% of erases catch a read -- the same fraction as the
  earlier flat-out phase.
- worst 21.5 and 22.6 ms, against an independently measured erase of 22.3 ms
  mean. The stall IS the erase, now confirmed from both ends.
- 170 passes idle against 83 erasing: erasing halves read throughput at this
  erase rate.
