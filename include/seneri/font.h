/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SENERI_FONT_H
#define SENERI_FONT_H

#include <stddef.h>
#include <stdint.h>

/*
 * The console font, read by src/rust/font.rs.
 *
 * Every field in the glyph table's header is a length or an index that becomes
 * an offset into the blob, which is the same argument that put the logo decoder
 * in Rust and is stated once in docs/RUST.md. The difference is frequency: a
 * logo is decoded once at boot, a glyph is looked up once per character
 * printed, so this is the first Rust in Seneri on a hot path.
 *
 * The glyph bitmaps derive from GNU Unifont under GPL-2-or-later;
 * tools/font8x16.txt carries the attribution and is the committed source.
 */

/*
 * Mirrors enum Status in src/rust/font.rs. The two are kept in step by
 * font_status_string, which a static assertion sizes against this list.
 */
enum font_status {
    FONT_STATUS_OK = 0,
    FONT_STATUS_NULL_ARGUMENT = 1,
    FONT_STATUS_BAD_HEADER = 2,
    FONT_STATUS_BAD_GEOMETRY = 3,
    FONT_STATUS_TRUNCATED = 4,
    FONT_STATUS_TRAILING_BYTES = 5,
    FONT_STATUS_NO_SUCH_GLYPH = 6,
    FONT_STATUS_BUFFER_TOO_SMALL = 7
};

/*
 * The tallest cell this kernel will accept, and therefore the size of the row
 * buffer a caller must offer seneri_font_glyph. src/rust/font.rs refuses
 * anything taller, so the two bounds are one bound stated twice; the static
 * assertion in src/kernel/screen.c is what keeps them equal.
 */
#define FONT_MAX_CELL_HEIGHT 32U

/* Returns 1 when the reader's own rejection tests all pass. */
int32_t seneri_font_self_test(void);

/* How many bytes the built-in glyph table occupies. */
size_t seneri_font_size(void);

/* Read the cell size and covered range. Every pointer must be non-null. */
int32_t seneri_font_geometry(
    uint32_t *width,
    uint32_t *height,
    uint32_t *first,
    uint32_t *count
);

/*
 * Copy one glyph's rows into out, one byte per row, leftmost pixel in the most
 * significant bit. Writes exactly the cell height and no more.
 */
int32_t seneri_font_glyph(uint32_t code, uint8_t *out, size_t out_len);

const char *font_status_string(int32_t status);

#endif
