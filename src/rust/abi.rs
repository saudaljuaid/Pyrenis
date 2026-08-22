// SPDX-License-Identifier: GPL-3.0-only
//! Where C calls Rust.
//!
//! Every function here is `extern "C"` and every one of them is the boundary
//! at which Rust's guarantees stop: a caller that passes a bad pointer or a
//! wrong length gets exactly the same undefined behaviour it would get from a
//! C callee. So the boundary is kept as small as it can be - raw pointers turn
//! into slices immediately, once, and everything past that point is safe Rust.
//!
//! Unsafe blocks appear only where validated C pointers become Rust slices or
//! where results are written back through validated C pointers. Each one states
//! the condition the caller has to meet.

use crate::elf64;
use crate::font;
use crate::fat16;
use crate::logo::{self, Format, Status};
use crate::linux_fat16;
use crate::linux_elf64;
use crate::ui_font;

/// Stop in C's console panic path if a compiler-inserted check ever fires.
pub(crate) fn panic() -> ! {
    unsafe extern "C" {
        fn console_panic(message: *const u8) -> !;
    }

    // SAFETY: this is a static NUL-terminated string and the C function never
    // returns. Keeping this declaration here preserves the one unsafe module.
    unsafe { console_panic(c"Rust panicked".as_ptr() as *const u8) }
}

/// The run-length image, produced by `tools/make-logo-asset.py` at build time.
/// The Makefile points `SAPOTE_LOGO_BLOB` at it; there is no committed copy.
static LOGO: &[u8] = include_bytes!(env!("SAPOTE_LOGO_BLOB"));

fn status_code(status: Status) -> i32 {
    status as i32
}

/// Run the decoder's own tests. Returns 1 when they all pass.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_logo_self_test() -> i32 {
    i32::from(logo::self_test())
}

/// How many bytes the built-in image occupies.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_logo_size() -> usize {
    LOGO.len()
}

/// Read the built-in image's declared size without decoding it.
///
/// # Safety
///
/// `width` and `height` must both be non-null and point at writable `u32` values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_logo_geometry(
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
pub unsafe extern "C" fn sapote_logo_decode(
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
/// time. The Makefile points `SAPOTE_FONT_BLOB` at it; there is no committed
/// copy of the blob, only the ASCII art it is built from.
static FONT: &[u8] = include_bytes!(env!("SAPOTE_FONT_BLOB"));

fn font_status_code(status: font::Status) -> i32 {
    status as i32
}

/// Run the font reader's own tests. Returns 1 when they all pass.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_font_self_test() -> i32 {
    i32::from(font::self_test())
}

/// How many bytes the built-in glyph table occupies.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_font_size() -> usize {
    FONT.len()
}

/// Read the glyph table's cell size and covered range without copying a glyph.
///
/// # Safety
///
/// Each pointer must be non-null and address a writable `u32`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_font_geometry(
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
pub unsafe extern "C" fn sapote_font_glyph(
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

/// Build-packed Spleen 12x24 glyphs. No BDF parser enters the kernel image.
static UI_FONT: &[u8] = include_bytes!(env!("SAPOTE_UI_FONT_BLOB"));

fn ui_font_status_code(status: ui_font::Status) -> i32 {
    status as i32
}

/// Run the SUF1 parser's synthetic acceptance and refusal tests.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_ui_font_self_test() -> i32 {
    i32::from(ui_font::self_test())
}

/// Return the byte length of the built-in SUF1 asset.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_ui_font_size() -> usize {
    UI_FONT.len()
}

/// Return a stable FNV-1a fingerprint of the exact built-in bytes.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_ui_font_fingerprint() -> u64 {
    ui_font::fingerprint(UI_FONT)
}

/// Validate the built-in asset and copy all declared metrics.
///
/// # Safety
///
/// `metrics` must be non-null and point to one writable `ui_font::Geometry`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_ui_font_geometry(
    metrics: *mut ui_font::Geometry,
) -> i32 {
    if metrics.is_null() {
        return ui_font_status_code(ui_font::Status::NullArgument);
    }
    match ui_font::geometry(UI_FONT) {
        Ok(value) => {
            // SAFETY: the pointer was checked above; the caller owns one
            // writable value with the same repr(C) layout.
            unsafe { *metrics = value };
            ui_font_status_code(ui_font::Status::Ok)
        }
        Err(status) => ui_font_status_code(status),
    }
}

/// Copy one glyph from the built-in SUF1 body into a caller-owned buffer.
///
/// # Safety
///
/// `out` must address `out_len` writable, non-aliased bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_ui_font_glyph(
    code: u32,
    out: *mut u8,
    out_len: usize,
) -> i32 {
    if out.is_null() {
        return ui_font_status_code(ui_font::Status::NullArgument);
    }
    // SAFETY: this is the caller's contract, and the null case was refused.
    let bytes = unsafe { core::slice::from_raw_parts_mut(out, out_len) };
    match ui_font::glyph(UI_FONT, code, bytes) {
        Ok(_) => ui_font_status_code(ui_font::Status::Ok),
        Err(status) => ui_font_status_code(status),
    }
}

fn fat16_status_code(status: fat16::Status) -> i32 {
    status as i32
}

/// Validate a CPU-owned BPB block and copy pointer-free checked geometry.
///
/// # Safety
///
/// `block` must address `block_len` readable, non-aliased bytes and `out` must
/// address one writable `fat16::Geometry`. The two ranges must not overlap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_fat16_parse_bpb(
    block: *const u8,
    block_len: usize,
    namespace_blocks: u64,
    namespace_block_bytes: u32,
    out: *mut fat16::Geometry,
) -> i32 {
    if out.is_null() {
        return fat16_status_code(fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable Geometry and null was refused.
    unsafe { *out = fat16::Geometry::invalid() };
    if block.is_null() {
        return fat16_status_code(fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises this readable range; null was refused.
    let bytes = unsafe { core::slice::from_raw_parts(block, block_len) };
    match fat16::parse_bpb(bytes, namespace_blocks, namespace_block_bytes) {
        Ok(value) => {
            // SAFETY: the validated non-null output still names one value.
            unsafe { *out = value };
            fat16_status_code(fat16::Status::Ok)
        }
        Err(status) => fat16_status_code(status),
    }
}

/// Validate and copy an exact canonical raw 8.3 query.
///
/// # Safety
///
/// `name` must address `name_len` readable bytes and `out` one writable query;
/// the ranges must not overlap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_fat16_make_query(
    name: *const u8,
    name_len: usize,
    out: *mut fat16::RootQuery,
) -> i32 {
    if out.is_null() {
        return fat16_status_code(fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable RootQuery and null was refused.
    unsafe { *out = fat16::RootQuery::invalid() };
    if name.is_null() {
        return fat16_status_code(fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises this readable range; null was refused.
    let bytes = unsafe { core::slice::from_raw_parts(name, name_len) };
    match fat16::make_query(bytes) {
        Ok(value) => {
            // SAFETY: the validated non-null output still names one value.
            unsafe { *out = value };
            fat16_status_code(fat16::Status::Ok)
        }
        Err(status) => fat16_status_code(status),
    }
}

/// Locate one validated root entry inside one CPU-owned root block.
///
/// # Safety
///
/// `block` must address `block_len` readable bytes; `geometry` and `query`
/// must each address one readable value; `out` must address one writable root
/// entry. No input may overlap the output.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_fat16_find_root(
    block: *const u8,
    block_len: usize,
    geometry: *const fat16::Geometry,
    query: *const fat16::RootQuery,
    destination_bytes: u32,
    out: *mut fat16::RootEntry,
) -> i32 {
    if out.is_null() {
        return fat16_status_code(fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable RootEntry and null was refused.
    unsafe { *out = fat16::RootEntry::invalid() };
    if block.is_null() || geometry.is_null() || query.is_null() {
        return fat16_status_code(fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises this readable range and two readable values;
    // all null cases were refused and the output is non-aliased by contract.
    let (bytes, checked_geometry, checked_query) = unsafe {
        (
            core::slice::from_raw_parts(block, block_len),
            *geometry,
            *query,
        )
    };
    match fat16::find_root(
        bytes,
        &checked_geometry,
        &checked_query,
        destination_bytes,
    ) {
        Ok(value) => {
            // SAFETY: the validated non-null output still names one value.
            unsafe { *out = value };
            fat16_status_code(fat16::Status::Ok)
        }
        Err(status) => fat16_status_code(status),
    }
}

/// Validate FAT16 reserved entries and capture cluster two's EOC by value.
///
/// # Safety
///
/// `block` must address `block_len` readable bytes; `geometry` must address one
/// readable value; `out` must address one writable FAT result. No input may
/// overlap the output.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_fat16_parse_fat(
    block: *const u8,
    block_len: usize,
    geometry: *const fat16::Geometry,
    out: *mut fat16::FatState,
) -> i32 {
    if out.is_null() {
        return fat16_status_code(fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable FatState and null was refused.
    unsafe { *out = fat16::FatState::invalid() };
    if block.is_null() || geometry.is_null() {
        return fat16_status_code(fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises this readable range and one readable value;
    // all null cases were refused and the output is non-aliased by contract.
    let (bytes, checked_geometry) = unsafe {
        (
            core::slice::from_raw_parts(block, block_len),
            *geometry,
        )
    };
    match fat16::parse_fat(bytes, &checked_geometry) {
        Ok(value) => {
            // SAFETY: the validated non-null output still names one value.
            unsafe { *out = value };
            fat16_status_code(fat16::Status::Ok)
        }
        Err(status) => fat16_status_code(status),
    }
}

/// Join validated geometry, root, and FAT values into one checked extent.
///
/// # Safety
///
/// Each input must address one readable value and `out` one writable extent;
/// no input may overlap the output.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_fat16_validate_extent(
    geometry: *const fat16::Geometry,
    entry: *const fat16::RootEntry,
    fat: *const fat16::FatState,
    out: *mut fat16::Extent,
) -> i32 {
    if out.is_null() {
        return fat16_status_code(fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable Extent and null was refused.
    unsafe { *out = fat16::Extent::invalid() };
    if geometry.is_null() || entry.is_null() || fat.is_null() {
        return fat16_status_code(fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises three readable non-aliased values and every
    // null case was refused above.
    let (checked_geometry, checked_entry, checked_fat) = unsafe {
        (*geometry, *entry, *fat)
    };
    match fat16::validate_extent(
        &checked_geometry,
        &checked_entry,
        &checked_fat,
    ) {
        Ok(value) => {
            // SAFETY: the validated non-null output still names one value.
            unsafe { *out = value };
            fat16_status_code(fat16::Status::Ok)
        }
        Err(status) => fat16_status_code(status),
    }
}

/// Validate deterministic file bytes and return their SHA-256 by value.
///
/// # Safety
///
/// `data` must address `data_len` readable bytes and `out` one writable
/// `fat16::Payload`; the ranges must not overlap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_fat16_validate_payload(
    data: *const u8,
    data_len: usize,
    out: *mut fat16::Payload,
) -> i32 {
    if out.is_null() {
        return fat16_status_code(fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable Payload and null was refused.
    unsafe { *out = fat16::Payload::invalid() };
    if data.is_null() {
        return fat16_status_code(fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises this readable range; null was refused.
    let bytes = unsafe { core::slice::from_raw_parts(data, data_len) };
    match fat16::validate_payload(bytes) {
        Ok(value) => {
            // SAFETY: the validated non-null output still names one value.
            unsafe { *out = value };
            fat16_status_code(fat16::Status::Ok)
        }
        Err(status) => fat16_status_code(status),
    }
}

fn linux_fat16_status_code(status: linux_fat16::Status) -> i32 {
    status as i32
}

/// Run the pointer-free BusyBox FAT-chain invariant controls.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_linux_fat16_self_test() -> u32 {
    linux_fat16::self_test()
}

/// Construct the one canonical raw 8.3 BusyBox query by value.
///
/// # Safety
///
/// `out` must address one writable root query.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_linux_fat16_make_query(
    out: *mut fat16::RootQuery,
) -> i32 {
    if out.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable RootQuery and null was refused.
    unsafe { *out = linux_fat16::make_query() };
    linux_fat16_status_code(linux_fat16::Status::Ok)
}

/// Locate the one bounded BusyBox root entry in a CPU-owned root block.
///
/// # Safety
///
/// Inputs must address their complete readable values, `out` must address one
/// writable root entry, and no input may overlap the output.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_linux_fat16_find_root(
    block: *const u8,
    block_len: usize,
    geometry: *const fat16::Geometry,
    query: *const fat16::RootQuery,
    destination_bytes: u32,
    out: *mut fat16::RootEntry,
) -> i32 {
    if out.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable RootEntry and null was refused.
    unsafe { *out = fat16::RootEntry::invalid() };
    if block.is_null() || geometry.is_null() || query.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises the readable, non-aliased values above.
    let (bytes, checked_geometry, checked_query) = unsafe {
        (core::slice::from_raw_parts(block, block_len), *geometry, *query)
    };
    match linux_fat16::find_root(
        bytes, &checked_geometry, &checked_query, destination_bytes,
    ) {
        Ok(value) => {
            // SAFETY: the non-null output still names one writable value.
            unsafe { *out = value };
            linux_fat16_status_code(linux_fat16::Status::Ok)
        }
        Err(status) => linux_fat16_status_code(status),
    }
}

/// Validate the exact bounded FAT chain and return pointer-free cluster/LBA
/// values.
///
/// # Safety
///
/// Inputs must address their complete readable values, `out` must address one
/// writable chain, and no input may overlap the output.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_linux_fat16_build_chain(
    fat_block: *const u8,
    fat_len: usize,
    geometry: *const fat16::Geometry,
    entry: *const fat16::RootEntry,
    out: *mut linux_fat16::Chain,
) -> i32 {
    if out.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable Chain and null was refused.
    unsafe { *out = linux_fat16::Chain::invalid() };
    if fat_block.is_null() || geometry.is_null() || entry.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises the readable, non-aliased values above.
    let (bytes, checked_geometry, checked_entry) = unsafe {
        (core::slice::from_raw_parts(fat_block, fat_len), *geometry, *entry)
    };
    match linux_fat16::build_chain(bytes, &checked_geometry, &checked_entry) {
        Ok(value) => {
            // SAFETY: the non-null output still names one writable value.
            unsafe { *out = value };
            linux_fat16_status_code(linux_fat16::Status::Ok)
        }
        Err(status) => linux_fat16_status_code(status),
    }
}

/// Validate the complete CPU-owned BusyBox bytes and return their SHA-256.
///
/// # Safety
///
/// `data` must address `data_len` readable bytes, `out` one writable payload,
/// and the two ranges must not overlap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_linux_fat16_validate_payload(
    data: *const u8,
    data_len: usize,
    out: *mut linux_fat16::Payload,
) -> i32 {
    if out.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable Payload and null was refused.
    unsafe { *out = linux_fat16::Payload::invalid() };
    if data.is_null() {
        return linux_fat16_status_code(linux_fat16::Status::NullArgument);
    }
    // SAFETY: the caller promises this readable range; null was refused.
    let bytes = unsafe { core::slice::from_raw_parts(data, data_len) };
    match linux_fat16::validate_payload(bytes) {
        Ok(value) => {
            // SAFETY: the non-null output still names one writable value.
            unsafe { *out = value };
            linux_fat16_status_code(linux_fat16::Status::Ok)
        }
        Err(status) => linux_fat16_status_code(status),
    }
}

fn linux_elf64_status_code(status: linux_elf64::Status) -> i32 {
    status as i32
}

/// Run the pointer-free measured BusyBox ELF conjunction controls.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_linux_elf64_self_test() -> u32 {
    linux_elf64::self_test()
}

/// Parse one complete CPU-owned BusyBox ELF into pointer-free segment facts.
///
/// # Safety
///
/// `input` must address `input_len` readable, non-aliased bytes and `out` one
/// writable validated image. The ranges must not overlap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_linux_elf64_parse(
    input: *const u8,
    input_len: usize,
    out: *mut linux_elf64::ValidatedImage,
) -> i32 {
    if out.is_null() {
        return linux_elf64_status_code(linux_elf64::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable result and null was refused.
    unsafe { *out = linux_elf64::ValidatedImage::invalid() };
    if input.is_null() {
        return linux_elf64_status_code(linux_elf64::Status::NullArgument);
    }
    // SAFETY: the caller promises this complete readable range.
    let bytes = unsafe { core::slice::from_raw_parts(input, input_len) };
    match linux_elf64::parse(bytes) {
        Ok(value) => {
            // SAFETY: the validated output pointer still names one value.
            unsafe { *out = value };
            linux_elf64_status_code(linux_elf64::Status::Ok)
        }
        Err(status) => linux_elf64_status_code(status),
    }
}

fn elf64_status_code(status: elf64::Status) -> i32 {
    status as i32
}

/// Run all host-independent ELF64 parser mutation families.
#[unsafe(no_mangle)]
pub extern "C" fn sapote_elf64_self_test() -> u32 {
    elf64::self_test()
}

/// Parse one CPU-owned ELF file into pointer-free validated facts.
///
/// # Safety
///
/// `input` must address `input_len` readable, non-aliased bytes and `out` must
/// address one writable `elf64::ValidatedImage`.  The ranges must not overlap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sapote_elf64_parse(
    input: *const u8,
    input_len: usize,
    out: *mut elf64::ValidatedImage,
) -> i32 {
    if out.is_null() {
        return elf64_status_code(elf64::Status::NullArgument);
    }
    // SAFETY: the caller promises one writable output and null was refused.
    unsafe { *out = elf64::ValidatedImage::invalid() };
    if input.is_null() {
        return elf64_status_code(elf64::Status::NullArgument);
    }
    // SAFETY: the caller promises this one readable range; null was refused.
    let bytes = unsafe { core::slice::from_raw_parts(input, input_len) };
    match elf64::parse(bytes) {
        Ok(value) => {
            // SAFETY: the validated output pointer still names one value.
            unsafe { *out = value };
            elf64_status_code(elf64::Status::Ok)
        }
        Err(status) => elf64_status_code(status),
    }
}
