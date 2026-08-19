# Kernel threads

More than one thread of control, on one core, cooperatively scheduled.

Everything below this layer runs on the single 16 KiB stack `boot.S` sets up, so
any operation that has to wait blocks the whole machine. That is why
`timer_sleep_ns` halts instead of yielding, and why no driver can own a device
that takes milliseconds to answer. `docs/MONOTONIC_TIME.md` named this as the
thing the deadline layer exists to make possible. This is it.

## The one idea

A call to `thread_switch_context` leaves its return address on the stack it was
called on. So changing the stack pointer changes which return address the final
`ret` uses.

Every thread that is not running is sitting inside that one function, having
pushed its callee-saved registers and its flags. Resuming a thread is letting
its call finish. Creating a thread is building a stack that *looks* as though it
is already suspended there, so that the first switch onto it returns into a
trampoline instead of back into a caller.

That is the entire mechanism. Everything else here is bookkeeping and refusals.

## What the switch saves, and why

System V AMD64 makes RBX, RBP and R12–R15 callee-saved, so those are exactly the
registers that must survive. Everything else is caller-saved: the compiler has
already spilled anything it still needs across the call, and saving it here
would be saving it twice.

RFLAGS is saved as well, and that is a deliberate addition rather than an ABI
requirement, for two reasons:

- The direction flag must be clear on entry to any function. A switch that let
  one thread's DF leak into another would be a lasting corruption of every
  string operation, not a lost value.
- The interrupt enable bit lives there. When preemption arrives, a thread must
  resume with the interrupt state it was *suspended* with, not with the state of
  whichever thread happened to run before it. Saving it now means that increment
  adds nothing to this file.

The outgoing stack pointer is published only after every register is on that
stack. A stack pointer recorded earlier names a frame that is still being
written — harmless today, and a race the moment anything can resume a thread
asynchronously.

A new thread's prepared frame carries the entry function in the saved R12 and
its argument in R13, so the switch that starts it delivers both as ordinary
callee-saved registers at no extra cost. Its initial RFLAGS is `0x2`: the
reserved bit set, interrupts disabled, direction flag clear.

## Stacks, and the guard below each

Thread stacks live in their own region above the kernel heap's window, four
pages each, with **one unmapped page below every stack**.

The stride places each slot's guard directly above the previous slot's highest
stack address, so a stack that runs off *either* end meets a page that is never
mapped rather than a neighbouring thread's frame. `thread_verify` re-derives all
of this from the page tables rather than from the table's own arithmetic: every
guard must translate as absent and every stack page as present, writable and at
4 KiB granularity.

Slot zero is the boot thread — the context this kernel has been running on since
`boot.S`. It is *adopted*, not created: its stack already exists, this layer did
not map it, and it has no prepared frame because it is not suspended. Its stack
region is deliberately left unmapped so that slot index and stack address stay
the same arithmetic for every thread.

## Bounds and refusals

- **The run queue is a linear scan over a fixed table**, picking the next ready
  thread after the current one and wrapping. Deliberately predictable, so a test
  can require an exact rotation rather than merely that everything eventually
  ran — the same reasoning that keeps `heap.c` on first fit.
- **A yield with nothing else ready returns without switching.** That is a
  description of the run queue, not a failure.
- **The boot thread may not exit** — nothing created it and there would be
  nothing to return to — and **may not be the only thing left**: a last runnable
  thread exiting is a deadlock, and it is named rather than halted into.
- **A thread may not join itself**, and an identifier naming nothing is refused.
- **Stopping is refused while any thread is still runnable.** Tearing down then
  would unmap a stack still holding a suspended frame, and the failure would
  surface as a fault at an address belonging to a thread that no longer exists.
- **Only the boot thread may stop the scheduler.**
- **Interrupts must be disabled** for start, create and stop. The run queue is
  not reentrant and nothing yet arrives to make it so.
- **The table is one heap allocation** made at `thread_start` and released at
  `thread_stop`, following the pattern `timer.c` set: never per operation.

## Executable proof

`thread_self_test` runs on every boot before any stack is mapped: the stack
layout is checked to be disjoint and correctly guarded for every slot; the
rotation is driven over a synthetic table through wrap, skip, exited, running
and nothing-ready; the prepared frame is built into a private arena and every
slot compared, including that nothing below it was touched and that the initial
flags have the reserved bit set with IF and DF clear; and every entry point is
required to refuse by name before the system is started.

Normal boot then creates three threads that rotate through a shared log:

```text
Seneri OS: threads online, 3 ready of 8 on 12 stack frames
Seneri OS: thread rotation 123123123123
Seneri OS: threads switched 20 times, 3 exited
Seneri OS: kernel threads established
```

**The rotation is asserted exactly.** A scheduler that runs everything eventually
but not in the order it claims is one no caller can reason about, and "all three
threads ran" would not have caught it. The boot proof also requires every stack
frame and every interior page table those mappings needed to come home, exactly
as `prove_paging_lifecycle` does.

The `threads` scenario covers what normal boot does not reach: filling the table
to capacity and being refused exactly one past it, distinct identifiers, joining
self and joining nothing, stopping while a thread is runnable, each worker
confirming `thread_current` answers about the thread actually running, and every
stack and guard being absent after teardown.

The `thread-guard` scenario walks off a stack and requires the fault, at the
guard address, on the created thread's own stack — which also proves the fault
path works on a stack this layer allocated rather than only on `boot.S`'s.

### Negative controls

Each applied to a clean tree, rebuilt, run, and reverted.

| Breakage | Observed failure |
| --- | --- |
| the switch does not preserve callee-saved registers | normal boot never completes |
| the outgoing stack pointer is recorded before the registers | normal boot never completes |
| the run queue ignores thread state | `PANIC: thread table and stack layout self-test failed` |
| an exiting thread is not marked exited | `PANIC: an exited thread was scheduled again` |
| stopping does not unmap the stacks | `PANIC: starting and stopping threads did not return every frame` |
| the guard page is additionally mapped | `ST FAIL thread-guard: the thread guard page is mapped`, and `PANIC: thread table does not match the address space` on normal boot |
| the stack stride leaves no guard between stacks | **the build fails**: `static assertion failed: "the thread guard page moved and the fault diagnostic no longer matches"` |
| a normal-boot contract line is renamed | `normal scenario did not complete the integrated production path` |

Three deserve a note.

**The two switch controls fail by hanging, not by diagnosing.** Corrupting the
register save produces a machine that stops making progress rather than one that
reports anything, because the thing that would report is itself running on a
corrupted frame. That is the honest shape of this failure and it is recorded
rather than dressed up: the scenario times out, which the harness treats as a
failure, but no diagnostic is possible and none should be claimed.

**The guard control had to be re-aimed.** The first attempt mapped the stack
starting *at* the guard page, which shifts the whole stack down by a page — so
the run faulted while writing the prepared frame, long before reaching the guard
check, and reported `fatal interrupt did not match its expectation`. The control
was rewritten to map the guard *in addition* to a correct stack, and then two
independent checks catch it: the scenario's own guard assertion, and
`thread_verify` on normal boot.

**Deleting the unmap could not be compiled.** `-Werror=unused-function` rejected
the build before anything ran, exactly as `docs/VIRTUAL_MEMORY.md` records for
the page-table reclamation controls. It was neutralised in a way the compiler
accepts instead, and then failed as intended.

## What a real stack overflow does — measured

The `thread-guard` scenario proves the guard page is unmapped and faults on an
explicit write. It does **not** prove what happens on a true overflow, where RSP
itself has reached the guard. That was measured separately rather than reasoned
about, and the measurement took two attempts.

**The first attempt produced no overflow at all.** A recursive probe of the
obvious shape — `return filler[0] + probe(depth + 1)` — ran to its bound and
returned normally. GCC's accumulator transform had turned the recursion into a
loop, so no frame was ever pushed. That is a non-result and it is recorded
because the natural way to write this control does not test what it appears to.

Rewriting the probe to read its own frame *after* the recursive call forces the
frame to persist, and that overflows. The result:

```text
Seneri OS DOUBLE FAULT - HALTED
```

So a real overflow is **contained and deterministic** — the double-fault IST
catches it and the machine halts safely rather than triple faulting — but it is
**not diagnosed as an overflow**. The page fault is taken with RSP already on the
guard, so the handler cannot push its own frame and the fault escalates. The
guard still does its job: it converts silent corruption of a neighbouring
thread's stack into a contained halt. It just cannot yet say whose stack ran out.

Fixing that means giving the page fault its own IST stack, which is listed below
rather than done here because it is a change to the interrupt tables and belongs
with a focused review of them.

## Deferred work

- **No preemption.** A thread runs until it yields or returns. The switch
  already saves the flags so that adding a timer-driven switch does not change
  what a resumed thread's interrupt state is; the scheduler tick and the
  re-entrancy it implies are the next increment.
- **`timer_sleep_ns` still halts.** It is the reason this layer exists, and
  turning a sleep into a block that yields is the increment that finally makes
  `docs/MONOTONIC_TIME.md`'s "nothing sleeps concurrently" false.
- **Exited threads keep their stacks until `thread_stop`.** Freeing a stack from
  the thread running on it would pull the ground out from under the switch that
  has not happened yet. Reaping after the switch is a few lines and a real
  ordering subtlety, and it is deferred for the same reason the heap never
  shrinks: there is no caller whose behaviour depends on it.
- **A stack overflow cannot be diagnosed**, only contained. See above; it needs
  an IST for the page fault.
- **No priorities, no fairness beyond round robin, no blocking primitives.**
  There is nothing to contend for yet: no lock, no queue, no device.
- **Fixed stack size.** Four pages each, chosen because it is what `boot.S`
  gives the thread this kernel starts on. A thread that needs more has no way to
  say so and will meet its guard.
- **Nothing is per-processor.** The run queue, the current slot and the table are
  single statics. A second core needs its own of each, and a lock over what they
  share.
- **Verified under QEMU only.**
