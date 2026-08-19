# Owning the page tables

Seneri looked like it enforced W^X. It did not.

`linker.ld` has separated the kernel into properly flagged segments since day
one — `text` is `FLAGS(5)`, `rodata` is `FLAGS(4)`, `data` is `FLAGS(6)` — and
`make verify` has asserted that no ELF load segment is RWX for just as long.
That assertion passes. It was also the only thing enforcing W^X, and it inspects
the **file**, not the machine.

At runtime the kernel executed on the tables `src/arch/x86_64/boot.S` builds in
32-bit mode: the first 4 GiB as 2048 huge pages, every one of them
`PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE`. There was no no-execute bit on any
of them, and there could not have been, because `boot.S` sets only `EFER.LME`
and never `EFER.NXE` — so the no-execute bit did not exist on the processor.
Every byte below 4 GiB was simultaneously readable, writable and executable:
kernel text, kernel rodata, the BSS, the frame allocator's bitmaps, the firmware
ACPI tables, and every page the frame allocator handed out.

This is the third time Seneri has found a property that was verified in name
only. A `.PHONY` bug meant the entire QEMU suite silently never ran (`bbcec6f`).
The PIT was programmed in mode 3 and delivered two interrupts per period, so
both calibrated clocks ran at half rate and agreed with each other about it
(`docs/PM_TIMER.md`). The pattern is identical each time: **the check was
necessary and was never sufficient.**

This increment builds and installs the kernel's own four-level hierarchy with
per-section permissions, and proves the permissions are enforced by taking the
fault. It does **not** add a heap. That is the increment after this one.

## Why boot.S could not simply be fixed

The kernel loads at `0x100000` and ends at `0x17F000`, so all four of its load
segments sit inside a single 2 MiB page. One page carries one permission.
Per-section permissions are therefore impossible without 4 KiB granularity over
that region, and `boot.S` runs in 32-bit mode before the frame allocator exists,
with no way to obtain the tables that would need.

## What the hierarchy contains

Four levels, `CR4.PAE` already set by `boot.S`, `CR4.LA57` refused. Nine frames:
one PML4, one PDPT, four page directories, and one page table for each 2 MiB
region that needs 4 KiB granularity.

| Range | Granularity | Permissions |
| --- | --- | --- |
| physical page 0 | — | **absent** |
| Multiboot2 header page | 4 KiB | read-only, NX |
| kernel text | 4 KiB | read-only, **executable** |
| kernel rodata | 4 KiB | read-only, NX |
| kernel data, BSS, stacks, page tables | 4 KiB | writable, NX |
| VGA text buffer `0xB8000` | 4 KiB | writable, NX, **uncacheable** |
| discovered local APIC page | 4 KiB | writable, NX, **uncacheable** |
| discovered I/O APIC pages | 4 KiB | writable, NX, **uncacheable** |
| discovered PCI ECAM window | 4 KiB | writable, NX, **uncacheable** |
| framebuffer-intersecting pages | 4 KiB | writable, NX, **write-combining** |
| everything else below 4 GiB | 2 MiB | writable, NX, **write-back** |

Every entry is supervisor-only. There is no user flag to request:
`paging_map` refuses any permission bit it does not recognise, so a caller that
meant `PAGE_USER` gets a refusal rather than a supervisor mapping it believes is
a user one. The audit counts user-reachable leaves anyway, and boot panics if it
ever finds one.

Nothing sets the global bit, which is why a CR3 load is a complete flush.

### The null page is deliberately absent

Nothing in Seneri reads physical address zero. Leaving it unmapped turns a null
dereference into a page fault naming `CR2 = 0` instead of a silent read of the
real-mode interrupt vector table. `paging_verify` re-checks this on every call.

### Why the identity window is still 4 GiB

`SENERI_EARLY_PHYSICAL_LIMIT` is a promise that every physical address below
4 GiB is reachable at the same virtual address. It was `boot.S`'s promise; it is
now `paging.c`'s, deliberately unchanged. Three things depend on it:

- the frame allocator addresses exactly this range;
- `acpi_span_is_early_mapped` gates every firmware table read against it, and
  firmware tables sit wherever firmware put them;
- **`paging.c` reads each of its own tables through the table's own physical
  address.** A table frame outside the window would have no address the walk
  could reach it through, so `allocate_table` refuses one.

Narrowing it means re-pointing all three at once. That is a separate increment,
and it needs the temporary-mapping window a heap makes practical.

### Why the APIC windows are uncacheable

Intel SDM volume 3A section 11.4.1 requires the APIC register space to be mapped
strong-uncacheable. Under `boot.S` it was covered by write-back huge pages and
worked only because MTRRs happened to override it and because QEMU is forgiving.
Each discovered register page now sets `PCD` and `PWT`, which with the PAT bit
clear selects PAT entry 3.

Seneri checks that entry and entry 0 rather than assuming firmware defaults. It
then changes only unused entry 1 to write-combining, with exact readback and
cache flushes around the CR3 transition. The bootstrap hierarchy selects entry
0; live register mappings select entry 3; framebuffer pages select entry 1 with
PWT alone. A missing PAT, a reserved byte, or an inherited entry 0/3 with the
wrong meaning has its own refusal. `docs/WRITE_COMBINING.md` contains the full
entry-selection and MTRR argument.

**This is unverified on real hardware.** Getting device cacheability wrong
produces hangs that will not reproduce under QEMU. The addresses come from ACPI,
not from the `0xFEE00000` / `0xFEC00000` constants, but the UC and WC policies
have only been exercised under TCG and WHPX.

Every other MMIO region below 4 GiB is still covered by write-back 2 MiB pages,
exactly as `boot.S` left it. A general device-mapping policy needs a PAT- and
MTRR-aware mapper and is deferred; only the windows this kernel actually touches
are corrected here.

## The two bits that make permissions mean anything

- **`EFER.NXE`** (MSR `0xC0000080` bit 11). Until it is set, bit 63 of an entry
  is reserved and using such an entry faults. Availability is
  `CPUID.80000001H:EDX[20]`, checked and refused rather than assumed, the way
  `src/kernel/tsc.c` checks for an invariant TSC.
- **`CR0.WP`** (bit 16). With it clear, supervisor writes ignore the read-only
  bit entirely — and ring 0 is the only ring Seneri has, so every permission
  installed here would have been advisory. This bit is the difference between
  a W^X guarantee and a comment.

Both are set while the boot hierarchy still marks every page writable and
executable, so neither can revoke a permission from an instruction already in
flight. Both are **read back** rather than assumed: a write that did not take is
exactly the case where the kernel would go on to claim a guarantee it cannot
enforce.

## The CR3 switch

At the instant CR3 is loaded, the new tables must already map — at their current
addresses, with adequate permissions — the executing instruction, the rest of
kernel text, the stack, the frame allocator's bitmaps, the ACPI tables the kernel
still holds pointers into, and the local and I/O APIC windows. Anything missed
faults on the very next instruction with no diagnostic anyone can print.

The order is:

1. refuse the processor if NX or PAT is unavailable, `LA57` is on, `PAE` is off,
   any PAT byte is reserved, entry 0 is not WB, or entry 3 is not UC;
2. enable `EFER.NXE` and `CR0.WP`, reading each back;
3. derive a PAT target that changes unused entry 1 to WC and build the whole
   hierarchy against that target;
4. **walk it in software and compare every page against the intent** — every
   4 KiB page of every fine region against its permissions and selected memory
   type, every 2 MiB leaf against the bulk policy, and page zero against being
   absent;
5. run the W^X audit *before* the switch, because installing a hierarchy that
   violates the invariant and only then noticing means the machine already ran
   on it;
6. write and read back `IA32_PAT`, execute `WBINVD`, load CR3, and execute
   `WBINVD` again;
7. re-verify PAT and the hierarchy, and on failure flush and **restore the
   previous CR3 and PAT** before reporting a status.

Step 7 is the one recovery available: the boot hierarchy is still intact and
still maps everything, so reporting a status from the old address space beats
halting inside the new one.

### Construction is order-independent

The tables come from `frame_allocate`, so frames are allocated *while* the map is
being built, and they land inside the fine region the build is walking. This is
safe because the permission for an address is a pure function of that address:
anything inside a fine region that is not a named section is writable and
non-executable. A table frame handed out mid-build gets the same entry whether
its own address had already been visited or not.

### TLB invalidation

`invlpg` for a single page in `paging_map`, `paging_unmap` and `paging_protect`;
a CR3 load for the wholesale switch. Since no entry sets the global bit, the CR3
load flushes every cached translation with no residue. **This kernel is
single-core throughout, so there is no shootdown to perform** — no other
processor holds a stale copy. `invalidate` deliberately does nothing for a
hierarchy that is not installed, so the self-test cannot evict a live entry.

## Invariants

1. No page is writable and executable, on the machine, at any point after the
   switch. Checked by walking the installed tables, not by reading the ELF.
2. No page is reachable from user mode.
3. Effective permission is the conjunction down the whole path. Interior entries
   are deliberately the most permissive the architecture allows, so the leaf is
   the single place a permission is decided. The audit and `paging_translate`
   both compute the conjunction rather than reading the leaf.
4. Every table frame is below `SENERI_EARLY_PHYSICAL_LIMIT` and identity-mapped,
   so its physical address is the address the walk reads it through.
5. Page zero is absent.
6. A range operation validates the whole range before writing any of it, so a
   refusal leaves the hierarchy exactly as it was found. The only failure the
   apply pass can still hit is table-frame exhaustion, and that one is rolled
   back explicitly.
7. Translation decodes PAT, PCD, and PWT with the installed PAT value and reports
   the selected memory type. It never infers type from PCD alone.
8. A live `paging_protect` may change access rights but not memory type. Such a
   transition needs the full cache protocol and is refused by name.

## What is refused

`paging_map`, `paging_unmap`, `paging_protect` and `paging_translate` validate
and refuse with a named status rather than clamping, guessing or adapting. Every
output struct is zeroed on rejection, so a partial result cannot be mistaken for
a complete one.

- a length of zero, or one that is not a whole number of pages;
- an unaligned virtual or physical address — including 4 KiB alignment where a
  2 MiB leaf needs 2 MiB;
- a non-canonical address, **including a range that starts canonical and runs
  into the hole** between the two halves of the address space;
- an arithmetic overflow in either the virtual or physical range;
- a physical address wider than the 51:12 field an entry provides;
- a permission naming a right that does not exist;
- a request naming both uncacheable and write-combining memory;
- a protection request that would change a live page's memory type;
- **any combination that would be writable and executable**;
- mapping over an existing mapping;
- unmapping or protecting what is not mapped;
- a 4 KiB change inside a 2 MiB leaf — splitting one is deferred, so it is
  refused rather than silently applied to the whole 2 MiB;
- a second installation, and any operation at all before the first;
- a table frame the walk could not read back through its own address;
- exhaustion of the table supply.

## Executable proof

### `paging_self_test`

Pure arithmetic and a **private hierarchy the processor never runs on**, built
from a static arena so every rejection is reachable without a frame allocator.
It runs before boot touches any hardware.

- index extraction at all four levels, from one synthetic address with a
  different index at each, so a shift wrong by one level cannot still produce
  the expected answer; and a high-half address for the top index;
- canonical form on both sides of both boundaries, and the leaf size at each of
  the four levels;
- entry composition and decomposition round-tripping over every valid
  permission, with the frame recovered unchanged;
- synthetic PAT layouts and leaves covering every architectural type, reserved
  bytes, missing PAT support, and the distinct 4 KiB and large-leaf PAT bits;
- both sides of the physical-width bound — `PAGE_FRAME_MASK` is itself the
  highest page an entry can name and must be *accepted*; the first page above it
  must not be. **This found a real bug**: the original check compared a last byte
  against a mask whose low twelve bits are zero, and rejected the highest legal
  frame;
- a full map, translate, protect, unmap cycle, and every refusal above;
- a 2 MiB leaf refusing the 4 KiB pages beneath it, and refusing to be installed
  over a page directory entry that already points at a page table;
- **the audit seeing a violation.** A leaf is corrupted by hand — no path through
  the file can produce one — and the audit must count it. An audit that always
  reported zero would pass every boot without checking anything;
- **the conjunction, at each level.** An ancestor carrying no-execute must hide a
  leaf that does not, and a user bit on the leaf alone must not make a page
  user-reachable. An audit that read only the leaf would get both wrong;
- table-supply exhaustion, and an arena outside the identity window;
- synthetic kernel layouts: inverted, unaligned at either end, and an image
  larger than the linked bound;
- synthetic CPUID, CR4 and PAT values producing every processor refusal before
  any real PAT MSR access.

### Normal boot

`prove_paging_lifecycle` runs on every boot: a frame the allocator just handed
out is mapped outside the identity window, written and read back, narrowed to
read-only, read back again, then unmapped, proved absent, and released. Nothing
in it faults.

```text
Seneri OS: paging root 0x000000000017F000 table frames 9 regions 3 NX yes write protect yes
Seneri OS: paging leaves 3580 writable 3557 executable 16 both 0
Seneri OS: kernel page tables installed
Seneri OS: no writable executable mapping
Seneri OS: virtual memory established
```

`executable 16` is exactly the sixteen pages between `__text_start` and
`__text_end`. `both 0` is the invariant, counted off the live hierarchy.

The last line comes from a second `paging_verify` at the *end* of boot, after
every other subsystem has run. Everything between the switch and there executed
on this hierarchy, including three subsystems that write device memory through
it, so it is what proves none of them corrupted a table or turned a bit back
off.

### The `paging` scenario

The arithmetic above proves the tables say the right thing. This proves the
**processor** agrees, which is the part a file check can never do:

1. the state is active, NX and write protection are on, and `paging_verify`
   passes;
2. kernel text translates to executable-and-not-writable, kernel data to
   writable-and-not-executable, page zero to nothing;
3. every refusal, through the public interface, against the live tables;
4. a fresh frame is mapped writable at `0x200000000`, written, and read back;
5. `paging_protect` narrows it to read-only; the contents survive and reads
   still work;
6. a supervisor byte store at a known instruction address must fault, and the
   fault must match on vector, error code, `CR2` and `RIP`.

```text
  vector=14 name=page fault
  cr2=0x0000000200000000
  page-fault bits: P=1 W=1 U=0 RSVD=0 I=0
```

`P=1 W=1 U=0` is a protection violation on a present page. The existing
`page-fault` scenario's absent page is `P=0 W=0 U=0`, and the Makefile asserts
both, so the two scenarios cannot pass on each other's fault.

The probe is written in assembly, like the other fault probes, because a
compiler is free to move, duplicate, widen or delete an equivalent C store and
none of those would still fault at a predictable address. **It returns if the
store succeeds**, so a permission that quietly failed to take is a scenario
failure rather than a timeout. `docs/MONOTONIC_TIME.md` records why that matters:
a hang is not a diagnosis.

## Negative controls

Every one was applied to a clean tree, rebuilt, run, and reverted; the suite is
green before and after.

| Breakage | Observed failure |
| --- | --- |
| kernel text mapped writable as well as executable | `PANIC: a page may not be writable and executable` — refused at build time, before the tables existed |
| the same, plus the W^X refusal removed from `validate_permissions` | `PANIC: page table arithmetic self-test failed` — the self-test fires before boot reaches hardware |
| a writable leaf loses its no-execute bit *after* validation | `PANIC: installed page tables do not match their intent` — the pre-switch walk, before CR3 was ever loaded |
| `paging_protect` updates the entry but never invalidates the TLB | `ST FAIL paging: a read-only page accepted a supervisor write` |
| `paging_protect` reports success without touching the entry | `PANIC: page table arithmetic self-test failed` |
| the same, but only on the live hierarchy, so the self-test cannot see it | `PANIC: narrowing a mapping changed what it points at` — caught by the boot lifecycle proof |
| `EFER.NXE` never set | `PANIC: the no-execute bit did not take effect in EFER` |
| `CR0.WP` never set, and its readback guard removed too | `ST FAIL paging: a read-only page accepted a supervisor write` |
| the audit blinded — it never counts a writable-executable leaf | `PANIC: page table arithmetic self-test failed` |
| kernel text no longer pads to a page | `ld: kernel text does not end on a page`, `ld: a gap separates kernel text and rodata` |

Three of these are worth reading twice.

**The stale-TLB control is the one the fault test exists for.** The entry said
read-only and the processor wrote anyway, because it was still using a cached
translation. No amount of walking the tables in software would have found it —
only taking the fault does.

**The `CR0.WP` control proves that bit is load-bearing.** With it clear, the
tables were completely correct, the audit was completely satisfied, and a ring-0
write to a read-only page succeeded. That is the failure mode this whole
increment was written to end, reproduced by removing one instruction.

**Two controls failed at a layer earlier than aimed.** Marking text
writable-and-executable never reached the runtime walk, because
`validate_permissions` refuses the request; and a no-op `paging_protect` never
reached the scenario, because the pure self-test catches it first. Re-aiming each
one layer down produced the intended failure, and both results are recorded above
rather than dropped. `docs/PM_TIMER.md` records the same lesson from the PIT
retirement: a control that fails to fail is a result.

No control hung. The two that could have — the ones where a permission silently
fails to take — both fail with a status, because `paging_probe_write` returns
when its store succeeds.

## Reclaiming interior tables

`paging_unmap` used to clear a leaf and stop, leaving an emptied page table
mapped forever. That was tolerable while the only unmap in the kernel ran once
at boot. `docs/KERNEL_HEAP.md` changed that: the heap's growth rollback unmaps
pages in ordinary operation, so a repeated grow-and-fail cycle would have leaked
a table frame at a time, and the frame allocator would eventually have run dry
and blamed whoever asked last.

An unmap now gives back any interior table it empties, and then the table above
it if removing that one empties it in turn. **The root is never reclaimed** —
CR3 points at it, and a hierarchy with no root is not a hierarchy. The climb
stops at the first table that still holds an entry, because everything above it
is reachable through that entry.

Only a 4 KiB leaf can empty a page table. A 2 MiB leaf lives in a page directory
the identity map keeps populated, and this kernel never unmaps one.

`table_is_empty` scans a table's 512 entries and exits on the first present one,
so the cost is trivial except on the unmap that actually empties a table.

`paging_get_state().table_frames` is now taken from the hierarchy after every
mutation rather than fixed at install, and `paging_verify` checks the two agree
and that the count never reaches zero.

### Proof

`test_table_reclamation` drives a private hierarchy: one page needs three
interior tables; a second page in the same table needs none; emptying half the
table reclaims nothing and leaves the other page translating; emptying the last
entry collapses the whole path back to the root alone; the root's own entry is
cleared rather than left pointing at a freed page; and a 2 MiB leaf then
installs where the page table used to be, which is reclamation's whole point.
Two pages in different page tables under one directory prove the climb stops at
the directory while it still holds the other.

`prove_paging_lifecycle` compares the frame allocator's free count before and
after a complete map-write-protect-unmap-release cycle on every boot. The
`paging` scenario runs sixty-four such cycles and requires both the frame count
and the table count to be identical afterwards — one leaked table per cycle is
invisible in a single pass and fatal over a long-running kernel.

| Breakage | Observed failure |
| --- | --- |
| reclamation never runs (the original leak) | `PANIC: mapping a page and undoing it did not return every frame` |
| a table is freed while it still holds entries | `PANIC: page table arithmetic self-test failed` |
| the table is freed but the parent entry keeps pointing at it | `PANIC: page table arithmetic self-test failed` |
| the root is reclaimed too | `PANIC: page table arithmetic self-test failed` |

The first three could not be produced by simply deleting the code: `-Werror`
rejected the build for an unused function or an unused variable before any test
ran, which is a small proof of its own. Each was neutralised in a way the
compiler accepts instead.

## Deferred work

- **The kernel heap.** It needs this increment first; it is the increment after.
  `paging_map` at a fresh virtual address with frames from the allocator is
  exactly what it will be built on.
- **Splitting a huge leaf.** A 4 KiB change inside a 2 MiB mapping is refused,
  not performed. Splitting means allocating a page table, populating 512 entries
  from the huge entry, and swapping it in under a live translation.
- **Higher-half relocation.** Doing it in the same change as taking ownership of
  the tables would have made the CR3 switch far harder to review.
- **Narrowing the identity window**, and re-pointing the frame allocator, the
  ACPI readers and the table walk together.
- **A general device cache policy.** APIC, VGA, and PCI ECAM are UC and the
  framebuffer is WC; the rest of the MMIO hole is still write-back. A registry,
  MTRR dump, and alias audit remain missing.
- **Userspace, ring 3, per-process address spaces.** The user bit is refused, not
  supported.
- **Demand paging, swap, or any fault-driven mapping.** The page-fault handler
  stays fatal.
- **Anything per-processor.** The hierarchy is a single static and there is no
  shootdown, because there is one core.
- **Real hardware.** The memory-type layer was exercised under QEMU TCG and
  WHPX. KVM and bare metal remain open; device cacheability is the kind of change
  that can fail only on iron.
