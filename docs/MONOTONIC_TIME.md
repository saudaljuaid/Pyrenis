# Monotonic time and deadlines

Seneri had three clocks that agreed and nothing that could use them. Each one
handed callers a raw counter sample and left them to remember which counter it
came from, how wide it was, and what its rate meant. Nothing that sleeps or
schedules can be written against that.

This increment adds the two things that turn calibrated rates into usable time:
an instant, and a way to be woken at one.

## The clock

`clock_monotonic_ns()` reports nanoseconds since `clock_start()`. There is one
origin, fixed once, and one answer to "how long since boot".

The source is the time-stamp counter, not the ACPI power management timer, even
though the ACPI timer is what fixes the rate. The reason is width. The ACPI
counter is 24 bits on the supported target and wraps every 4.687 seconds, so
using it directly would oblige every caller to sample often enough to catch each
wrap — an obligation nothing in a kernel can honour while it is doing something
else. The TSC is 64 bits and cannot wrap in any span this kernel will see, and
since `docs/PIT_RETIREMENT.md` its rate is derived from the ACPI timer, so
nothing is given up by preferring it.

```text
Seneri OS: monotonic clock on time-stamp counter
```

### Monotonicity is the contract

A reading below the previous one is clamped to the previous one rather than
reported. This is the one place in the kernel that repairs an input instead of
refusing it, and it is deliberate: monotonicity is what callers are promised, and
a clock that stepped backwards once would let a deadline that has not arrived
look as though it had. Every wait built on it would return early.

The repair is counted, not hidden. `clock_get_state().backward_steps` exposes how
many times it happened, and normal boot panics if it happened at all. On the
supported target the TSC is not invariant, so this is a real possibility across a
power transition rather than a formality.

## Deadlines

`timer_arm(deadline_ns, callback, context, &id)` registers a callback for a
monotonic instant. `timer_cancel(id)` withdraws it. `timer_sleep_ns(ns)` blocks
until the clock has advanced that far.

Pending deadlines live in a table `timer_start` obtains from the kernel heap and
`timer_stop` gives back. It is no longer a static array: `TIMER_MAX_PENDING` is
the capacity requested, `timer_capacity()` reports what was obtained, and every
scan is bounded by that runtime value rather than by an array size the compiler
fixed. This is the heap's first consumer; see `docs/KERNEL_HEAP.md`.

**The table is allocated once, not per arm, and that is deliberate.** `timer_arm`
is reachable from inside the timer interrupt, because a callback is allowed to
arm a fresh deadline, and the heap is not reentrant. Allocating per arm would
put a heap transaction inside an interrupt handler. Growing the table on demand
is deferred for the same reason.

A table that has not been obtained is a capacity of zero, and every loop is
bounded by capacity, so a null table makes every scan simply empty. That is what
makes the null pointer safe rather than a hazard, and it is why none of the
table helpers carries a null check of its own.

**Not started must mean no table held.** `timer_start` refuses if a table is
already present even when `started` is false, because that combination is a
subsystem holding memory it does not know about, and starting over the top of it
would lose that block for the life of the kernel.

A bucketed wheel still replaces the linear scan once something arms deadlines in
bulk.

Identifiers are never zero, so a caller can hold zero as "no timer" without a
separate flag.

### What is refused

- a deadline already in the past, or nearer than `TIMER_MINIMUM_INTERVAL_NS`.
  Firing immediately would hide the caller's arithmetic bug behind a callback
  that appeared to work;
- an identifier that names no pending timer, including one already cancelled or
  already fired, so a stale identifier cannot silently cancel a later timer that
  reused its slot;
- a full table, rather than overwriting the entry nearest to expiring;
- anything at all before `timer_start`, and `timer_start` itself before the clock
  has an origin.

### The hardware underneath

The local APIC timer gained a one-shot mode. One vector and one count register
serve both modes, so a periodic timer and an armed deadline exclude each other,
and `apic_timer_start` refuses while an expiry handler is installed because that
handler already owns the vector registration.

Registration is tied to the installed handler rather than to each arming. That
distinction is load-bearing and was learned the hard way: an expiry rearms from
inside the timer interrupt, where the vector is still registered, so a per-arm
registration is refused as already present. The first version of this code did
exactly that, and the result was that the first deadline fired, nothing was
rearmed, and every later deadline was lost — the `timers` scenario hung rather
than failed, which is how it was found.

Expiry order is by deadline, earliest first, with ties broken by slot so the
order does not depend on scan direction. Each entry is released *before* its
callback runs, so a callback may arm a fresh deadline into the slot it just
vacated, and the scan restarts after every callback because a callback is allowed
to cancel or arm anything.

### How bounded the sleep is

`CONTRIBUTING.md` forbids unbounded waits, and this one deserves a precise claim
rather than a comfortable one.

The sleep loop halts, waiting for the timer interrupt. Two exits are guaranteed:
the monotonic clock passing twice the requested interval plus a grace period, and
finding nothing armed to wake it. The second matters more than it looks — halting
waits for *an* interrupt, so if nothing is armed then nothing will arrive and the
clock bound would never be reached to notice; the processor would simply stop.
Refusing to halt with no deadline armed is what turns that into a status, and the
third negative control below is exactly that case.

What remains unbounded is an armed deadline the hardware never delivers. Software
cannot distinguish that from a stopped processor, and every other wait loop in
this kernel — `pit_wait_for_ticks`, `apic_timer_wait_for_ticks` — has the same
exposure.

## Executable proof

`clock_self_test` proves the monotonic guarantee from synthetic readings: a
rising reading passes through and counts no repair, an equal reading is not
backwards, a lower one is clamped and counted, each further step back is counted
separately rather than collapsed, and zero is not a special case in either
position. It also pins down what the clock reports before it has an origin.

`timer_self_test` proves the table from synthetic entries, with no hardware and
no timing, which is why the table is kept separate from the arming: which
deadline is next, which identifier maps to which slot, that an instant *equal* to
a deadline reaches it rather than only an instant past it, that a full table
refuses a further entry, that an entry with no callback is still released, and
that ties are broken by slot. It also proves nothing works before `timer_start`.

`apic_timer_self_test` gained the interval-to-count conversion: a millisecond and
a whole second at 62.5 MHz, an interval too short to advance the counter once, one
longer than the register can hold — just past sixty-eight seconds at that rate —
a zero rate, and a span so wide the whole-seconds half would overflow.

The `timers` QEMU scenario proves the hardware path: the clock refuses to start
without a calibrated counter, starts once, never steps backwards across sixty-four
reads, then three deadlines armed in reverse order fire in time order, a sleep
past all of them returns no earlier than it should, a cancelled deadline does not
fire and its identifier goes stale, and the table settles empty.

Normal boot additionally requires:

```text
Seneri OS: monotonic clock on time-stamp counter
Seneri OS: slept 50516366 ns for a 50000000 ns deadline
Seneri OS: deadline timers online
Seneri OS: monotonic time established
```

A sleep is the strongest single check here: it can only return on time if the
clock's origin, the nanosecond conversion, the one-shot count and the expiry path
are all correct together. Boot holds the measured sleep to a quarter of the
requested interval and refuses any undershoot at all.

## Negative controls

Three deliberate breakages, each reverted afterwards:

| Breakage | Observed failure |
| --- | --- |
| the monotonic clamp removed | `PANIC: monotonic clock self-test failed`, before boot reaches hardware |
| the sleep's deadline halved | `PANIC: sleep returned before its deadline`, reporting `slept 25534838 ns for a 50000000 ns deadline`; `ST FAIL timers: not every deadline fired` |
| the expiry path stopped rearming | `ST FAIL timers: sleep did not complete` |
| stop never returns the table to the heap | `ST FAIL timers: stopping did not return the table to the heap` |
| start reports success without obtaining a table | `PANIC: deadline timers started without a table` |
| the self-test borrows the table and never puts it back | `PANIC: deadline timers were started twice` |

The third of those did not fail on the first attempt. A stale table pointer left
behind by the self-test is simply overwritten by the next `timer_start`, so
nothing observed it and the suite stayed green. The response was to add the
invariant that was missing rather than to accept the non-result: `timer_start`
now refuses to run while a table is held, and the control fails.

A fourth control — freeing the table before removing the expiry handler, rather
than after — **also failed to fail, and was left as a non-result.** `timer_stop`
requires interrupts disabled and is only reached with nothing armed, so the
window where an interrupt could walk a freed table is unreachable in this
kernel. The ordering is defence in depth for a future where stop can race, not
something any scenario here can observe, and it is recorded rather than dressed
up as tested.

The third is the one that justifies the armed check in the sleep loop. Without
it, that breakage does not fail — it hangs, and a hang in a QEMU scenario is a
timeout rather than a diagnosis. This is also how the registration bug described
above was found in the first place.

## Deferred work

Phase 0 is complete: the calibrated rates are now a clock and a deadline. What is
still missing sits above this layer, not inside it.

- **Nothing sleeps concurrently.** `timer_sleep_ns` halts rather than yielding.
  ~~A second sleeper needs threads~~ — threads now exist, see `docs/THREADS.md`,
  but the scheduler is cooperative and a sleep does not hand it the processor.
  Making a sleep a block is what finally makes this entry false, and it needs
  preemption first.
- ~~**The table is a linear scan over fixed storage**, because there is no
  heap.~~ **Half fixed.** The storage is now one kernel heap allocation made at
  `timer_start`, so `TIMER_MAX_PENDING` is a default rather than an array bound;
  see `docs/KERNEL_HEAP.md`. The *scan* is still linear, and turning it into a
  bucketed wheel is still waiting for something that arms enough deadlines to
  care.
- **Callbacks run in interrupt context** with interrupts disabled, so they must
  not block. Deferring work to a thread needs a thread.
- **No wall-clock time.** This is time since boot only; a date needs the ACPI
  real-time clock and a policy about what to trust it for.
- **Nothing here is per-processor.** The TSC is core-local and the table is a
  single static, so a second processor needs its own of both, and an invariant
  TSC before the two could be compared. The supported target still reports none.
