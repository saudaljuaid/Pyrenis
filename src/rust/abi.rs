// SPDX-License-Identifier: GPL-3.0-only
//! Where C calls Rust.
//!
//! Every function here is `extern "C"` and every one of them is the boundary
//! at which Rust's guarantees stop: a caller that passes a bad pointer or a
//! wrong length gets exactly the same undefined behaviour it would get from a
//! C callee. So the boundary is kept as small as it can be - raw pointers turn
//! into slices immediately, once, and everything past that point is safe Rust.
//!
//! That is also why there is exactly one `unsafe` block per entry point, each
//! with the condition the caller has to have met written above it.

use crate::font;
use crate::logo::{self, Format, Status};

/// The run-length image, produced by `tools/make-logo-asset.py` at build time.
/// The Makefile points `SENERI_LOGO_BLOB` at it; there is no committed copy.
static LOGO: &[u8] = include_bytes!(env!("SENERI_LOGO_BLOB"));

fn status_code(status: Status) -> i32 {
    status as i32
}

/// Run the decoder's own tests. Returns 1 when they all pass.
#[unsafe(no_mangle)]
pub extern "C" fn seneri_logo_self_test() -> i32 {
    i32::from(logo::self_test())
}

/// How many bytes the built-in image occupies.
#[unsafe(no_mangle)]
pub extern "C" fn seneri_logo_size() -> usize {
    LOGO.len()
}

/// Read the built-in image's declared size without decoding it.
///
/// # Safety
///
/// `width` and `height` must each be null or point at a writable `u32`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn seneri_logo_geometry(
    width: *mut u32,
    height: *mut u32,
) -> i32 {
    if width.is_null() || height.is_null() {
        return status_code(Status::NullArgument);
    }

    match logo::geometry(LOGO) {
        Ok(geometry) => {
            // SAFETY: both pointers were checked non-null just above, and the
            // caller's contract is that each addresses a writable u32.
            unsafe {
                *width = geometry.width;
                *height = geometry.height;
            }
            status_code(Status::Ok)
        }
        Err(status) => status_code(status),
    }
}

/// Decode the built-in image into `out`, one packed pixel per element.
///
/// `out` is filled row by row and must hold exactly `out_pixels` writable
/// `u32`s. The channel shifts and background come from the framebuffer, so
/// nothing here assumes a byte order.
///
/// # Safety
///
/// `out` must point at `out_pixels` writable, aligned `u32`s, and must not
/// alias anything else live for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn seneri_logo_decode(
    out: *mut u32,
    out_pixels: usize,
    red_shift: u8,
    green_shift: u8,
    blue_shift: u8,
    background: u32,
) -> i32 {
    if out.is_null() {
        return status_code(Status::NullArgument);
    }

    // SAFETY: the caller's contract is exactly the requirement of
    // from_raw_parts_mut - out_pixels writable, aligned, non-aliased u32s -
    // and the null case was refused above. This is the only place in the crate
    // where a pointer becomes a slice; everything below it is bounds checked.
    let pixels = unsafe { core::slice::from_raw_parts_mut(out, out_pixels) };

    let format = Format {
        red_shift,
        green_shift,
        blue_shift,
        background,
    };

    match logo::decode(LOGO, pixels, &format) {
        Ok(_) => status_code(Status::Ok),
        Err(status) => status_code(status),
    }
}

/// The packed glyph table, produced by `tools/make-font-asset.py` at build
/// time. The Makefile points `SENERI_FONT_BLOB` at it; there is no committed
/// copy of the blob, only the ASCII art it is built from.
static FONT: &[u8] = include_bytes!(env!("SENERI_FONT_BLOB"));

fn font_status_code(status: font::Status) -> i32 {
    status as i32
}

/// Run the font reader's own tests. Returns 1 when they all pass.
#[unsafe(no_mangle)]
pub extern "C" fn seneri_font_self_test() -> i32 {
    i32::from(font::self_test())
}

/// How many bytes the built-in glyph table occupies.
#[unsafe(no_mangle)]
pub extern "C" fn seneri_font_size() -> usize {
    FONT.len()
}

/// Read the glyph table's cell size and covered range without copying a glyph.
///
/// # Safety
///
/// Each pointer must be non-null and address a writable `u32`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn seneri_font_geometry(
    width: *mut u32,
    height: *mut u32,
    first: *mut u32,
    count: *mut u32,
) -> i32 {
    if width.is_null() || height.is_null() || first.is_null() || count.is_null() {
        return font_status_code(font::Status::NullArgument);
    }

    match font::geometry(FONT) {
        Ok(geometry) => {
            // SAFETY: all four pointers were checked non-null just above, and
            // the caller's contract is that each addresses a writable u32.
            unsafe {
                *width = geometry.width;
                *height = geometry.height;
                *first = geometry.first;
                *count = geometry.count;
            }
            font_status_code(font::Status::Ok)
        }
        Err(status) => font_status_code(status),
    }
}

/// Copy one glyph's rows into `out`, one byte per row, leftmost pixel in the
/// most significant bit. Writes `height` bytes and no more.
///
/// # Safety
///
/// `out` must point at `out_len` writable bytes and must not alias anything
/// else live for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn seneri_font_glyph(
    code: u32,
    out: *mut u8,
    out_len: usize,
) -> i32 {
    if out.is_null() {
        return font_status_code(font::Status::NullArgument);
    }

    // SAFETY: the caller's contract is exactly the requirement of
    // from_raw_parts_mut - out_len writable, non-aliased bytes - and the null
    // case was refused above. Everything past this line is bounds checked.
    let rows = unsafe { core::slice::from_raw_parts_mut(out, out_len) };

    match font::glyph(FONT, code, rows) {
        Ok(_) => font_status_code(font::Status::Ok),
        Err(status) => font_status_code(status),
    }
}
