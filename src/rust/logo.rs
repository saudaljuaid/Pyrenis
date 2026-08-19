// SPDX-License-Identifier: GPL-3.0-only
//! The boot logo: a bounded decoder for a run-length encoded image.
//!
//! The image is produced by `tools/make-logo-asset.py` at build time. The
//! kernel cannot decode the PNG it comes from - that inflates to 16 MB, larger
//! than the whole kernel heap, and a DEFLATE decoder is a great deal of attack
//! surface to run before anything else works. So the expensive half happens at
//! development time and this reads a format that can be validated in one pass.
//!
//! Every length in the stream is attacker-controlled in principle, so nothing
//! here indexes without a check. That is the entire argument for this file
//! being Rust: the checks are the compiler's, not this author's.

/// Four magic bytes, a little-endian width and a little-endian height.
const HEADER_SIZE: usize = 8;
const MAGIC: [u8; 4] = *b"SRL1";

/// One length byte then four RGBA bytes.
const RUN_SIZE: usize = 5;

/// A Seneri policy bound. The framebuffer this draws into is at least this
/// wide on every mode the kernel accepts, and a header claiming more is a
/// header describing a different image than the one that was built.
const MAX_DIMENSION: u32 = 1024;

/// What a decode can conclude. Mirrored by `enum logo_status` in
/// `include/seneri/logo.h`; the two are kept in step by a compile-time
/// assertion on the C side.
#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Status {
    /// The image decoded and filled exactly the pixels its header claimed.
    Ok = 0,
    /// A null pointer crossed the boundary.
    NullArgument = 1,
    /// The blob is shorter than a header, or does not start with the magic.
    BadHeader = 2,
    /// The header names a width or height this kernel will not accept.
    BadGeometry = 3,
    /// A run of length zero, which would never terminate the image.
    ZeroRun = 4,
    /// The runs describe more pixels than the header declared.
    TooManyPixels = 5,
    /// The blob ended before the header's pixels were all described.
    Truncated = 6,
    /// Bytes remain after the last pixel, so the two disagree about the image.
    TrailingBytes = 7,
    /// The caller's buffer cannot hold the image the header describes.
    BufferTooSmall = 8,
}

/// How the framebuffer wants a pixel packed, and what to blend against.
///
/// Passed in rather than assumed: `src/kernel/framebuffer.c` reads the channel
/// positions from the boot loader, so this cannot hard-code a byte order any
/// more than that can.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Format {
    /// Bit position of the red channel.
    pub red_shift: u8,
    /// Bit position of the green channel.
    pub green_shift: u8,
    /// Bit position of the blue channel.
    pub blue_shift: u8,
    /// Already-packed colour that transparent parts of the logo show.
    pub background: u32,
}

/// The image's declared size, read without decoding it.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Geometry {
    /// Declared width in pixels.
    pub width: u32,
    /// Declared height in pixels.
    pub height: u32,
}

fn read_u16(bytes: &[u8]) -> u16 {
    u16::from(bytes[0]) | (u16::from(bytes[1]) << 8)
}

/// Read and validate the header without touching the runs.
pub fn geometry(blob: &[u8]) -> Result<Geometry, Status> {
    let header = blob.get(..HEADER_SIZE).ok_or(Status::BadHeader)?;

    if header[..4] != MAGIC {
        return Err(Status::BadHeader);
    }

    let width = u32::from(read_u16(&header[4..6]));
    let height = u32::from(read_u16(&header[6..8]));

    if width == 0 || height == 0 || width > MAX_DIMENSION
        || height > MAX_DIMENSION
    {
        return Err(Status::BadGeometry);
    }

    Ok(Geometry { width, height })
}

/// Blend one source pixel over the background and pack it for the framebuffer.
fn compose(red: u8, green: u8, blue: u8, alpha: u8, format: &Format) -> u32 {
    let mix = |channel: u8, under: u8| -> u32 {
        // Rounded rather than truncated, so a fully opaque pixel is exact.
        let value = (u32::from(channel) * u32::from(alpha)
            + u32::from(under) * u32::from(255 - alpha)
            + 127)
            / 255;
        value & 0xFF
    };

    let under = |shift: u8| -> u8 { ((format.background >> shift) & 0xFF) as u8 };

    (mix(red, under(format.red_shift)) << format.red_shift)
        | (mix(green, under(format.green_shift)) << format.green_shift)
        | (mix(blue, under(format.blue_shift)) << format.blue_shift)
}

/// Decode `blob` into `out`, one packed pixel per element, row by row.
///
/// `out` must hold exactly the pixels the header declares - not at least them.
/// A caller that passed a larger buffer would be told the image filled it when
/// it did not, and every pixel past the end would be whatever was there before.
pub fn decode(blob: &[u8], out: &mut [u32], format: &Format)
    -> Result<Geometry, Status>
{
    let geometry = geometry(blob)?;
    let pixels = (geometry.width as usize) * (geometry.height as usize);

    if out.len() < pixels {
        return Err(Status::BufferTooSmall);
    }

    let mut written = 0usize;
    let mut offset = HEADER_SIZE;

    while written < pixels {
        let run = blob
            .get(offset..offset + RUN_SIZE)
            .ok_or(Status::Truncated)?;
        let count = usize::from(run[0]);

        if count == 0 {
            return Err(Status::ZeroRun);
        }

        if count > pixels - written {
            return Err(Status::TooManyPixels);
        }

        let packed = compose(run[1], run[2], run[3], run[4], format);

        for slot in &mut out[written..written + count] {
            *slot = packed;
        }

        written += count;
        offset += RUN_SIZE;
    }

    // The runs and the header must agree about where the image ends.
    if offset != blob.len() {
        return Err(Status::TrailingBytes);
    }

    Ok(geometry)
}

/// Every refusal, driven by malformed blobs built here.
///
/// Runs on every boot before the real image is touched, so a decoder that
/// stopped checking is named at once rather than when something feeds it a
/// picture it did not expect.
pub fn self_test() -> bool {
    let format = Format {
        red_shift: 16,
        green_shift: 8,
        blue_shift: 0,
        background: 0,
    };
    let mut out = [0u32; 8];

    // A two-by-two image: one run of three, then one single pixel.
    let good: [u8; 18] = [
        b'S', b'R', b'L', b'1', 2, 0, 2, 0,
        3, 0x10, 0x20, 0x30, 0xFF,
        1, 0x40, 0x50, 0x60, 0xFF,
    ];

    let decoded = decode(&good, &mut out[..4], &format);

    if decoded != Ok(Geometry { width: 2, height: 2 })
        || out[0] != 0x0010_2030
        || out[2] != 0x0010_2030
        || out[3] != 0x0040_5060
    {
        return false;
    }

    // Alpha blends against the background rather than being ignored.
    let half: [u8; 13] = [
        b'S', b'R', b'L', b'1', 1, 0, 1, 0,
        1, 0xFF, 0xFF, 0xFF, 0x80,
    ];
    let over_black = Format { background: 0, ..format };
    let over_white = Format { background: 0x00FF_FFFF, ..format };

    if decode(&half, &mut out[..1], &over_black) .is_err() {
        return false;
    }

    let blended = out[0] & 0xFF;

    if blended == 0 || blended == 0xFF {
        return false;
    }

    if decode(&half, &mut out[..1], &over_white).is_err()
        || (out[0] & 0xFF) <= blended
    {
        return false;
    }

    // A fully transparent run must leave exactly the background.
    let clear: [u8; 13] = [
        b'S', b'R', b'L', b'1', 1, 0, 1, 0,
        1, 0xFF, 0x00, 0x00, 0x00,
    ];

    if decode(&clear, &mut out[..1], &over_white) != Ok(Geometry {
        width: 1,
        height: 1,
    }) || out[0] != 0x00FF_FFFF
    {
        return false;
    }

    // Every rejection, one broken field at a time.
    let mut broken = good;
    broken[0] = b'X';

    if decode(&broken, &mut out[..4], &format) != Err(Status::BadHeader) {
        return false;
    }

    if decode(&good[..HEADER_SIZE - 1], &mut out[..4], &format)
        != Err(Status::BadHeader)
    {
        return false;
    }

    broken = good;
    broken[4] = 0;

    if decode(&broken, &mut out[..4], &format) != Err(Status::BadGeometry) {
        return false;
    }

    broken = good;
    broken[6] = 0;

    if decode(&broken, &mut out[..4], &format) != Err(Status::BadGeometry) {
        return false;
    }

    // A width past the bound, written little-endian across both bytes.
    broken = good;
    broken[4] = 0x01;
    broken[5] = 0x04;

    if decode(&broken, &mut out[..4], &format) != Err(Status::BadGeometry) {
        return false;
    }

    // A run of zero length would leave the loop making no progress.
    broken = good;
    broken[8] = 0;

    if decode(&broken, &mut out[..4], &format) != Err(Status::ZeroRun) {
        return false;
    }

    // A run that alone exceeds the whole image.
    //
    // Five rather than four: a run of exactly the remaining pixels is legal,
    // and using it here meant this case fell through to the trailing-bytes
    // check instead - which is what the first run of this test found.
    broken = good;
    broken[8] = 5;

    if decode(&broken, &mut out[..4], &format) != Err(Status::TooManyPixels) {
        return false;
    }

    // And one that only exceeds it because of what came before.
    broken = good;
    broken[8] = 2;
    broken[13] = 3;

    if decode(&broken, &mut out[..4], &format) != Err(Status::TooManyPixels) {
        return false;
    }

    // The blob ends mid-run.
    if decode(&good[..good.len() - 1], &mut out[..4], &format)
        != Err(Status::Truncated)
    {
        return false;
    }

    // A buffer one pixel short of the declared image.
    if decode(&good, &mut out[..3], &format) != Err(Status::BufferTooSmall) {
        return false;
    }

    // Bytes after the last pixel: the runs and the header disagree.
    let mut trailing = [0u8; 23];
    trailing[..18].copy_from_slice(&good);
    trailing[18] = 1;

    if decode(&trailing, &mut out[..4], &format) != Err(Status::TrailingBytes) {
        return false;
    }

    // The geometry reader alone must agree with the full decode.
    geometry(&good) == Ok(Geometry { width: 2, height: 2 })
        && geometry(&broken[..2]) == Err(Status::BadHeader)
}
