/* SPDX-License-Identifier: GPL-3.0-only */
#include <stddef.h>
#include <stdint.h>

#include <seneri/logo.h>

/*
 * The C half of the boot logo: nothing but the names for what the Rust decoder
 * can refuse. It lives on this side because every other subsystem's status
 * strings do, and because a caller should not have to know which language
 * produced a refusal in order to print it.
 */
const char *logo_status_string(int32_t status)
{
    static const char *const messages[] = {
        "ok",
        "null logo argument",
        "logo header is missing or malformed",
        "logo declares a size this kernel will not accept",
        "logo contains a run of zero length",
        "logo runs describe more pixels than its header",
        "logo ended before its header's pixels were described",
        "logo has bytes after its last pixel",
        "logo does not fit the buffer offered for it"
    };

    _Static_assert(
        sizeof(messages) / sizeof(messages[0]) ==
            (size_t)LOGO_STATUS_BUFFER_TOO_SMALL + 1U,
        "logo status messages are out of sync with src/rust/logo.rs"
    );

    if (status < 0 ||
        (size_t)status >= sizeof(messages) / sizeof(messages[0])) {
        return "unknown logo status";
    }

    return messages[status];
}
