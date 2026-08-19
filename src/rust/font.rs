// SPDX-License-Identifier: GPL-3.0-only
//! The console font: a bounded reader for a packed bitmap glyph table.
//!
//! The table is produced by `tools/make-font-asset.py` from the ASCII art in
//! `tools/font8x16.txt`. Doing the parsing at development time leaves the
//! kernel a format it can validate in a single pass with no allocation, which
//! matters here more than it did for the logo: the console runs before almost
//! everything, and a font lookup happens once per character printed.
//!
//! Every field in the header is a length or an index, and every one of them is
//! used to compute an offset into the blob. That is the entire argument for
//! this file being Rust - the bounds checks belong to the compiler rather than
//! to whoever remembers to write them.
//!
//! The glyph bitmaps themselves are derived from GNU Unifont, which is free
//! software under the GNU General Public License version 2 or, at the user's
//! option, any later version. `tools/font8x16.txt` carries the attribution.

/// Four magic bytes, then glyph width, glyph height, first code point and
/// glyph count, one byte each.
const HEADER_SIZE: usize = 8;
const MAGIC: [u8; 4] = *b"SNF1";

/// A row is one byte, so a glyph can be no wider than eight pixels. This is a
/// property of the format rather than a policy: a wider cell needs a wider row.
const MAX_WIDTH: u32 = 8;

/// A Seneri policy bound. The smallest mode the kernel accepts is 640x480, so a
/// cell taller than this could not fit a single row of text on screen.
const MAX_HEIGHT: u32 = 32;

/// What reading the table can conclude. Mirrored by `enum font_status` in
/// `include/seneri/font.h`; a compile-time assertion on the C side keeps the
/// two in step.
#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Status {
    /// The table is well formed and the glyph was produced.
    Ok = 0,
    /// A null pointer crossed the boundary.
    NullArgument = 1,
    /// The blob is shorter than a header, or does not start with the magic.
    BadHeader = 2,
    /// The header names a cell size or glyph range this kernel will not accept.
    BadGeometry = 3,
    /// The blob ends before the header's glyphs are all present.
    Truncated = 4,
    /// Bytes remain after the last glyph, so header and body disagree.
    TrailingBytes = 5,
    /// The code point asked for is outside the range the table covers.
    NoSuchGlyph = 6,
    /// The caller's buffer cannot hold one glyph's rows.
    BufferTooSmall = 7,
}

/// What the table's header declares.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Geometry {
    /// Pixels across one cell. At most eight, because a row is one byte.
    pub width: u32,
    /// Pixels down one cell, which is also the bytes per glyph.
    pub height: u32,
    /// The code point the first glyph stands for.
    pub first: u32,
    /// How many consecutive code points the table covers.
    pub count: u32,
}

/// Read and fully validate the header.
///
/// The whole blob is checked here, not only the first eight bytes: a header
/// that agrees with itself but not with the number of bytes behind it is a
/// header describing a different table than the one that was built.
pub fn geometry(blob: &[u8]) -> Result<Geometry, Status> {
    if blob.len() < HEADER_SIZE {
        return Err(Status::BadHeader);
    }

    if blob[0..4] != MAGIC {
        return Err(Status::BadHeader);
    }

    let width = u32::from(blob[4]);
    let height = u32::from(blob[5]);
    let first = u32::from(blob[6]);
    let count = u32::from(blob[7]);

    if width == 0 || width > MAX_WIDTH {
        return Err(Status::BadGeometry);
    }

    if height == 0 || height > MAX_HEIGHT {
        return Err(Status::BadGeometry);
    }

    if count == 0 || first + count > 256 {
        return Err(Status::BadGeometry);
    }

    // count and height are each at most 256 and 32, so this cannot overflow a
    // usize on any target this kernel builds for.
    let body = (count as usize) * (height as usize);

    let declared = match HEADER_SIZE.checked_add(body) {
        Some(total) => total,
        None => return Err(Status::BadGeometry),
    };

    if blob.len() < declared {
        return Err(Status::Truncated);
    }

    if blob.len() > declared {
        return Err(Status::TrailingBytes);
    }

    Ok(Geometry {
        width,
        height,
        first,
        count,
    })
}

/// Copy one glyph's rows into `out`, one byte per row, leftmost pixel in the
/// most significant bit.
///
/// `out` may be longer than the cell; only the first `height` bytes are
/// written, and the number written is returned so the caller need not re-read
/// the header to know it.
pub fn glyph(blob: &[u8], code: u32, out: &mut [u8]) -> Result<usize, Status> {
    let geometry = geometry(blob)?;

    if code < geometry.first || code >= geometry.first + geometry.count {
        return Err(Status::NoSuchGlyph);
    }

    let height = geometry.height as usize;

    if out.len() < height {
        return Err(Status::BufferTooSmall);
    }

    let index = (code - geometry.first) as usize;
    let start = HEADER_SIZE + index * height;

    // geometry() proved blob.len() == HEADER_SIZE + count * height and the
    // range check above proved index < count, so this slice is inside the
    // blob. The compiler checks it anyway, which is the point of this file.
    let rows = match blob.get(start..start + height) {
        Some(rows) => rows,
        None => return Err(Status::Truncated),
    };

    out[..height].copy_from_slice(rows);
    Ok(height)
}

/// Build a minimal well-formed table in `buffer` and return the part used.
///
/// One glyph of two rows, so the fixtures below can damage exactly one field
/// at a time and nothing else.
fn fixture(buffer: &mut [u8; 16]) -> usize {
    buffer[0..4].copy_from_slice(&MAGIC);
    buffer[4] = 8; // width
    buffer[5] = 2; // height
    buffer[6] = 0x20; // first
    buffer[7] = 1; // count
    buffer[8] = 0b1010_1010;
    buffer[9] = 0b0101_0101;
    HEADER_SIZE + 2
}

/// The refusals this reader must make, checked on synthetic tables.
///
/// Every case below is unreachable from the font the build produces, which is
/// exactly why it is here: a rejection nothing can reach is a rejection nobody
/// has ever seen work.
pub fn self_test() -> bool {
    let mut buffer = [0u8; 16];
    let used = fixture(&mut buffer);

    // The good case, first, so a later failure cannot be blamed on the fixture.
    let good = match geometry(&buffer[..used]) {
        Ok(geometry) => geometry,
        Err(_) => return false,
    };

    if good.width != 8 || good.height != 2 || good.first != 0x20 || good.count != 1 {
        return false;
    }

    let mut rows = [0u8; 2];

    match glyph(&buffer[..used], 0x20, &mut rows) {
        Ok(2) => (),
        _ => return false,
    }

    if rows != [0b1010_1010, 0b0101_0101] {
        return false;
    }

    // Shorter than a header.
    if geometry(&buffer[..HEADER_SIZE - 1]) != Err(Status::BadHeader) {
        return false;
    }

    // Wrong magic.
    let mut damaged = buffer;
    damaged[3] = b'2';
    if geometry(&damaged[..used]) != Err(Status::BadHeader) {
        return false;
    }

    // A cell no pixels wide, and one wider than a row can hold.
    for width in [0u8, (MAX_WIDTH + 1) as u8] {
        let mut damaged = buffer;
        damaged[4] = width;
        if geometry(&damaged[..used]) != Err(Status::BadGeometry) {
            return false;
        }
    }

    // A cell no pixels tall. A cell taller than the bound cannot be built here
    // without also changing the body length, so height is checked at zero only.
    let mut damaged = buffer;
    damaged[5] = 0;
    if geometry(&damaged[..used]) != Err(Status::BadGeometry) {
        return false;
    }

    // No glyphs at all, and a range running off the end of a byte.
    let mut damaged = buffer;
    damaged[7] = 0;
    if geometry(&damaged[..used]) != Err(Status::BadGeometry) {
        return false;
    }

    let mut damaged = buffer;
    damaged[6] = 0xFF;
    damaged[7] = 2;
    if geometry(&damaged[..used]) != Err(Status::BadGeometry) {
        return false;
    }

    // A header claiming more glyphs than there are bytes for.
    let mut damaged = buffer;
    damaged[7] = 2;
    if geometry(&damaged[..used]) != Err(Status::Truncated) {
        return false;
    }

    // Bytes after the last glyph.
    if geometry(&buffer[..used + 1]) != Err(Status::TrailingBytes) {
        return false;
    }

    // Either side of the covered range.
    if glyph(&buffer[..used], 0x1F, &mut rows) != Err(Status::NoSuchGlyph) {
        return false;
    }

    if glyph(&buffer[..used], 0x21, &mut rows) != Err(Status::NoSuchGlyph) {
        return false;
    }

    // A buffer one byte too small for the cell.
    let mut cramped = [0u8; 1];
    if glyph(&buffer[..used], 0x20, &mut cramped) != Err(Status::BufferTooSmall) {
        return false;
    }

    true
}
