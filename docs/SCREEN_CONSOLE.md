# Text on the screen

Until this layer existed Seneri could draw pixels and could write words, but
never both. The console spoke to a serial port and a VGA text buffer; the
framebuffer knew about pixels and nothing about characters. This is what joins
them, and it is the first thing in this project a person standing in front of
the machine can read.

## What it claims

- **A character is a bitmap, and drawing one is copying its rows.** The font is
  8 pixels wide and 16 tall, so one glyph is sixteen bytes and one row is one
  byte with the leftmost pixel in the most significant bit.
- **The grid is derived by division, and a partial cell is not a cell.**
  1024x768 is 128 columns by 48 rows. 1023 pixels across an 8-pixel cell is 127
  whole cells and seven pixels of nothing.
- **Every pixel of a cell is written, lit or not.** A character replaces what was
  under it rather than being drawn over it, so no glyph can leave a fragment of
  the one before it.
- **An unknown byte is drawn, not refused.** A console exists to carry the
  message explaining what went wrong; one that stops on an unexpected character
  will eventually swallow exactly the message somebody needed. Uncovered bytes
  become `?`, which the font is checked to cover at initialization so the
  substitution cannot recurse.
- **Nothing is buffered.** There is no shadow copy of the screen and no dirty
  tracking. A character goes straight into device memory when it arrives.

That last one is a trade and it is worth stating plainly. It makes a scroll
expensive — the whole framebuffer has to be read back through an uncacheable
mapping — and it buys the property the rest of this document depends on: what is
on the glass is the only state there is, so `screen_verify_cell` checks the
console by reading the screen rather than by consulting a copy that could agree
with the bug.

## Where the font comes from

`tools/font8x16.txt` is ASCII art, one `#` per lit pixel, and it is committed.
`tools/make-font-asset.py` packs it into a 1,528-byte blob at build time using
only the Python standard library, so a clone builds the kernel with nothing but
Python — no font library, no OTF, no network.

The glyph bitmaps derive from **GNU Unifont**, which is free software under the
GNU General Public License version 2 or, at the user's option, any later
version. GPL-2-or-later permits use under GPL-3.0-only, which is this project's
licence, so no exception is being relied on. `tools/font8x16.txt` carries the
copyright notice. Unifont is a 16-pixel bitmap font by design, so these are its
glyphs exactly rather than an outline someone rasterised and rounded; every
printable ASCII character was measured to fit inside 7x15 of the 8x16 cell.

The reader is `src/rust/font.rs`. Every field in that header is a length or an
index that becomes an offset into the blob, which is the rule `docs/RUST.md`
states. The difference from the logo is frequency: a logo is decoded once, a
glyph is looked up once per character printed, so this is the first Rust in
Seneri on a hot path.

## Where it sits in boot

As early as the address space allows. The framebuffer is a device window that
`install_page_tables` carves out, so nothing here can run before that; the
console comes up immediately after the heap, and everything from line 54 of the
boot transcript onward appears on the screen as well as on the serial port.

The logo is drawn first and the console replaces it. That ordering is the whole
argument for putting this early rather than at the end of boot: a splash nobody
can read is worth less than the log, and a log that only starts once boot has
finished has missed the part somebody watching a machine that will not start
actually needs.

Routing is additive. `console_putc` still writes to the serial port and the VGA
text buffer exactly as it did, because the test harness reads the serial log and
a change that quietly moved the transcript would break every scenario at once.

## Reentrancy

The buffer a glyph is copied into is a local in each function that needs one,
never a file-scope static. Thirty-two bytes on a 16 KiB stack is nothing, and a
shared buffer would make drawing non-reentrant — this console is live while
preemption is running, so a thread switched out mid-glyph would hand the next
caller half of somebody else's character.

## Deliberate breakage

Every claim above was broken on purpose and the result recorded. The three that
panic do so after a named message and then halt, so the harness catches them on
its fifteen-second timeout rather than on an exit value; that is how every panic
in this kernel behaves and it is not specific to this layer.

| Control | Result |
| --- | --- |
| Draw bit 0 as the leftmost pixel instead of bit 7, mirroring every glyph. | `PANIC: screen console does not match the font it drew from`, before boot reaches the scenarios. |
| Swap foreground and background, so every glyph is drawn inverted. | Same panic. Readback compares both lit and unlit pixels, so an inversion is not a near miss. |
| Add one to the glyph index in `src/rust/font.rs`, so every character draws its neighbour. | Same panic. |
| **Reverse the scroll copy so it reads rows it has already overwritten.** | **Passed.** See below. |
| The same reversal, after the gap it exposed was closed. | `ST FAIL screen: a scroll did not move the rows it copied`. |
| Feed the font reader a table one byte longer than its header describes. | `font table has bytes after its last glyph`, from `font_self_test`, before any pixel is drawn. |

The fourth row is the one worth reading. **The reversed scroll passed every check
this layer had.** Scrolling by more than the screen height is a fill and
scrolling by zero is a no-op, so both take early exits and neither reaches the
copy loop — and the boot proof cleared the screen before verifying, which wiped
the corruption it should have caught. The scenario now draws two lines, scrolls
by exactly one cell, and requires the second line to have become the first.

A control that fails to fail is the most useful result this practice produces,
and it is the only reason that gap is closed rather than shipped.

## Proved where

`prove_screen_console` in `src/kernel/boot_proofs.c` runs on every boot: it
brings the console up, draws a known string, reads it back cell by cell, checks
that an uncovered byte substitutes, that the last column wraps, and that the
last row scrolls.

The `screen` scenario in `src/kernel/test.c` does what boot cannot. Boot needs
the console it is printing through, so it can never leave it broken to see what
happens; nothing after the scenario needs a screen. It draws **all ninety-five
glyphs and reads every one back**, checks that a second `screen_initialize` is
refused, that a cell outside the grid is refused by the console rather than by
the framebuffer's bounds check, and that a scroll moves the rows it copied.

    make qemu-test-screen        # exit value 0x28

## Deferred work

- **A scroll is a full framebuffer read-back.** Under QEMU that is free, because
  QEMU models no cache; on real hardware every source pixel is an uncached bus
  cycle and a scroll will be visibly slow. A back buffer in ordinary write-back
  memory would fix it, and would cost the property that the screen is the only
  state.
- **One foreground colour.** There is no attribute per cell, so nothing can be
  highlighted — a panic looks like every other line.
- **No cursor is drawn.** The cursor position exists in `struct screen_state` and
  nothing renders it, because nothing yet reads input to put there.
- **No input at all.** This is half of a terminal. The other half is a keyboard.
- **ASCII only**, 0x20 to 0x7E. The format indexes by subtraction from a first
  code point, so a second range would need a second table rather than a wider
  one.
