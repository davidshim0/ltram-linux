# fig2, two conditions

`fig2-slowdown_erase_triggered.png` is the original sweep. Kept because it is
the condition a real system is in, not because it is wrong.

The sweep drained the pool only when clean fell below 40,000 sectors, so the
large sizes started depleted and with a backlog of dirty sectors left over
from the previous size. Under those conditions clean crosses the low
watermark part way through the run, the erase engine turns on, and NOR cannot
serve a read while a sector is erasing. That is a candidate explanation for
NOR per-line cost jumping from 980 ns at 64 MB to 1216 at 128 MB while DRAM
moved 207 to 216.

`repeat_large.sh` re-measures 64, 128 and 256 MB from a fully drained pool
with the engine pinned off, ten times each, and records erases_done across
every measured run so a contaminated point cannot pass as a result. The
control at 64 MB reproduced the sweep to within 1 ns/line (207.4 vs 207.0
DRAM, 980.7 vs 979.8 NOR), so the two conditions differ in the condition and
not in the instrument.

CAVEAT ON THE NAME: "erase_triggered" is the hypothesis, not yet the finding.
It holds only if the redone 128 MB point comes back near 980 with zero erases
during the run. If it comes back near 1216 with zero erases, contention is
ruled out, the effect is the device, and this file needs a different name.
