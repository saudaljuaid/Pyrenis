# PCI configuration space

Every driver Seneri will ever have begins by being found. This increment finds
what is on the machine — buses, devices, functions, what class each one is, and
which of them can raise a message-signalled interrupt — and does nothing else
with it.

## What this layer does not do

It is worth stating first, because the restraint is the design.

Every access this layer makes is a **read**. Configuration reads have no side
effects, so enumerating a machine cannot disturb a device that is already
working — including the one the serial console is talking through. Sizing a base
address register means writing all ones into it and reading back which bits
stuck, and doing that to the wrong function is how an enumerator takes a working
machine off its own console. Writing to configuration space belongs to the
increment that also owns a device, not to the one that counts them.

So there is no bus mastering enabled, no interrupt routed, no BAR sized, no
device claimed. `pci_verify` re-reads and compares; it does not repair.

## Two mechanisms, and why both

There are two ways to reach configuration space, and this file implements both
because that is what lets each one check the other.

**Configuration mechanism #1** (PCI Local Bus Specification 3.0 section
3.2.2.3.2) is a pair of I/O ports: a 32-bit selector latched into `0xCF8` and
the chosen register read through `0xCFC`. It needs no mapping, has existed on
every PC since PCI did, and reaches the first 256 bytes of every function of
every bus. It carries no segment group, so it reaches group zero only.

**The enhanced configuration access mechanism** (PCI Express Base Specification
6.1 section 7.2.2) is a window of memory where the bus occupies address bits
27:20, the device 19:15, the function 14:12 and the register the low twelve.
Firmware says where it is in the MCFG table; `docs/ACPI_TABLES.md` covers
finding and validating that. It is the only way to reach the extended
configuration space above 256 bytes, and it must be mapped before it can be
read.

**Enumeration runs on the ports**, because they are bounded by nothing and
present everywhere. The window is then required to agree with them.

## The agreement invariant

For every function that falls inside the mapped window, every 32-bit register of
the first 256 bytes must read identically through both mechanisms.

This is the same argument the three clocks make in `docs/PIT_RETIREMENT.md`. Two
readers built independently, reaching the same registers by completely different
routes — two I/O instructions versus a load from uncacheable memory whose
address is computed from a firmware table — have no reason to agree about
anything unless both are addressing the function the enumeration believes they
are. One assertion therefore covers the window's base, its bus stride, its
device and function strides, the mapping's cacheability, and whether the
firmware description was read correctly.

A register that does not read the same twice **through one mechanism** cannot
say anything about two, so it is counted as unstable and skipped rather than
reported as a disagreement. On the tested machines nothing is ever skipped;
`volatile_dwords` is reported anyway so that a machine where something is does
not look like a machine where everything was compared.

## Bounds and refusals

Firmware and hardware control every value in this path, so each way of not
terminating has its own refusal.

- **Buses are found through bridges, not swept.** A worklist starting at bus
  zero, each bus scanned at most once. A machine that populates buses 0 and 5
  probes two buses, not 256. Scanning each bus once is also what makes a bridge
  whose secondary bus leads back into an already-scanned bus terminate; a bridge
  that names *its own* bus is refused by name, because a description that wrong
  makes everything else read from it suspect.
- **Only function zero's header type says the others exist.** Probing functions
  1 through 7 of a single-function device is how an enumerator invents devices.
- **A vendor identifier of `0xFFFF` means absent.** Nothing drives the bus for a
  function that is not there, so it floats high. The `pci` scenario requires an
  empty slot on bus zero to read all ones in *every* register, not only the
  vendor, because a machine answering zero for the rest would let a decoder
  invent a device with class zero at every empty slot.
- **The capability list is bounded three ways**: a pointer below `0x40` is
  inside the standard header and refused, a position already visited is a cycle
  and refused, and the number of dword-aligned positions bounds the walk even if
  both of those passed. There is deliberately **no** upper-bound check: a
  capability pointer is one byte and every position is dword aligned, so the
  largest value it can hold after masking is `0xFC`, which is exactly the last
  register. A `_Static_assert` states that instead, because a check no input can
  reach is a check no test can drive.
- **The function table is one heap allocation**, made at `pci_initialize` and
  released at `pci_shutdown`, following the pattern `src/kernel/timer.c` set:
  never per operation, because the heap is not reentrant. Exceeding
  `PCI_MAX_FUNCTIONS` is a refusal, not a truncation — a silently short device
  list is worse than no device list.
- **Interrupts must be disabled.** The port pair is two registers used as one,
  so anything that ran between the address write and the data read would answer
  about a different function.

## The window is device memory

Configuration space read through a cached mapping would be answered from
whatever a cache line held when it was last filled. The window is therefore
mapped uncacheable, for the same reason the APIC windows are.

`paging.c` owns that decision, because `paging.c` owns the address space. It
carves the window out of the identity map as a device region — one 2 MiB region
of 4 KiB pages with `PAGING_WRITE | PAGING_UNCACHED` — exactly as it already
does for the local APIC, the I/O APICs and the VGA buffer, and reports the
result in `paging_state.ecam_window_base`. `pci.c` reads that decision rather
than making its own.

Three things must hold for the window to be carved out, and **none of them
failing is an error**:

- Firmware must declare one at all. A machine with no PCI Express host bridge
  does not, and its configuration space is still fully reachable through the
  ports.
- Its base must be 2 MiB aligned, because a device region is one whole region of
  the identity map.
- It must lie inside the early identity window.

When any of those fails, `ecam_window_base` stays zero, `pci.c` runs on the
ports alone, and boot says so. This is deliberately **not** the shape
`docs/IO_APIC.md` uses for a missing directed-EOI register: refusing to boot
there is right because there is no other way to route a level-triggered
interrupt, and refusing here would be wrong because there is a complete, tested,
universally available alternative.

One consequence is stated rather than hidden: Seneri maps **two buses** of a
window firmware may declare as 256. That is every bus any machine it is tested
on populates, and a register past the mapped region is refused rather than
wrapped. Reaching further, and reaching extended configuration space at all,
needs the window mapped somewhere of its own; that is a later increment.

## Executable proof

`pci_self_test` runs on every boot before any hardware is read. It is arithmetic
and list walking over synthetic values: address composition with every field at
its maximum so a shift one bit out lands in a neighbouring field; window
displacement for bus, device, function and register, including the first and
last register the mapping can reach and the first one past it; the capability
walk over a chain that terminates, one that points into the header, one that
points back at its head, one that points at itself, and one that ends at the
last legal position; field extraction from a dword by absolute offset; and every
refusal both readers owe.

Two QEMU scenarios then prove it against real machines.

`pci` runs on the default machine, which is i440fx and has no PCI Express host
bridge. It proves the path that has nothing but the ports: no window is mapped,
the window reader refuses by name rather than reading address zero, no
comparison is reported, the host bridge is at 00:00.0, an empty slot reads all
ones everywhere, every register of every function reads the same twice, the
class lookup finds a class that is not the first function and finds nothing for
an unassigned class, and the table is released.

`pci-ecam` runs on `-machine q35` with a PCIe root port and two `e1000e`
endpoints. It proves the window: it is mapped where firmware said, **every page
of it translates as uncacheable at 4 KiB granularity**, every function was
compared and not merely some, enumeration crossed a bridge onto a second bus, a
recorded MSI-X capability is really at the offset recorded for it, the window
gives different answers for different functions, and a bus past the mapped
region is refused.

Normal boot reports:

```text
Seneri OS: PCI mechanism 1 online, no window mapped
Seneri OS: PCI buses 1 functions 6 bridges 0
Seneri OS: PCI configuration space enumerated
Seneri OS: PCI mechanisms agree on 0 registers of 0 functions, 0 unstable
Seneri OS: PCI enumeration established
```

and on q35 with the scenario's hardware:

```text
ST PCI window agreed on 576 registers of 9 functions across 2 buses, 4 with MSI-X
```

### Negative controls

Each applied to a clean tree, rebuilt, run, and reverted.

| Breakage | Observed failure |
| --- | --- |
| the window's bus stride is one bit too wide | `PANIC: PCI configuration arithmetic self-test failed` |
| the window ignores the device number | `PANIC: PCI configuration arithmetic self-test failed` |
| the adopted window base is one function slot high | `ST FAIL pci-ecam: PCI configuration mechanisms disagree about a register` |
| the window read is one register off | `ST FAIL pci-ecam: PCI configuration mechanisms disagree about a register` |
| the window is mapped write-back instead of uncacheable | `ST FAIL pci-ecam: the configuration window is not device memory` |
| bridges are recorded but never traversed | `ST FAIL pci-ecam: enumeration did not cross a bridge` |
| the two mechanisms are never compared | `ST FAIL pci-ecam: the two mechanisms were not compared everywhere` |
| the capability list may revisit a position | `PANIC: PCI configuration arithmetic self-test failed` |
| an absent function is recorded as a device | `ST FAIL pci: PCI capability list contains a cycle` |
| the class lookup ignores its arguments | `ST FAIL pci: the class lookup did not find the network device` |
| a normal-boot contract line is renamed | `normal scenario did not complete the integrated production path` |
| the `pci` scenario is run on `-machine q35` (no source change) | `ST FAIL pci: a configuration window was mapped on this machine` |

Five of these deserve a note.

**The first two were caught by the wrong thing, and that is a result.** Breaking
the window's bus or device stride was meant to prove the two-mechanism
comparison works. It never got that far: the self-test refused the boot first.
That is the right order to fail in, but it meant the comparison itself was still
unproved, so two further controls were aimed at things the self-test cannot see
— the base adopted from firmware, and the read itself. Both produce
`PCI configuration mechanisms disagree about a register`, which is the claim
this increment exists to make and now the only control that makes it.

**The scenario's refusals initially did not name themselves.** The first run of
those two controls reported `PCI enumeration would not initialize`, which says
nothing about why. `docs/KERNEL_HEAP.md` records the same lesson from the heap's
verify call sites; the scenarios now report `pci_status_string(status)` and the
diagnosis above is what they print.

**Mapping the window write-back initially failed to fail.** With the window
mapped as ordinary write-back memory, both scenarios still passed and both
mechanisms still agreed on every register. QEMU under TCG models no cache, so a
cached read of device memory is indistinguishable from an uncached one and *no
behavioural test on this machine can tell the difference*. Rather than record a
non-result, the claim was moved to something checkable: the `pci-ecam` scenario
now walks every page of the window with `paging_translate` and requires
`PAGING_WRITE | PAGING_UNCACHED` at 4 KiB granularity, which is the same move
`paging.c` makes when it walks its own tables instead of trusting them. The
control fails as shown above. What is proved is that the mapping is right, not
that a wrong one would misbehave here — on real hardware it would.

**The class lookup control also failed to fail at first.** The host bridge is
the first function on both machines, so a `pci_find_class` that ignored its
arguments entirely and returned function zero still satisfied every check the
scenario made. The scenario now also requires a class that is present but is
*not* the first function, and requires an unassigned class to be found nowhere.

**An absent function recorded as a device is caught, but not where expected.**
Deleting the `0xFFFF` check does not produce a count that is obviously too high;
it produces `PCI capability list contains a cycle`, because an absent function's
all-ones configuration space has the capability-list status bit set and a
capability pointer of `0xFC` that points at itself. The bound catches it. That
the diagnosis names a cycle rather than a phantom device is recorded here rather
than tidied up, because it is what a reader chasing that failure will see.

## A flake, measured

One run of `make qemu-tests` during this work failed in `prove_monotonic_time`
with `sleep overshot its deadline` — 78 ms measured for a 50 ms deadline against
a 25% tolerance. It is recorded here because it was observed, not because this
increment caused it.

The measurement runs before any code this increment adds, so the path being
timed is unchanged. That reasoning was checked rather than asserted. Twenty-five
boots of the **untouched base commit** and twenty-five of this tree, interleaved
on an idle host:

| | n | minimum | median | maximum | over tolerance |
| --- | --- | --- | --- | --- | --- |
| base commit | 25 | 50.48 ms | 50.57 ms | 50.80 ms | 0 |
| this tree | 25 | 50.49 ms | 50.57 ms | 50.75 ms | 0 |

The two distributions are indistinguishable: a 1% overshoot, with the tolerance
at 25%. Across all thirty-five boots of this tree taken during the work, two
were outliers — 78 ms and 60 ms — and **both occurred while the host was also
rebuilding**, one of them in the first scenario after a clean `make verify`.
Neither the base commit nor this tree produced one on an idle host.

So the mechanism is host scheduling under TCG, not the guest. No test was changed
for it and the tolerance was not widened: a 25% window that a real regression
would blow through is worth more than a green run. It is written down because a
25% tolerance on a 50 ms sleep is not as much headroom as it looks on a loaded
CI machine, and the next person to see it should not have to rediscover this.

## Deferred work

- **No base address register is sized, and nothing is mapped.** That is the next
  increment and the one that first *writes* configuration space. It has to
  disable a function's decode while it probes, which is the first operation here
  that can take a working device away from whoever is using it.
- **Extended configuration space is unreachable.** Both readers stop at 256
  bytes. The window would reach 4096, but Seneri maps two buses of it as one
  identity-map region and reads no further than the ports can, so the two
  mechanisms compare like for like. Reaching the rest needs the window mapped in
  its own virtual range.
- **MSI and MSI-X are found, not programmed.** They are why capabilities are
  read at all: a message-signalled interrupt is a memory write to the local
  APIC, so it is edge-triggered by construction and needs no I/O APIC
  redirection entry — and therefore none of what `docs/IO_APIC.md` defers.
  Programming one needs a vector allocator, which does not exist yet.
- **Segment groups beyond the first are recorded and ignored.** The ports cannot
  carry a segment at all, so a second group would have to be read entirely
  through a window, and there is nothing to test that against.
- **No device is claimed and no driver exists.** Enumeration is a list;
  `docs/HARDWARE_AND_APPLICATIONS.md` is the argument about what should be built
  on it.
- **Bridge windows are not read.** Secondary and subordinate bus numbers are
  recorded because traversal needs them; the memory and prefetch windows a
  bridge forwards are not, because nothing allocates address space yet.
- **Verified under QEMU only**, on i440fx and q35. Configuration space is one of
  the better-modelled parts of an emulator, but the uncacheable window is
  exactly the kind of thing that only misbehaves on iron.
