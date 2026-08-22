<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Bounded Linux x86-64 initial process stack

The BusyBox proof enters `_start` with one prebuilt, private user stack.  The
stack pointer is canonical, mapped user read/write and execute-disabled, and
16-byte aligned.  Every addition, multiplication, subtraction, alignment, and
pointer range is checked before bytes are installed.

In increasing addresses from the initial `%rsp`, the word layout is:

```text
3
pointer to "busybox"
pointer to "echo"
pointer to "SAPOTE"
0                         argv terminator
0                         empty envp terminator
AT_PAGESZ, 4096
AT_NULL, 0
```

The strings are NUL-terminated private copies in the same stack mapping.  No
word points into kernel, MMIO, DMA, page-table, filesystem, or NVMe storage.
The accepted argument count is exactly three; the total argument bytes are
exactly 20 including terminators.  No environment entry is accepted.

`AT_PAGESZ` is the only non-null auxiliary entry.  A clean host launcher maps
the measured segments, creates exactly this stack, and transfers directly to
the BusyBox entry.  It produces `SAPOTE\n` and the same nine target syscall
entries as the normal Linux launch.  musl 1.2.6 consumes the page size; the
selected static image does not require `AT_PHDR`, credentials, entropy,
platform strings, vDSO, executable name, or a dynamic-loader entry.  Sapote
therefore does not invent any of them, and in particular publishes no
deterministic `AT_RANDOM` bytes.

Candidate, building, installed, running, exiting, stopping, and released
process states own the stack explicitly.  Stack construction failure releases
all partial frames and mappings.  Teardown switches to installed kernel CR3
first, removes the stack in reverse ownership order, and requires the complete
pre-proof resource census before the installed result can be published.
