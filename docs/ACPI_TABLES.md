# ACPI table trust boundary

This increment turns the validated RSDP into one bounded path to the Multiple
APIC Description Table. It discovers interrupt-controller metadata without
changing interrupt-controller state.

## System-description table invariants

Seneri follows the RSDT only for an ACPI 1.0 root and otherwise follows the
XSDT selected during RSDP validation. The root signature must agree with that
selection. Its declared length must include the 36-byte ACPI description header,
fit within Seneri's current first-4-GiB identity map, and contain a whole number
of 32-bit RSDT or 64-bit XSDT entries. The complete table checksum must be zero.

Firmware controls every length and pointer in this path. Early discovery
therefore limits a root to 256 entries and any individual table to 1 MiB. These
are Seneri policy bounds, not ACPI architectural limits. They keep every loop
finite until the virtual-memory manager can map firmware tables individually.

Each root entry is decoded from its little-endian byte representation so an
XSDT's naturally four-byte-aligned 64-bit entries do not create an unaligned C
access. Before Seneri examines a referenced signature, the address must be
nonzero and the fixed header must fit in the early map. Before it consumes the
table, the complete declared span and checksum must also be valid.

## MADT invariant

Exactly one referenced table must carry the `APIC` signature. Its length must
include the 44-byte fixed MADT prefix, and every reserved MADT flag must be zero.
Discovery records the table address, revision, OEM identity, local APIC address,
and `PCAT_COMPAT` flag. It deliberately does not parse interrupt-controller
structures or touch local APIC, I/O APIC, PIC, or PIT registers.

The frame allocator continues to treat ACPI reclaimable and NVS memory as
reserved, so the discovered table cannot be recycled after discovery.

## MCFG invariant

The memory-mapped configuration table is the first table Seneri reads whose
**absence is not a fault**. A machine with no PCI Express host bridge has no
reason to publish one, so `acpi_mcfg_discover` returns
`ACPI_STATUS_MISSING_MCFG` and the caller decides what that means. Everything
else about it is refused exactly as the MADT and the FADT are: at most one table
may carry the `MCFG` signature, and a second is a refusal rather than a race
between two descriptions of the same hardware.

PCI Firmware Specification 3.3 section 4.1.2 gives the table a 44-byte prefix -
the description header plus eight reserved bytes - followed by a whole number of
sixteen-byte allocation structures. A payload that is not a whole number of them
describes a layout this reader does not know, and is refused rather than
truncated to the part that happens to fit. A table with no allocation at all is
refused too: firmware that publishes the table is asserting a window exists.

Each allocation names a 64-bit base address, a segment group, and an inclusive
start and end bus. Four things must hold, and each has its own refusal:

- The last bus may not precede the first.
- The base may not be zero, and must be 1 MiB aligned. PCI Express Base
  Specification 6.1 section 7.2.2 puts the bus number in address bits 27:20, so
  a base that is not a multiple of 1 MiB cannot be indexed the way the window
  exists to be indexed. This is a structural impossibility, not a preference.
- The base plus the window's own size may not run past the end of the address
  space, so a high base is refused rather than wrapped to a low address that
  would read back as ordinary memory.
- No two windows may claim the same buses of the same segment group, **or** the
  same memory whatever buses they claim. Either would let one physical address
  mean two devices, and resolving it by position would silently pick one.

Discovery records the description only. It never reads the memory the window
names; `src/kernel/pci.c` does that, and it is a separate increment.

## Executable proof

The in-kernel rejection suite constructs valid RSDT and XSDT graphs and then
proves rejection of a mismatched root signature, partial or excessive root
entries, bad root and child checksums, null and out-of-map table addresses,
short child tables, a missing or duplicate MADT, a short MADT, and nonzero
reserved MADT flags.

The MCFG suite is built the same way, by editing a fixture that is otherwise
accepted so that each rejection is reached with everything before it valid: a
payload one byte short of a whole allocation, a table that stops before its
first allocation, a last bus below its first, a zero base, a base one byte off
1 MiB alignment, a base whose window runs past the end of the address space, two
windows of different segments on the same memory, two windows of one segment on
the same buses, an absent table, and a duplicate one. The acceptance case checks
the decoded fields against what the fixture wrote, which is what makes the
rejections mean anything.

The normal QEMU scenario must also walk SeaBIOS's real ACPI tables and emit:

```text
Seneri OS: ACPI MADT verified
Seneri OS: ACPI configuration windows verified
```

The default QEMU machine is i440fx, which has no PCI Express host bridge and
publishes no MCFG, so the normal scenario proves the **absent** path on every
run and reports `Seneri OS: ACPI MCFG absent`. The present path is proved by
booting the same image on `-machine q35`, where firmware declares one window at
`0xB0000000` covering all 256 buses of segment 0.

### Negative controls

Each applied to a clean tree, rebuilt, run, and reverted.

| Breakage | Observed failure |
| --- | --- |
| the 1 MiB alignment requirement is dropped | `PANIC: ACPI table rejection self-test failed` |
| overlapping windows are accepted | `PANIC: ACPI table rejection self-test failed` |
| a partial allocation structure is accepted | `PANIC: ACPI table rejection self-test failed` |
| absence is treated as a fault | `PANIC: ACPI root table does not contain an MCFG` on the default machine, while **the same binary still passes on `-machine q35`** |

The last one is the one worth keeping. It is the only control here whose result
depends on the machine rather than on the fixture, so it is what proves the
reader is answering from firmware rather than from anything compiled into it:
one kernel, two machines, two different answers, both correct.

## Deferred work

- **Nothing reads the window yet.** `acpi_mcfg_discover` describes memory it
  never touches. Mapping it uncacheable and reading configuration space through
  it is `docs/PCI_ENUMERATION.md`.
- **A window above 4 GiB is described but unusable.** Discovery does not require
  the window to fall inside the early identity map, because describing it and
  reaching it are different questions. The consumer is what has to refuse.
- **Segment groups beyond the first are recorded and otherwise ignored.**
  Nothing in Seneri yet has a second segment to address.
- **The eight reserved bytes are not checked for zero.** The specification calls
  them reserved; refusing on them would reject firmware over a field no reader
  uses, which is a refusal with no failure behind it.

`docs/ACPI_TOPOLOGY.md` covers the next increment, which parses the MADT's
variable-length records with the same bounded discipline. Only after that
topology is proved may Seneri mask the legacy PIC permanently, route a timer
through discovered APIC hardware, and retire the PIT proof.
