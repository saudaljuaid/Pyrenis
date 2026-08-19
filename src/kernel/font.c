/* SPDX-License-Identifier: GPL-3.0-only */
#include <stddef.h>
#include <stdint.h>

#include <seneri/font.h>

/*
 * The C half of the console font: the names for what the Rust reader can
 * refuse. It sits on this side for the same reason logo.c does - every other
 * subsystem's status strings are here, and a caller should not need to know
 * which language produced a refusal in order to print it.
 */
const char *font_status_string(int32_t status)
{
    static const char *const messages[] = {
        "ok",
        "null font argument",
        "font table header is missing or malformed",
        "font table declares a cell or range this kernel will not accept",
        "font table ended before its glyphs were all present",
        "font table has bytes after its last glyph",
        "font table does not cover that character",
        "font glyph does not fit the buffer offered for it"
    };

    _Static_assert(
        sizeof(messages) / sizeof(messages[0]) ==
            (size_t)FONT_STATUS_BUFFER_TOO_SMALL + 1U,
        "font status messages are out of sync with src/rust/font.rs"
    );

    if (status < 0 ||
        (size_t)status >= sizeof(messages) / sizeof(messages[0])) {
        return "unknown font status";
    }

    return messages[status];
}
