# The device operations, measured. And they serialise -- but the policy dodges it.

First time either operation has been timed on this board. Everything before this
quoted the datasheet.

## The two numbers the project rests on

| | datasheet | measured | |
|---|---|---|---|
| erase | 16.4 ms | **22.3 ms mean**, 476 of 485 in the 16-32 ms bucket | confirmed |
| program | 1.2 ms | **1.13 ms mean**, all 7,665 idle-device writes in 1-2 ms | confirmed |

## Do a program and an erase serialise?

**Yes, completely.** Same operation, split by what the device was doing when it
started:

| write started | duration |
|---|---|
| on an idle device | 1-2 ms. Every one of 7,665. |
| while an erase was in flight | spread to 16-32 ms; 39 of 69 at the top |

A write that catches an erase waits the erase out and then programs. There is no
overlap at the device.

## But the collision is rare, and that is software

The device erased for 10.8 s of 120 s -- **9.0% duty**. If writes arrived
independently of erases, 695 of 7,728 should have collided.

**69 did. Ten times fewer than chance.**

`lt_erase_work_fn` gates on `ltram_device_idle()` before starting an erase, and
that gate is working: erases are placed in the gaps between writes. The
serialisation is a property of the hardware; the *avoidance* is a property of the
policy, and it is carrying most of the load.

Cost of the collisions that do get through: 69 x ~20 ms = 1.38 s, **1.1% of wall
time**, against 9.0% of wall time spent erasing.

## Why this matters for the design

The two costs were assumed to add: promote at N/s, erase at M/s, pay both. They
do not, because the policy keeps them apart. But the mechanism that keeps them
apart is a device-idle check, and anything that makes the device look busy when
it is not -- or fails to notice that it is -- turns 1.1% back into 9%.

Rates during this run: promotion 64.4/s, erase 4.0/s, residency reached 96%.
