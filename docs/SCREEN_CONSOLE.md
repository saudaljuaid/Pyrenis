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
- **The picture is buffered, but its proof is not.** Glyphs are drawn into one
  heap-backed surface in ordinary write-back memory. Damage is one bounding
  rectangle and only that rectangle is presented to the framebuffer.

Buffering changes where drawing happens, not where correctness is checked.
`screen_verify_cell` still reads the framebuffer after present and compares the
glass with the font. Pointing it at the back buffer would be the most dangerous
possible change here: the writer and verifier could share the same addressing
bug and agree perfectly while the visible screen was wrong.

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

## Staging and reentrancy

The font's 32-byte row buffer and the at-most 8x32 pixel tile are local to each
draw, never shared scratch storage. A thread switched out while building a glyph
therefore cannot hand another caller half of its tile. The screen cursor, damage
rectangle and surface are shared state and remain unsynchronised; Seneri has one
CPU and no interrupt handler writes text, so no two callers currently execute
this path at once. A lock belongs with the first real concurrent writer.

## Deliberate breakage

Every claim above was broken on purpose and the result recorded. Controls that
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
| Point `screen_verify_cell` at the back buffer instead of the framebuffer. | **Passed, as it must.** The check then proves only that the buffer agrees with itself; `docs/SURFACE.md` records why this is the control that must never become production code. |

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
allocates the full-screen back buffer, draws a known string, presents it, reads
the framebuffer back cell by cell, checks that an uncovered byte substitutes,
that the last column wraps, and that the last row scrolls.

The `screen` scenario in `src/kernel/test.c` does what boot cannot. Boot needs
the console it is printing through, so it can never leave it broken to see what
happens; nothing after the scenario needs a screen. It draws **all ninety-five
glyphs and reads every one back**, checks that a second `screen_initialize` is
refused, that a cell outside the grid is refused by the console rather than by
the framebuffer's bounds check, and that a normal console newline scrolls the
cached rows and presents the moved picture.

    make qemu-test-screen        # exit value 0x28

## Deferred work

- **One foreground colour.** There is no attribute per cell, so nothing can be
  highlighted — a panic looks like every other line.
- **No cursor is drawn.** The cursor position exists in `struct screen_state` and
  nothing renders it. The back buffer makes an untorn cursor possible, but
  cursor shape, blinking and restoration are a later increment.
- **No synchronization.** The one-CPU kernel has no concurrent screen writer.
  The first one needs to serialize the cursor and damage state rather than only
  protecting the pixel copy.
- **ASCII only**, 0x20 to 0x7E. The format indexes by subtraction from a first
  code point, so a second range would need a second table rather than a wider
  one.
