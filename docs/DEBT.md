# What Seneri owes

Every other document here ends with a *Deferred work* list, which is honest but
local: each one knows what its own layer is missing and nothing knows what the
whole thing is carrying. This is that register.

It is measured rather than remembered. Every number below came from the tree at
the commit that added this file, and the commands are given so the next person
can re-measure rather than trust it.

## Verdict

**The engineering discipline held; the structure did not keep up.** Nothing
here is a correctness hole in a shipped layer — the twenty-three scenarios pass,
`nm -u` is empty, the image has no global offset table, and W^X is enforced by
hardware rather than by a linker script. What slipped is *shape*: one file
absorbing every new proof, one function signature growing a parameter per
increment, and a test harness whose contract is now ninety-one shell assertions.

The debt is real but it is the cheap kind. It is written down before it is
expensive rather than after.

## Paid on the way in

Found while writing this register, fixed in the same commit.

| | |
| --- | --- |
| **`framebuffer_verify` ran at the wrong moment.** | It was called inside the framebuffer's own proof, and the logo was then blitted through that same mapping — roughly 800,000 further stores that nothing re-checked. Verifying before the last thing that writes through a mapping is verifying the wrong moment, which is precisely what `paging_verify` and `heap_verify` sit at the end of boot to avoid. Moved. A control that disturbs the mapping after the logo is drawn now panics with `framebuffer does not match the address space`; under the old ordering it passed. |
| **`docs/MONOTONIC_TIME.md` was factually wrong.** | It listed as deferred: *"The table is a linear scan over fixed storage, because there is no heap."* `timer.c` has taken that table from the heap since the commit before this session. The code changed and the document did not. Corrected, and split — the storage is fixed no longer, the scan is still linear. |
| **`docs/PIT_RETIREMENT.md` carried a claim already corrected elsewhere.** | That level-triggered routing "gates PCI device interrupts". `README.md` was updated when PCI enumeration showed every PCIe endpoint offers MSI-X; this document was not. |

The second of those is the one worth dwelling on. These documents are the
project's contract — the reason `make verify` means something is that someone
wrote down what it was supposed to mean. A deferred-work entry that has silently
become false is worse than no entry, because it is read as current.

## Outstanding

Ordered by what it costs to leave alone, not by size.

### 1. Integration debt — measured, and smaller than it looked

**The exit-value collision is resolved.** PR #31
(`ioapic: route level-triggered sources with directed EOI`) was opened against
`main` first, so it keeps `0x22`; the scenarios added since were renumbered up
to `0x23`–`0x27` and `0x22` is reserved by name in both `test.c` and the
`Makefile`. The two changes can now land in either order without one silently
passing as the other.

**The pile of unmerged branches is not a pile.** `git branch -r` shows eighteen
branches and `git branch -r --no-merged origin/main` shows all eighteen, which
reads as eighteen abandoned lines of work. It is not. Every pull request in this
repository was squash-merged, and a squash merge leaves the original branch tip
unreachable from `main` even though every line of it landed. The reachability
question is the wrong one; the patch question is the right one:

    git cherry origin/main <branch>     # '-' means already upstream, '+' means not

Run across all eighteen, sixteen report every commit already upstream. Of the
remaining two, `seneri-os-tsc-primitive-2gc90u` reports one commit not upstream
purely because the patch context shifted — the symbol it adds, `cpu_read_tsc`,
is present in `main` verbatim in `src/arch/x86_64/cpu.S` and declared in
`include/seneri/cpu.h`. Checked by hand rather than trusted.

**So exactly two branches carry work that is not in `main`:**

| Branch | Pull request | State |
| --- | --- | --- |
| `seneri-os-ioapic-level-dapmyc` | #31 | open, one commit |
| `seneri-os-pci-enumeration` | #32 | open, eight commits |

The other sixteen are the remains of merged or superseded pull requests. Two of
them — `claude/seneri-acpi-pm-timer-k69tgx` (#23) and `seneri-os-c-update-2gc90u`
(#14) — belong to pull requests closed unmerged as duplicates, and their content
reached `main` through #24 and #17 respectively. `git cherry` agrees.

They cost nothing to keep except the false impression they create, which is the
whole reason this entry exists. Deleting them is one command per branch and the
commits stay recoverable from each pull request's page:

    git push origin --delete <branch>

That is a decision about someone else's repository, so it is written down here
rather than done.

**This census was broken before it was believed.** Patch identity is not proof,
so the claim was re-checked a second way and then the checker itself was
checked:

| Control | Result |
| --- | --- |
| For all sixteen branches, does `main` contain every function symbol the branch adds to `src/` or `include/`? | Yes, every one. The claim survives a symbol-level check, not just a patch-ID one. |
| Is #31's work genuinely absent from `main`, so the census is not vacuously true? | Absent. `acknowledgement_targets_are_resolved`, `directed_eoi_is_gated_on_version` and `entries_round_trip` exist on that branch and nowhere in `main`. |
| Can the checker fail at all? | Yes. Fed `seneri_this_symbol_does_not_exist` it reports missing, so a clean run means something. |

**What remains between #31 and #32 is textual.** Measured with
`git merge-tree --write-tree --name-only`, the two branches touch the same five
files — `Makefile`, `README.md`, `docs/PIT_RETIREMENT.md`, `src/kernel/kernel.c`
and `src/kernel/test.c` — in the same regions: the scenario list, the boot
sequence, and the deferred-work paragraph both changes rewrite.
`include/seneri/test.h` merges cleanly.

None of those are semantic disagreements any more, but they are hand
resolutions, and there are more of them every increment that lands on either
side. **Both branches merge cleanly with `main` today.** The order that costs
least is #31 first, then #32.

### 2. `kernel.c` has become the place proofs go to live

    $ wc -l src/kernel/kernel.c
    2211          # 1256 at the start of this session; 1990 when this file was written

Every increment adds a `prove_X()` and every `prove_X()` lands here, so the file
grows once per subsystem regardless of how well factored that subsystem is. It
is now the third largest file in the kernel and the only one with no single
responsibility.

**This entry has already been proved right once.** It was written at 1,990
lines, warning that each increment landing first makes the move bigger. The very
next increment — preemption — added 221 more. That is the cost of deferring it,
measured rather than predicted.

The fix is not clever: boot proofs belong beside the subsystems they prove, or
in a `src/kernel/boot_proofs.c` that `kernel_main` calls in order. Doing it is
mechanical. Doing it *before* the next few increments is the point, because each
one that lands first makes the move bigger.

### 3. Signatures growing a parameter per increment

`paging_initialize` took one argument three commits ago and takes three now:

    paging_initialize(topology)
    paging_initialize(topology, mcfg)
    paging_initialize(topology, mcfg, framebuffer)

Every addition is a *device window* — a physical range that must be carved out
of the identity map as uncacheable. That is one concept wearing three parameters,
and the next device window makes it four. It should be a
`struct paging_device_windows`, and the change is small today.

`kernel_test_run` has the same shape for the same reason, at four parameters.

### 4. The harness contract is ninety-one shell assertions

    $ grep -c 'grep -F\|grep -E' Makefile
    91

Most of them are one `||`-joined chain checking the normal boot transcript. It
works — renaming any contract line has been shown to turn the suite red, every
time, for every increment. But it is a continuation-backslash away from silently
dropping a check, and this project has already been bitten once by a test that
passed without running.

A file of expected lines and a loop over it would be shorter, readable, and
harder to break by accident.

### 5. Public surface that only tests call

Twenty-five exported functions have no caller outside their own file and the
test suite:

    apic_spurious_count, apic_timer_expiry_count, apic_timer_is_calibrated,
    apic_timer_is_running, cpu_tables_active, cpu_address_on_ist,
    interrupts_validate, interrupts_ready, frame_reserve_range, pci_shutdown,
    pci_is_initialized, pci_config_read_port, pci_config_read_ecam,
    pci_function_count, pic_is_retired, pic_is_initialized, pit_active_route,
    pit_frequency, pit_is_running, pm_timer_nanoseconds_to_ticks,
    timer_stop, timer_is_started, timer_arm, ...

Much of it is deliberate observability, which is fine. But **`timer_arm` was the
deadline layer's primary entry point with nothing in the kernel calling it** —
the only production user of that subsystem was `timer_sleep_ns`. That was worth
knowing before building preemption on top of it, because the API was less
exercised than its test count suggested.

**Resolved, and the warning was earned.** `thread.c` now arms the quantum
through `timer_arm`, so the path has a production caller. The first run of that
caller hung the machine — not because `timer_arm` was wrong, but because the
threads it was arming for started with interrupts disabled. An entry point that
only tests had used met its first real caller and the first real caller had a
bug. That is the shape this entry predicted.

### 6. No host-side Rust test target

The Rust decoder's self-test runs in the kernel on every boot, so it is covered.
But it was *developed* against a host harness that runs in two seconds instead of
a full QEMU boot, and that harness is not in the repository. The next person
debugging a Rust component will rebuild it from scratch.

A `make rust-check` that compiles `src/rust/logo.rs` for the host and runs its
tests is a few lines and pays for itself the first time it is used.

### 7. Single-core state, spread wider every increment

    $ mutable statics per file
    paging.c 40, acpi_madt.c 23, pci.c 21, cpu.c 18, timer.c 16, thread.c 16, ...

Every subsystem holds its state in file-scope statics. This is documented as
deferred in every relevant document and it is the right call for a single-core
kernel — but the surface is now twelve files wide, and it grows with each one.
Nothing needs doing yet. What matters is that the day a second processor appears,
this is not a surprise, and the number above is what it will cost.

## Not debt

Measured, and healthy:

- **Twenty-three QEMU scenarios in 27.9 seconds.** A full boot is under a second
  even with an 786,432-pixel readback in it.
- **726 KB kernel image**, of which 66 KB is the logo.
- **No `TODO`, `FIXME`, `XXX` or `HACK` anywhere** in `src/`, `include/`, `docs/`
  or the `Makefile`.
- **No undefined symbols, no global offset table, no RWX segment**, all asserted
  at link or build time rather than checked by hand.
- **Every subsystem has a document, a self-test, and a negative-control table.**
  That has not slipped once, across five increments and two languages.

## How to re-measure this

    wc -l src/kernel/*.c | sort -rn | head
    grep -c 'grep -F\|grep -E' Makefile
    grep -rn 'TODO\|FIXME\|XXX\|HACK' src/ include/ docs/ Makefile
    nm -u build/seneri.elf
    git log --oneline origin/main..HEAD | wc -l

And, for §1 — which branch still holds work that `main` does not:

    for b in $(git branch -r --no-merged origin/main | grep -v HEAD); do
        printf '%-48s %s\n' "$b" "$(git cherry origin/main "$b" | grep -c '^+') unlanded"
    done

Read that output with the squash-merge caveat in mind: a `+` means the patch
identity differs, which is *evidence* of unlanded work and not proof of it.
Shifted context produces a `+` for a change that landed. Confirm by looking for
the symbol the commit adds before concluding a branch matters.

This file is worth exactly as much as the last time somebody ran those.
