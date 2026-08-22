<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Bounded Linux x86-64 syscall ABI subset

This milestone implements the Linux machine ABI required by one pinned
`busybox echo SAPOTE` invocation.  It is not a native Sapote ABI, POSIX layer,
or general file-descriptor interface.

The `syscall` number arrives in `RAX`; arguments arrive in `RDI`, `RSI`, `RDX`,
`R10`, `R8`, and `R9`; and the signed result leaves in `RAX`.  Refusals use
negative Linux errno values.  `RCX` and `R11` contain the architecturally
clobbered return address and flags.  The kernel accepts the request only from
the armed process generation, expected private CR3, CPL3 code selector, and
loaded executable range.

The normalized independent trace is committed as
`userspace/busybox/syscall-allowlist.txt`.  Excluding the host `execve` that
installs the file, the binary makes nine calls across these seven numbers:

| number | call | exact bounded semantics |
| ---: | --- | --- |
| 1 | `write` | descriptor 1, exactly seven readable bytes equal to `SAPOTE\n`, one proof-only sink |
| 9 | `mmap` | the observed anonymous fixed guard page and one observed anonymous RW page only |
| 11 | `munmap` | the one anonymous RW page returned by the preceding call |
| 12 | `brk` | query the fixed heap base, then grow it by exactly 8192 bytes |
| 158 | `arch_prctl` | `ARCH_SET_FS` to measured writable user address `0x400001008998` only |
| 218 | `set_tid_address` | measured writable user address `0x400001008b34`, returning one positive proof-process id |
| 231 | `exit_group` | status zero only; enters the no-return exiting transition |

Every pointer and extent is checked against the active private address space
with checked copy-in or copy-out.  Page permissions, overflow, cross-segment
ranges, partial copies, invalid flags, order changes, repeated operations, and
post-exit requests are refused with the relevant Linux errno.  No operation
can name stdin, another descriptor, a path, a writable file, a terminal, a
pipe, another process, or a user-selected mapping.

Every other syscall number returns `-ENOSYS` without changing process,
mapping, heap, sink, or syscall-CPU state.  The allowlist has a compile-time
ceiling of sixteen and a committed cardinality of seven.

The CPU foundation programs and reads back `IA32_EFER.SCE`, `IA32_STAR`,
`IA32_LSTAR`, and `IA32_FMASK`.  The reviewed assembly saves user RSP before
using it, disables the architecturally exposed interrupt window, switches
immediately to the validated kernel/TSS stack, aligns the System V C call, and
never executes C on user memory.  Return uses a checked `iretq`, after
validating user RIP, RSP, CS, SS, RFLAGS, generation, active CR3, provenance,
and the candidate/armed/entered/returned state.  `int 0x80`, the private
v0.7.0 `int 0x81` proof gate, direct handler calls, and `sysretq` cannot satisfy
this proof.
