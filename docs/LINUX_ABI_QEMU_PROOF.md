<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Static BusyBox Linux ABI QEMU proof

This document freezes the v0.8.0 installed-proof boundary and robustness
matrix before implementation.  One canonical root file is read through a new
read-only FAT16/NVMe fixture, decoded by Rust, installed into one private W^X
address space, and entered at the measured BusyBox entry.  The only invocation
is `busybox echo SAPOTE`.

The executable contract is fixed in
[the reproducible-build record](BUSYBOX_REPRODUCIBLE_BUILD.md), the initial
stack in [the stack contract](LINUX_INITIAL_STACK.md), and the syscall subset
in [the ABI contract](LINUX_SYSCALL_ABI.md).  The filesystem consumer accepts
one `BUSYBOX` root entry and a checked chain of exactly nine 4096-byte data
clusters.  It adds no partition parser, directory traversal, VFS, cache,
write, mount, or public block API.  CPU parsing begins only after every DMA and
filesystem buffer has returned to CPU ownership.

The complete fixture SHA-256 is
`41513E5D6F4C33F898F887D4F40F37149A29B1AE13B5E8A600495C18A38C7A6F`.
Its file payload SHA-256 is the executable digest
`B308F2CAD5B5CD0EEB92A622DEC8D71C1A08F628A22CDC5BCDE2B98B53220746`.

Candidate/validated executable, candidate/building/installed/running/exiting/
stopping/released process, candidate/armed/entered/returned/disarmed syscall
CPU, initial stack, bounded heap, stdout sink, request/result provenance, and
multi-cluster read session are separate typed lifecycles.  Invalid, repeated,
stale-generation, cross-process, reversed, and post-release transitions have
named refusals.  Kernel CR3 is restored before any private resource is
reclaimed; result publication follows a complete equal census only.

## Controlled robustness matrix

Each numbered row is one committed control.  Rows described as exhaustive
families run every byte truncation, relevant enum mutation, or allocation
boundary named by the row.  Every failure must restore kernel CR3 and the full
pre-proof resource census.

| # | controlled refusal or proof |
| ---: | --- |
| 1 | every truncation through the 64-byte ELF header |
| 2 | every truncation through each of the five 56-byte program headers |
| 3 | bad ELF magic, class, endianness, identification version, ABI, ABI version, or nonzero padding |
| 4 | non-`ET_EXEC`, including `ET_DYN`/PIE |
| 5 | wrong machine, ELF version, flags, or ELF header size |
| 6 | wrapped, misaligned, undersized, or out-of-file program-header table |
| 7 | zero or more than eight program headers, or wrong program-header entry size |
| 8 | zero or more than four `PT_LOAD` segments |
| 9 | unsupported non-load header, including interpreter, dynamic, TLS, note, or property headers |
| 10 | missing, duplicate, malformed, or executable `PT_GNU_STACK` |
| 11 | nonzero, wrapped, misaligned, executable, or otherwise malformed `PT_GNU_STACK` extent |
| 12 | `p_memsz < p_filesz` or an empty load segment |
| 13 | wrapped or out-of-file load extent |
| 14 | wrapped, zero, kernel, or noncanonical virtual extent |
| 15 | invalid alignment or offset/address incongruence |
| 16 | overflowing or inconsistent page rounding |
| 17 | overlapping byte or page-rounded load extents |
| 18 | writable/executable flags or a permission conjunction that creates W+X |
| 19 | executable stack or stale writable executable alias |
| 20 | noncanonical, unmapped, non-executable, BSS, or wrong entry point |
| 21 | nonzero BSS after installation |
| 22 | relocation, interpreter, dynamic dependency, GOT fixup, or section-table dependency |
| 23 | mismatch from the exact measured five-header type/order/extent conjunction |
| 24 | partial executable output and candidate/validated transition reversal |
| 25 | absent, duplicate, deleted, label, directory, LFN, or noncanonical `BUSYBOX` root entry |
| 26 | zero, oversized, destination-exceeding, or more-than-512-cluster file length |
| 27 | free, bad, reserved, below-two, or out-of-range first/next cluster |
| 28 | premature end-of-chain before all 33,584 bytes |
| 29 | late end-of-chain or an overlong chain after the file bytes |
| 30 | direct or indirect FAT chain cycle |
| 31 | repeated data cluster without a syntactic cycle and chain-visit overflow |
| 32 | wrapped cluster/LBA/byte arithmetic or out-of-volume read |
| 33 | wrong per-cluster byte count, short final copy, or nonzero unused destination tail |
| 34 | parse, inspect, copy, reuse, or release while controller/DMA owns a buffer |
| 35 | failure at each session setup, cluster-read, ownership-return, and release boundary |
| 36 | path, partition, second-file, directory, write, mount, cache, or public-block request |
| 37 | `argc` below, above, or otherwise different from three |
| 38 | missing, extra, reordered, or unterminated `argv` pointer vector |
| 39 | wrong, oversized, nonterminated, overlapping, or cross-page argument string |
| 40 | nonempty or unterminated `envp` |
| 41 | missing, duplicate, oversized, unknown, or malformed auxiliary vector |
| 42 | bad stack alignment or wrapped/noncanonical stack/string arithmetic |
| 43 | stack pointer into kernel, MMIO, DMA, page tables, guard, filesystem, or cross-segment memory |
| 44 | allocation failure at each stack frame/map/write/protect transition and partial-stack teardown |
| 45 | `IA32_EFER.SCE` absent, not writable, or changed after arming |
| 46 | wrong `IA32_STAR` kernel/user selector encoding or descriptor state |
| 47 | wrong or noncanonical `IA32_LSTAR`, including any direct C target |
| 48 | wrong `IA32_FMASK` interrupt/direction/trap masking contract |
| 49 | syscall entry from CPL0, wrong CS/SS, or the private `int 0x81` gate |
| 50 | user RSP not saved first, kernel/TSS stack invalid, or C execution attempted on the user stack |
| 51 | wrong process, generation, CR3, instruction range, provenance, or stale syscall state |
| 52 | wrong RAX/RDI/RSI/RDX/R10/R8/R9 decoding, RCX/R11 clobber, result, or errno encoding |
| 53 | invalid return RIP, RSP, CS, SS, RFLAGS, CR3, generation, provenance, or state |
| 54 | repeated entry/return, return after disarm, disarm while entered, or stale CPU-state reuse |
| 55 | every non-allowlisted sample and an over-16-entry proposed allowlist returns `-ENOSYS` without mutation |
| 56 | `write` with descriptor other than 1, wrong operation order, or repeated sink publication |
| 57 | `write` with count/content other than exactly seven bytes `SAPOTE\n`, including short sink capacity |
| 58 | `write` from wrapped, unreadable, kernel, MMIO, DMA, page-table, guard, cross-segment, or partially readable memory |
| 59 | `arch_prctl` command/address mismatch, repeated setup, or unwritable FS target |
| 60 | `set_tid_address` address/order mismatch, repeated setup, or unwritable target |
| 61 | `brk` order, base, growth, bound, alignment, wrap, shrink, or post-guard mismatch |
| 62 | `mmap` address/length/protection/flags/fd/offset/order mismatch or mapping collision |
| 63 | `munmap` address/length/order/ownership mismatch or duplicate unmap |
| 64 | nonzero, duplicate, premature, or post-exit `exit_group` and syscall after exit |
| 65 | injected failure immediately before and after each of the nine observed syscall entries |
| 66 | allocation failure at every ELF frame, hierarchy/table, stack, heap, and filesystem construction boundary |
| 67 | invalid, repeated, stale, cross-process, reversed, or post-release executable/process transition |
| 68 | invalid, repeated, stale, reversed, or post-disarm syscall CPU transition |
| 69 | invalid stdout-sink, syscall-request/result-provenance, or multi-cluster-session transition |
| 70 | teardown out of reverse order, reclaim before kernel CR3, leaked mapping/frame/table/state, or non-kernel CR3 census |
| 71 | missing/duplicate Boot Ledger prerequisite, stage/result cardinality error, or non-neutral ordinary outcome |
| 72 | alternate scenario/guest/host exit values, direct handler, non-CPL3 fetch, non-`syscall` instruction, or premature result publication |

The installed stable line is fixed to:

```text
ST LINUX ABI busybox echo bytes 7 syscalls 9 stdout valid exit 0 ring 3 address-space private teardown clean robustness 72
```

It contains no address, process generation, host path, timing, PCI topology,
filesystem identifier, or toolchain path.  Ordinary boots and other fixtures
publish one neutral absence outcome.  Installed verification requires exactly
one success or neutral outcome.
