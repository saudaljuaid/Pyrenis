/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SENERI_LOGO_H
#define SENERI_LOGO_H

#include <stddef.h>
#include <stdint.h>

/*
 * The boot logo, decoded by src/rust/logo.rs.
 *
 * This is the first thing in Seneri that is not C, and the choice is argued in
 * docs/RUST.md: the decoder reads a byte stream whose every length field is
 * attacker-controlled in principle, which is precisely where a bounds check
 * the compiler inserts is worth more than one an author remembered.
 *
 * Nothing about the interface is special. These are ordinary C functions with
 * ordinary refusals; the caller cannot tell what language answered.
 */

/*
 * Mirrors enum Status in src/rust/logo.rs. The two are kept in step by
 * logo_status_string, which a static assertion sizes against this list.
 */
enum logo_status {
    LOGO_STATUS_OK = 0,
    LOGO_STATUS_NULL_ARGUMENT = 1,
    LOGO_STATUS_BAD_HEADER = 2,
    LOGO_STATUS_BAD_GEOMETRY = 3,
    LOGO_STATUS_ZERO_RUN = 4,
    LOGO_STATUS_TOO_MANY_PIXELS = 5,
    LOGO_STATUS_TRUNCATED = 6,
    LOGO_STATUS_TRAILING_BYTES = 7,
    LOGO_STATUS_BUFFER_TOO_SMALL = 8
};

/* Returns 1 when the decoder's own rejection tests all pass. */
int32_t seneri_logo_self_test(void);

/* How many bytes the built-in image occupies. */
size_t seneri_logo_size(void);

/* Read the declared size without decoding. Both pointers must be non-null. */
int32_t seneri_logo_geometry(uint32_t *width, uint32_t *height);

/*
 * Decode into a buffer of exactly out_pixels packed pixels, row by row. The
 * channel shifts and the background come from the framebuffer, so the decoder
 * assumes no byte order.
 */
int32_t seneri_logo_decode(
    uint32_t *out,
    size_t out_pixels,
    uint8_t red_shift,
    uint8_t green_shift,
    uint8_t blue_shift,
    uint32_t background
);

const char *logo_status_string(int32_t status);

#endif
