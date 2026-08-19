<p align="center">
  <img src="assets/seneri-logo.png" alt="Seneri OS logo" width="420">
</p>

# Seneri OS

[![verify](https://github.com/saudaljuaid/Seneri-OS/actions/workflows/verify.yml/badge.svg)](https://github.com/saudaljuaid/Seneri-OS/actions/workflows/verify.yml)

Seneri OS is a new, independent operating system built from first principles. It
is not a Linux distribution and it does not currently promise application or
hardware compatibility with an existing operating system.

The repository is at its foundation stage. It contains a deliberately small
x86_64 kernel seed—not a finished operating system and not a simulation.

## What boots today

GRUB loads a Multiboot2-compliant ELF kernel in 32-bit protected mode. Seneri
then validates the handoff and CPU, identity-maps the first 4 GiB, enables long
mode, installs a known GDT and stack, and transfers control to freestanding C.
The C kernel defensively validates every Multiboot2 tag, constructs a bounded
physical-frame allocator from the firmware memory map, proves allocation and
release, installs a complete IDT and production GDT/TSS, routes fatal CPU
exceptions through deterministic diagnostics, proves recoverable interrupt
entry plus PIT delivery, validates the firmware ACPI root, walks the checksummed
system-description tables to the MADT, parses that table's
interrupt-controller records into a validated processor, I/O APIC, and
interrupt-override topology, brings the bootstrap processor's local APIC online
in virtual wire mode, delivers the timer through a programmed I/O APIC
redirection entry, retires the legacy 8259 pair, discovers the ACPI power
management timer from the FADT, calibrates both the local APIC timer and a
time-stamp counter against that reference — whose rate is fixed by specification
rather than measured — retires the 8254 and proves all three clocks still agree
about an interval with it gone, establishes a monotonic clock and deadline
timers and sleeps on one, then builds and installs its own four-level page
tables — read-only executable text, read-only rodata, writable non-executable
data, uncacheable APIC registers, an absent null page — walks them in software
to prove no page is both writable and executable, takes a page fault
deliberately to prove the hardware agrees, and finally opens a guarded, bounded
kernel heap on that address space, growing it a page at a time and proving the
memory it hands out is real and disjoint, before halting safely.

The day-one success contract is the serial line:

```text
Seneri OS: day one passed
Seneri OS: memory foundation passed
Seneri OS: never triple fault milestone passed
Seneri OS: ACPI root verified
Seneri OS: ACPI MADT verified
Seneri OS: ACPI topology verified
Seneri OS: local APIC online
Seneri OS: I/O APIC online
Seneri OS: I/O APIC delivered eight interrupts
Seneri OS: legacy 8259 retired
Seneri OS: timer survives legacy retirement
Seneri OS: local APIC timer delivered eight interrupts
Seneri OS: TSC reference established
Seneri OS: ACPI FADT verified
Seneri OS: ACPI configuration windows verified
Seneri OS: PM timer independent reference established
Seneri OS: PIT retired
Seneri OS: clocks survive PIT retirement
Seneri OS: deadline timers online
Seneri OS: monotonic time established
Seneri OS: kernel page tables installed
Seneri OS: no writable executable mapping
Seneri OS: virtual memory established
Seneri OS: kernel heap online
Seneri OS: heap coalesced to one free block
Seneri OS: kernel heap established
Seneri OS: deadline table of 32 entries on the heap
Seneri OS: PCI configuration space enumerated
Seneri OS: PCI enumeration established
Seneri OS: kernel threads established
Seneri OS: preemption established
Seneri OS: framebuffer established
Seneri OS: logo established
```

## Build and prove it

On Ubuntu 24.04 or a compatible Debian-based environment, install:

```sh
sudo apt-get install binutils gcc grub-common grub-pc-bin make mtools qemu-system-x86 xorriso
rustup target add x86_64-unknown-none
```

Most of Seneri is C11 and GNU assembly. One component is Rust: the boot logo
decoder, which reads a byte stream the kernel did not produce.
`docs/RUST.md` states where that line goes and why it is not where it is usually
put.

Then run:

```sh
make verify   # clean build plus ELF, Multiboot2, symbol, and W^X checks
make smoke      # run the strict normal-boot QEMU protocol
make qemu-tests # run twenty-three deterministic fault, device and thread scenarios
make run      # optional interactive boot
make hooks    # enforce verification in this local clone
```

## Repository map

- `src/arch/x86_64/boot.S` — Multiboot2 header and 32-to-64-bit transition.
- `src/arch/x86_64/interrupts.S` — normalized interrupt entry and fatal probes.
- `src/kernel/interrupts.c` — IDT ownership, dispatch, and fault diagnostics.
- `src/kernel/cpu.c` — permanent GDT, TSS, and emergency IST stacks.
- `src/kernel/pic.c` and `pit.c` — legacy IRQ routing and timer proof.
- `src/kernel/multiboot2.c` — bounded parser for the boot information contract.
- `src/kernel/physical_memory.c` — 4 KiB physical-frame ownership and allocation.
- `src/kernel/acpi.c` — defensive ACPI RSDP validation and root discovery.
- `src/kernel/acpi_tables.c` — bounded RSDT/XSDT walking, MADT, FADT and MCFG
  discovery.
- `src/kernel/acpi_madt.c` — bounded MADT record walking and interrupt topology.
- `src/kernel/acpi_util.c` — shared firmware-table primitives and wire sizes.
- `src/kernel/apic.c` — local APIC bring-up, virtual wire routing, and identity.
- `src/kernel/ioapic.c` — I/O APIC redirection entries and ISA override routing.
- `src/kernel/apic_timer.c` — local APIC timer calibration and periodic ticks.
- `src/kernel/tsc.c` — time-stamp counter calibration and duration arithmetic.
- `src/kernel/pm_timer.c` — ACPI PM timer, wrap folding, and bounded waiting.
- `src/kernel/clock.c` — the monotonic clock and its one origin.
- `src/kernel/timer.c` — deadline timers on the APIC timer's one-shot mode.
- `src/kernel/paging.c` — the kernel's own page tables and the W^X guarantee.
- `src/kernel/heap.c` — the guarded, bounded, transactional kernel heap.
- `src/kernel/pci.c` — PCI configuration space, read two independent ways.
- `src/kernel/thread.c` — kernel threads, guarded stacks, and the run queue.
- `src/arch/x86_64/thread.S` — the context switch and where a new thread starts.
- `src/kernel/framebuffer.c` — every pixel on the screen, and proof of each one.
- `src/rust/logo.rs` — the boot logo decoder, in Rust, and its refusals.
- `tools/make-logo-asset.py` — turns the logo into what the kernel can decode.
- `linker.ld` — low-memory ELF layout with separate, page-aligned R, RX, and RW
  segments.
- `docs/ACPI_TABLES.md` — firmware-table bounds, invariants, and test protocol.
- `docs/ACPI_TOPOLOGY.md` — interrupt-topology invariants and test protocol.
- `docs/LOCAL_APIC.md` — local APIC invariants, virtual wire mode, and proof.
- `docs/IO_APIC.md` — redirection invariants, override routing, and proof.
- `docs/LEGACY_RETIREMENT.md` — how the 8259 pair is latched shut, and proof.
- `docs/APIC_TIMER.md` — why the APIC timer needs calibration, and its proof.
- `docs/TSC.md` — the second clock, why it exists, and what it cannot claim.
- `docs/PM_TIMER.md` — the first unmeasured reference, and the error it found.
- `docs/PIT_RETIREMENT.md` — recalibrating on that reference, and losing the 8254.
- `docs/MONOTONIC_TIME.md` — an instant, a deadline, and how bounded a sleep is.
- `docs/VIRTUAL_MEMORY.md` — owning the tables, and making W^X true on the metal.
- `docs/KERNEL_HEAP.md` — the first allocator that is not a fixed array.
- `docs/PCI_ENUMERATION.md` — counting the machine, and two readers checking each
  other.
- `docs/HARDWARE_AND_APPLICATIONS.md` — the costed route to drivers, wireless,
  and running programs.
- `docs/THREADS.md` — more than one thread of control, and the page below each.
- `docs/FRAMEBUFFER.md` — pixels, and why a picture is not proof of one.
- `docs/RUST.md` — the one component that is not C, and where that line goes.
- `docs/SCREEN_CONSOLE.md` — text on the framebuffer, and the font it draws
  from.
- `docs/DEBT.md` — a measured register of what the whole thing is carrying.
- `docs/NEVER_TRIPLE_FAULT.md` — interrupt ABI, invariants, and test protocol.
- `docs/MAP.md` — where every file is, what it does, and the order boot runs
  them in. Start here.
- `docs/WORKING_ON_SENERI.md` — how to change a layer and get it onto `main`,
  alone, including what to do when the build refuses you.
- `CONTRIBUTING.md` — non-negotiable engineering and commit rules.

## Current boundaries

Every interrupt Seneri owns now arrives through discovered hardware, the timer
interrupt originates in the processor's own local APIC, and both derived clocks
are calibrated against the ACPI power management timer, whose rate is fixed by
specification and measured against nothing. The 8254 is retired: it is stopped,
masked and latched shut, and the three clocks are proved to still agree about an
interval with it gone. Getting here took finding that the PIT had been delivering
two interrupts per programmed period, which had left both calibrated clocks
running at half their true rate while still agreeing with each other.

Those rates are now usable time: one monotonic clock with a single origin, and
deadlines armed on the APIC timer's one-shot mode, so code can ask to be woken at
an instant rather than count ticks. Boot sleeps on one to prove it.

The kernel also owns its page tables. It used to run on the 4 GiB of huge pages
`boot.S` builds before long mode, every one of them writable *and* executable,
because `EFER.NXE` was never set — so for as long as `make verify` had been
refusing an RWX load segment, the machine underneath had been entirely RWX. That
check inspected the file. Seneri now builds a four-level hierarchy with per-page
permissions, enables the no-execute bit and supervisor write protection, and
walks the installed tables in software at boot to assert that no page is both
writable and executable. The `paging` scenario writes to a page it just made
read-only and passes only on the resulting fault, so the claim rests on the
hardware refusing rather than on a table being read back.

The address space now carries a heap. `heap_allocate` and `heap_free` work in
bytes rather than 4 KiB frames, inside a 16 MiB window with an unmapped guard
page on each side, grown one page at a time and backed by frames that need not
be contiguous. Its metadata lives outside the memory it manages, so an overrun
cannot corrupt the allocator, and the `heap` scenario proves the guard by
walking off the end and taking the fault.

Above the heap, Seneri now knows what is actually plugged into the machine. It
reads the MCFG table firmware publishes, carves the configuration window it names
out of the identity map as uncacheable device memory, and enumerates every bus
reachable from bus zero — following bridges rather than sweeping 256 buses,
refusing a capability list that loops, and recording what each function is and
whether it can raise a message-signalled interrupt. Every access is a read, so
counting the machine cannot disturb it.

Configuration space is reachable two entirely different ways, so both are
implemented and each is made to check the other: enumeration runs on the I/O
ports every PC has, and the memory window is then required to agree with them
register for register on every function inside it. The same argument the three
clocks make, one layer up. A machine that declares no window — the default QEMU
machine is one — is not a failure; it runs on the ports alone and says so.

There is now more than one thread of control. A context switch saves the
callee-saved registers and the flags on the stack it was called on, so changing
the stack pointer changes which return address the final `ret` uses; every
suspended thread is sitting inside that one function waiting to finish. Threads
take their table from the heap and their stacks from the page tables, four pages
each with an unmapped guard page below — so a stack that runs off either end
meets a fault rather than a neighbour's frame — and the context this kernel
booted on is adopted as the first thread rather than special-cased. Boot creates
three and requires them to rotate in exactly the order the run queue promises,
then proves every frame and every interior page table came home.

There is also a screen. The Multiboot2 header asks the loader for a linear
framebuffer, optionally, so a loader that cannot set a graphics mode still boots;
what comes back is validated field by field rather than assumed, carved out of
the identity map as uncacheable device memory across as many 2 MiB regions as it
spans, and then proved. Boot writes a pattern whose colour is a function of the
coordinates and reads all 786,432 pixels back, because a framebuffer looks right
long before it is right and `CONTRIBUTING.md` says screenshots are not proof.

What is missing sits above that layer. The heap has its first consumer — the
deadline table is obtained from it at `timer_start` and returned at
`timer_stop` — but the ACPI topology and the interrupt tables are still fixed
arrays, and converting each is its own change. The heap never
shrinks — though the page tables underneath it are now reclaimed when an unmap
empties them — and `timer_sleep_ns` still halts rather than
blocking, so nothing sleeps concurrently yet. The scheduler now takes the
processor back on its own — three threads whose bodies contain no scheduler call
at all are timesliced within 2% of each other — but a sleeping thread still
holds its turn instead of leaving the run queue, and turning a sleep into a block
is the increment that changes that. The kernel is still identity-mapped rather than
higher-half, a 4 KiB change inside a 2 MiB mapping is refused rather than split,
and the page-fault handler stays fatal: there is no demand paging. There is no
wall-clock date, only time since boot. The supported target still reports no
invariant counter, so the TSC's rate being correct is not the same as its rate
being stable. Level-triggered I/O APIC routing still needs directed EOI,
which gates legacy device interrupts — though the enumeration above found that
every PCI Express endpoint on the tested machines offers message-signalled
interrupts, which are a memory write to the local APIC and so are edge-triggered
by construction, so that gap is no longer on the road to device drivers. Everything here is verified under QEMU; the
uncacheable APIC mappings in particular are the kind of change that fails only
on real hardware. No base address register is sized and no device is claimed, so the
enumeration is a list rather than a driver; `docs/HARDWARE_AND_APPLICATIONS.md`
costs the route from that list to storage, wireless, and running programs, and
records why Linux driver source is not on it. The logo is on that screen at boot, decoded from a run-length image by the one
Rust component in the kernel and blitted by C, with all 65,536 of its pixels
read back off the framebuffer and compared against the decode before boot
continues. There is still no text: nothing yet knows what a character is, and the
framebuffer is uncacheable rather than write-combining, so every store to it is
a bus cycle. A thread that overflows its stack is contained by
its guard page and the double-fault stack, but cannot yet be diagnosed as an
overflow, because the page fault has no interrupt stack of its own. It has a
deliberately narrow single-core foundation, with no preemption, userspace,
filesystem, networking, graphics, or hardware drivers. Those arrive only after the previous layer has an executable
acceptance test.

Seneri OS is licensed under GPL-3.0; see `LICENSE`.
