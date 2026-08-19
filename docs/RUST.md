# Rust in Seneri

Seneri is a C kernel with one Rust component. This document argues where the
line goes, and why it is not where people usually put it.

## The rule

> **Rust is for parsing input this kernel did not produce. C is for talking to
> hardware.**

A bounds check the compiler inserts and cannot be talked out of is worth most on
a byte stream from outside, because that is where a missing one stops being a
bug and becomes somebody else's primitive. Image files, filesystem metadata,
USB descriptors, network frames and 802.11 management frames are all that. They
are also, not coincidentally, where the interesting security history of every
operating system lives.

## Where Rust would buy nothing, and why

The intuition that Rust belongs in "the memory code" is worth taking seriously
and then rejecting, because it inverts the actual benefit.

`paging.c` writes page table entries. `pci.c` writes an address to a port and
reads a register. `thread.S` changes the stack pointer out from under a running
function. **Every one of those operations is `unsafe` in Rust** — they are
exactly the operations the safe subset exists to forbid. Rewriting them in Rust
would produce a file where every meaningful line sits inside an `unsafe` block,
the borrow checker supervising the bookkeeping around hardware accesses it
cannot reason about at all. That is not a safety improvement. It is a second
toolchain in the boot path in exchange for a stricter type system on the parts
that were never the risk.

So the split is not "dangerous code in Rust". It is:

| | Language | Why |
| --- | --- | --- |
| Page tables, port I/O, MMIO, context switch | C and assembly | Inherently `unsafe`; Rust adds a toolchain, not a guarantee |
| Fixed-shape firmware tables (ACPI) | C | Bounded, already proved, and rewriting working proved code is churn |
| Decoders of external byte streams | **Rust** | Every length is attacker-controlled; the checks should not be optional |
| Future: filesystem metadata, USB descriptors, network and 802.11 frames | **Rust** | Same argument, much larger surface |

That last row is the point. The logo decoder is small; it is here to establish
the toolchain, the build integration and the discipline **before** the layers
that will really need it exist.

## What is actually in Rust today

`src/rust/logo.rs` — the boot logo decoder. It reads a run-length encoded image
whose header, run lengths and pixel count are, in principle, attacker
controlled, and it refuses eight distinct malformations by name.

`src/rust/abi.rs` — the boundary. Every entry point is `extern "C"`, and each
contains **exactly one `unsafe` block**, at the single place a C pointer becomes
a Rust slice, with the caller's obligation written above it. Past that line
everything is safe Rust and every index is checked.

`src/rust/lib.rs` — the crate root and the panic handler.

The image itself is not committed. `tools/make-logo-asset.py` converts
`assets/seneri-logo.png` at build time: the kernel cannot decode the PNG,
because it inflates to 16 MB — larger than the entire kernel heap — and a
DEFLATE decoder is a great deal of code to run before anything else works.

## How it is built

One `rustc` invocation, no Cargo, no `build.rs`. The crate is a `staticlib`
linked into the same image as the C objects.

The target is `x86_64-unknown-none`, chosen because its constraints match the C
flags exactly — no MMX, no SSE, soft float, no red zone — which is what lets the
two languages share a stack and an interrupt frame without a shim. Warnings are
errors on both sides: `-Werror` for C, `-D warnings` plus
`deny(unsafe_op_in_unsafe_fn)` and `deny(missing_docs)` for Rust.

## What linking a second language actually cost

Two things, and both were found by breaking something rather than by reading.

**Rust emits sections the linker script had never seen.** `.data.rel.ro`,
`.llvmbc`, `.llvmcmd` and `.note.gnu.property` arrive with the static library.
`ld` places sections a script does not mention wherever it likes, and the first
Rust build to change size opened a gap between `.data` and `.bss` — which
`linker.ld` already asserted against, so it failed loudly, but only by luck of
which assertion happened to exist.

The fix is not to name those four sections. It is `--orphan-handling=error`:
**a section neither placed nor discarded is now a link error.** The script names
every section it wants, discards the build metadata, keeps the debug information
non-loaded, and nothing can be placed behind its back again.

**A freestanding kernel should have no global offset table**, and now it is
asserted. `__got_end == __got_start` or the link fails. This was added while
chasing the section problem and immediately earned its place — see below.

## Executable proof

`seneri_logo_self_test` runs on every boot beside the C self-tests, driving
malformed blobs built in Rust: a bad magic, a short header, a zero width, a zero
height, a width past the bound, a zero-length run, a run larger than the image,
a run that overruns only because of what preceded it, a truncated blob, a buffer
one pixel short, and trailing bytes after the last pixel. It also checks that
alpha actually blends — a fully transparent run must leave exactly the
background, and the same pixel over black and over white must differ.

Normal boot then decodes the real image, blits it centred, and **reads all
65,536 pixels back off the screen** to compare against the decode.

### Negative controls

| Breakage | Observed failure |
| --- | --- |
| the run-length bound is dropped | normal boot fails |
| the header magic is not checked | normal boot fails |
| a normal-boot contract line is renamed | `normal scenario did not complete the integrated production path` |
| **an unchecked index replaces the bounds check** | **the link fails: `the kernel gained a global offset table`** |

The last one is the interesting one, and it says something about this
integration that was not designed in.

Replacing `blob.get(range).ok_or(Status::Truncated)?` with `&blob[range]` does
not produce a kernel that panics at runtime. It produces a kernel that **cannot
be linked**. The unchecked index introduces a reachable panic, the panic path
drags in `core`'s formatting and location machinery, that machinery needs
relocations, and the relocations need a global offset table this kernel asserts
it does not have.

The corollary is visible in the finished image: `nm` on `seneri.elf` finds no
Rust panic path at all. Not a dormant one — none. Every fallible operation in
the decoder returns a status, the compiler proved no panic is reachable, and
optimised the handler away.

So the panic handler in `lib.rs` is, today, unreachable by construction. It is
kept because that is a property of the current code rather than a guarantee of
the build, and a future decoder that needs a genuine panic path should have
somewhere to land — but the honest statement is that it has never run, and the
build would refuse the change that first made it reachable.

## Deferred work

- **One component is not a policy.** The rule above is worth what the next three
  decoders do with it. USB descriptors and filesystem metadata are the tests.
- **No `alloc`.** The crate has no allocator, so no `Vec` and no `String`.
  Wiring `alloc` to the kernel heap is a small change and should wait until
  something needs it rather than being added because it is possible.
- **The GOT assertion may be too strict.** It is right for today's code and it
  caught a real problem. If legitimate Rust ever needs relocations, extending
  the linker script is a deliberate change with its own review, not a surprise.
- **No Rust in an interrupt handler**, and no Rust that runs before paging.
- **The panic handler has never executed.** See above.
- **Verified under QEMU only**, with one Rust toolchain version.
