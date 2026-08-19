# Retiring the 8254

The PIT was Seneri's ruler. Both derived clocks were measured against it, and
`docs/PM_TIMER.md` records what that cost: the PIT's own tick accounting was
wrong by a factor of two, both clocks inherited the error, and because they
inherited it equally they went on agreeing with each other and nothing noticed.

This increment removes the ruler. The local APIC timer and the time-stamp counter
are now calibrated against the ACPI power management timer, whose rate the
specification states rather than something this kernel measured, and the 8254 is
stopped, masked and latched shut.

## Why this could not have come earlier

Retiring a reference is only safe once something else can time an interval. That
was the whole argument for the previous increment: the PM timer's frequency is
fixed by ACPI 6.6 section 4.8.3.3 at 3.579545 MHz and is measured against
nothing, so it is not a third opinion drawn from the same evidence.

The boot sequence is that argument in order:

1. establish the PM timer and require its counter to advance;
2. calibrate the local APIC timer against it;
3. calibrate the time-stamp counter against it;
4. retire the PIT;
5. prove all three still agree about one interval with the 8254 dead.

Step 5 is the acceptance test for step 4. Nothing is retired before the thing
replacing it has been made to work.

## Calibrating against a stated rate

Both calibrations changed shape in one important way: the span is scaled by the
ticks the reference **actually** advanced, not by the ticks that were requested.

```c
rate = elapsed_counts * PM_TIMER_FREQUENCY_HZ / observed_reference_ticks;
```

`pm_timer_wait` is a bounded spin, so it may overshoot its request. Scaling by the
observed span keeps the overshoot out of the rate instead of inflating it. A
consequence worth knowing: the requested interval length is **not** load-bearing.
Doubling it changes only how long calibration takes. The scale factor is
load-bearing, and that is what the negative control below breaks.

The result is measurably better than the PIT ever managed. QEMU's local APIC
timer counts one tick per 16 ns under divide-by-sixteen, so 62.5 MHz exactly:

| Reference | Calibrated | Error against QEMU's model |
| --- | --- | --- |
| PIT, mode 3 (before `docs/PM_TIMER.md`) | 31.3 MHz | −50% |
| PIT, mode 2 | 62 643 780 | +0.23% |
| ACPI PM timer | 62 548 547 | **+0.078%** |

The PIT's remaining error was its 100 Hz tick granularity over a tenth of a
second. The ACPI timer resolves the same interval to 279 ns.

## What retirement does

`pit_retire()` stops the counter if it was running, masks its I/O APIC
redirection entry even if it never ran — firmware may have left the line live,
and a retired timer must not be able to deliver — and latches the subsystem. The
8259 pair is masked by its own retirement, so only the discovered route is
touched; a machine whose I/O APIC was never initialized has no route to mask.

Retirement is one way. `pit_start` and a second `pit_retire` both return
`PIT_STATUS_RETIRED` rather than quietly re-arming a timer the kernel no longer
reasons about. This mirrors `pic_retire` in `docs/LEGACY_RETIREMENT.md`.

## What the PIT is still for

The driver stays. The PIT is no longer a time reference, but it is still the
first interrupt source proved at boot: `prove_timer_route` counts it over the
legacy path and then the discovered one, and the `pit`, `apic`, `ioapic` and
`retired` scenarios use it to prove routing end to end. Deleting it would remove
that proof, not simplify the kernel. It is retired only after those proofs have
run.

## Executable proof

`apic_timer_self_test` and `tsc_self_test` prove the calibration arithmetic
against the new reference without hardware. Both now express the reference
interval as exactly one second — `PM_TIMER_FREQUENCY_HZ` of its ticks — so the
expected rate is exact rather than truncated, and both add a case where twice the
reference span must halve the computed rate. Two seconds is used rather than half
a second because the reference rate is odd, so halving it would truncate and the
expectation would stop being exact.

Two refusals became worth testing that were not before:

- a reference reporting zero elapsed ticks, which is now a hardware-supplied
  value rather than a compile-time constant, and must not divide by zero;
- a span so wide that scaling it by the reference rate would overflow. Against
  the PIT's rate of a hundred that branch needed a span no counter could produce;
  against 3.579545 MHz it is reachable, so `tsc_self_test` reaches it.

The `pit-retired` QEMU scenario is the acceptance test. It proves the PIT works,
retires it, confirms it refuses every further mutation, and then calibrates both
clocks and cross-checks all three **with the 8254 dead**:

```text
ST INFO pit-retired: PM 200422400 ns, APIC timer 200000000 ns, TSC 200077875 ns
```

Its order is deliberately the inverse of normal boot's. Normal boot calibrates
and then retires; the scenario retires and then calibrates. That inversion is the
point — see the third negative control.

Normal boot additionally requires:

```text
Seneri OS: PM timer counted 35795 ticks in 9999874 ns
Seneri OS: local APIC timer calibrated at 62548547 counts per second
Seneri OS: TSC calibrated at 2802032223 Hz, invariant no
Seneri OS: clocks agree: PM 200434412 ns, APIC timer 200000000 ns, TSC 200171595 ns
Seneri OS: PIT retired
Seneri OS: clocks survive PIT retirement
```

## Negative controls

Three deliberate breakages, each reverted afterwards:

| Breakage | Observed failure |
| --- | --- |
| calibration scale factor doubled | `ST FAIL apic-timer: local APIC timer rate disagrees with its reference`; `ST FAIL pit-retired: clocks disagree on an interval without the PIT`; `PANIC: PM timer and local APIC timer disagree on interval` |
| `pit_retire()` made not to latch | `ST FAIL pit-retired: PIT is not fully retired`; `PANIC: PIT did not stay retired` |
| calibration made to depend on the PIT again | `ST FAIL pit-retired: local APIC timer would not calibrate without the PIT` — and **normal boot still passes** |

The third is why the scenario exists. Normal boot calibrates before it retires,
so a reintroduced PIT dependency is invisible to it. Only the inverted order
catches it.

A fourth attempt failed to fail, which is worth recording: doubling
`REFERENCE_PM_TICKS` changed nothing, because the rate is scaled by the observed
span rather than the requested one. That is the design working, not the test
being weak — so the control was aimed at the scale factor instead.

## Deferred work

`docs/MONOTONIC_TIME.md` covers the increment that completes Phase 0, turning
these three calibrated rates into something a kernel can use: a monotonic clock
with a single origin, and deadlines armed on the local APIC timer's one-shot mode.

Both PM timer waits here are still polling spins, so nothing on this page
delivers an interrupt from the ACPI timer or maintains a clock between calls;
that is what the clock above is for.

Beyond that: the supported target still reports no invariant TSC, so the
time-stamp counter remains untrustworthy across power states even though its rate
is now correct. Level-triggered I/O APIC routing still needs directed EOI,
which gates legacy device interrupts — though `docs/PCI_ENUMERATION.md` found
that every PCI Express endpoint on the tested machines offers message-signalled
interrupts, which are edge triggered by construction, so that gap is no longer
on the road to device drivers. Nothing here is per-processor; every subsystem in
this kernel still holds a single static state.
