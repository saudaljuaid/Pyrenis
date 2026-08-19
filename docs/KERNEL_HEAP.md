# The kernel heap

Every layer below this one deals in whole 4 KiB frames or in fixed static
storage. The deadline table is `TIMER_MAX_PENDING` entries, the ACPI topology is
`ACPI_MAX_LOCAL_APICS` processors, the interrupt override table is sixteen —
each one a compile-time bound chosen because there was nowhere else to put the
data. Every one of those docs says the same thing: *this is fixed because there
is no heap*.

This is the heap. It is the increment `docs/VIRTUAL_MEMORY.md` was written to
make possible, and it could not have come first: it needs a virtual window it
can grow into one page at a time, and until the kernel owned its page tables
there was no such thing.

## Shape

One bounded window, mapped on demand, with out-of-band metadata.

```text
0x00000003FFFFF000   guard page          never mapped
0x0000000400000000   heap window         16 MiB, committed page by page
0x0000000401000000   guard page          never mapped
```

The window is virtual address space, not memory. Nothing is mapped at
initialization. The first allocation commits the first page: a frame from
`frame_allocate`, mapped writable and non-executable with `paging_map`. The heap
grows a page at a time and never shrinks.

The heap is **not physically contiguous** and does not need to be. Each page is
mapped on its own, which is the whole reason a virtual window is worth having.

### The block table

`HEAP_MAX_BLOCKS` block descriptors, in a static array, outside the memory being
managed. This is deliberate. In-band boundary tags — a header immediately before
each allocation — are the classic design and are one buffer overrun away from a
corrupted allocator that hands the same bytes to two callers. Out-of-band
metadata cannot be reached by a write through a heap pointer at all.

The price is a fixed bound on how many blocks the heap can be divided into. That
is a Seneri policy bound, like every other one in this kernel, and running into
it is a reported status rather than a corruption.

### The invariant

**The blocks tile `[0, committed)` exactly: ascending, aligned, no gap, no
overlap, and no two free blocks adjacent.**

The last clause is the one that does real work. Everything else would still hold
in a heap that never coalesced — it would satisfy every count, pass every
lifecycle test, and fragment itself to death over time. Requiring that no two
free blocks sit side by side means a free that failed to merge is a detectable
error rather than a slow leak.

`heap_verify` checks it, and checks it against the page tables too: every
committed page must be a 4 KiB writable non-executable mapping, both guards must
still be absent, and the block table's own totals must match the counters the
heap reports. A description of memory that has drifted from the memory it
describes is exactly how an allocator starts handing out the same bytes twice.

## What is refused

- a size of zero, and a size larger than the window;
- a null output pointer, and any call at all before initialization;
- a second initialization;
- initialization before the page tables exist, or with interrupts enabled;
- a growth that would run past the window, refused **before** a page is mapped;
- a growth the frame allocator cannot back, which unwinds every page it mapped;
- a split that would need a block descriptor the table cannot supply;
- **a pointer the heap never returned** — interior, unaligned, below the window,
  above the window, or past what has been committed;
- freeing the same allocation twice.

Every output pointer is set to `NULL` before anything else happens, so a
rejection can never leave a caller holding a stale value.

### The one place two refusals are not interchangeable

Freeing a pointer twice reports `DOUBLE_FREE` if the block is still there, and
`BAD_POINTER` if coalescing has since merged it away.

Both refuse and neither corrupts anything, but the name depends on what the
*neighbours* did, which is not something a caller can predict. Boot pins both:
the first allocation's address survives, because the fully merged block still
starts at offset zero, so it stays a double free; the middle allocation's start
was swallowed by the merge and now names nothing, so it becomes a bad pointer.

Making these identical would mean keeping freed blocks addressable forever,
which is the opposite of coalescing. The behaviour is documented rather than
hidden.

## Growth is transactional

A growth that cannot be completed leaves nothing behind. Frames are allocated
and mapped one page at a time, and a failure at any page unwinds every page that
call had already mapped.

A frame is released **only after its mapping has actually gone**. If `paging_unmap`
were to refuse, the frame stays owned by the heap rather than being handed back
to the allocator while something can still reach it through a live translation.
Leaking a frame is recoverable; handing out a frame that is still mapped is not.

## Executable proof

### `heap_self_test`

Pure table arithmetic against a private table, run before boot touches any
hardware: fitting, splitting, coalescing in both directions, and the tiling
invariant driven through every way it can be violated — a gap, an overlap, a
wrong total, an unaligned size, an empty block, and two free blocks side by
side. Plus exact-offset lookup rejecting interior offsets, and a split refused
by a full table. Plus every public refusal reachable before initialization.

### Normal boot

`prove_heap_lifecycle` runs every boot: three allocations of different sizes,
each written with its own byte pattern, then **all three read back after all
three are written** — checking each as it is filled would not catch a later
allocation overlapping an earlier one, which is the failure worth hunting. Then
freed outermost-first so the last free has to merge in **both** directions, which
a heap that only ever merges forwards would fail.

```text
Seneri OS: heap window 0x0000000400000000 size 16777216 guards 0x00000003FFFFF000 0x0000000401000000
Seneri OS: heap committed 4096 bytes in 1 pages, live 3
Seneri OS: kernel heap online
Seneri OS: heap coalesced to one free block
Seneri OS: kernel heap established
```

The three allocations total exactly 4096 bytes after rounding, so one page backs
all of them — a small demonstration that the granularity arithmetic is exact.

The last line comes from a second `heap_verify` at the very end of boot, after
every other subsystem has run.

### The `heap` scenario

What the arithmetic cannot prove: that the memory is real and the guards are
enforced by the processor.

Two distinct blocks that do not overlap; every wrong pointer this scenario can
construct, refused by name; a freed block reused at the identical address, which
a heap that only ever grew would fail; the whole window committed and the next
byte refused; and then the guard.

The screen now owns a long-lived 3 MiB surface by the time scenarios run. The
`heap` scenario releases that client first so “the whole window” still means all
16 MiB; the scenario ends at the deliberate guard fault and never returns to a
caller that needs the screen.

```text
  vector=14 name=page fault
  cr2=0x0000000401000000
  page-fault bits: P=0 W=1 U=0 RSVD=0 I=0
```

A supervisor write to an absent page. That is a **third distinct error code**, and
the Makefile asserts all three, so no two scenarios can pass on each other's
fault:

| Scenario | Fault | Error code |
| --- | --- | --- |
| `page-fault` | read of an absent page | `P=0 W=0 U=0` |
| `paging` | write to a present read-only page | `P=1 W=1 U=0` |
| `heap` | write to an absent guard page | `P=0 W=1 U=0` |

### The scenario asks the machine how much memory it has

Requesting the whole window goes one of two ways, and both are checked. With
more free memory than the window, every page commits and the next byte is
refused at the window bound. With less, growth runs out of frames part way
through — the **only** path that exercises rollback — and the heap must be left
exactly as it was, with every page it had mapped given back.

The QEMU suite runs at 128 MiB and takes the first branch. The second was
verified by hand at `-m 17M`, which is where the frame allocator runs out
mid-growth:

```text
ST INFO heap: growth rolled back at the frame limit
ST PASS heap
```

## Negative controls

Each applied to a clean tree, rebuilt, run, and reverted. The suite is green
before and after.

| Breakage | Observed failure |
| --- | --- |
| a split never splits, handing out the whole free block | `PANIC: kernel heap block table self-test failed` |
| a free never coalesces | `PANIC: heap did not coalesce back to one free block` |
| a free accepts any pointer inside the window | `PANIC: heap accepted a pointer it had already merged away` |
| a failed growth does not roll back its partial commit | `ST FAIL heap: a failed heap growth did not roll back` **(at `-m 17M`)** |
| `mapped_pages` accounting stops being updated | `PANIC: kernel heap state does not match its memory` |
| a heap guard page is mapped after the initial check | `PANIC: a kernel heap guard page is mapped` |

Two of these deserve a note.

**The rollback control failed to fail on the first attempt.** At 128 MiB there is
far more free memory than the 16 MiB window, so `frame_allocate` never fails
mid-growth and the rollback path is never entered — the control was simply not
reached, and the suite stayed green with the rollback deleted. That is a result,
and the response was not to accept it: the scenario was rewritten to check
*whichever* outcome the machine produces, and the control was re-aimed at
`-m 17M`, where it now fails as intended. `docs/PM_TIMER.md` and
`docs/VIRTUAL_MEMORY.md` record the same lesson from earlier increments.

**The coalescing control initially reported the wrong thing.** It tripped a
pointer-identity check that happened to run first, which named a symptom rather
than the cause. The boot proof was reordered so structural checks run before
identity checks, and it now reports `heap did not coalesce back to one free
block`. The verify call sites were also changed to report
`heap_status_string(status)` instead of a fixed message, which is why the guard
control names the guard.

## Deferred work

- **The heap never shrinks.** Committed pages stay mapped and their frames stay
  owned, even when the whole window is free. Returning them means deciding when
  a page is worth unmapping, which is a policy question with no callers to
  inform it yet.
- ~~Page tables are never reclaimed.~~ **Fixed.** `paging_unmap` now gives back
  any interior table it empties, so heap growth rollback no longer leaks a table
  frame per failed attempt; see `docs/VIRTUAL_MEMORY.md`.
- **First fit, linear scan.** Fine for a boot-time handful of blocks and
  deliberately predictable, so the self-test can pin exact offsets. It becomes a
  size-bucketed free list when something allocates in a loop.
- **`timer.c` is the first consumer**, and the pattern it sets is the one the
  others should follow: one allocation at start, released at stop, never per
  operation, because the subsystem is reachable from interrupt context. The
  fixed tables in `acpi_madt.c` and `interrupts.c` are still fixed and are the
  next candidates — though both are populated once at boot and never resized, so
  the bound they remove is smaller than the timer's.
- **Not reentrant, and not interrupt-safe.** Growth maps pages in a transaction
  that must not be interrupted, and nothing in Seneri allocates from an
  interrupt handler. A heap that could would need a lock, and a lock needs
  something to contend with it.
- **No `realloc`, no calloc-style zeroing.** A returned block holds whatever the
  previous owner left. Zeroing is the caller's business until something needs
  otherwise.
- **Verified under QEMU only**, at 128 MiB and by hand at 17 MiB.
