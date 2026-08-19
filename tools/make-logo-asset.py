#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Turn assets/seneri-logo.png into the run-length asset the kernel draws.

The kernel cannot decode a PNG: the logo inflates to 16 MB, which is larger
than the whole kernel heap, and a DEFLATE decoder is a lot of attack surface to
run before anything else works.  So the expensive half happens here, at
development time, and the kernel is left with a format it can validate in a
single bounded pass.

Run it only when the logo itself changes:

    python3 tools/make-logo-asset.py assets/seneri-logo.png 256 build/logo.srl

The result is a build artifact and is not committed; the kernel includes it at
compile time.  The wire format is four magic bytes, a little-endian width and
height, then runs of one length byte and four RGBA bytes.

Requires only the Python standard library.
"""
import sys
import zlib
import struct


def read_png(path):
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise SystemExit('not a PNG')
    pos, idat, header = 8, bytearray(), None
    while pos < len(data):
        length = struct.unpack('>I', data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b'IHDR':
            header = struct.unpack('>IIBBBBB', body[:13])
        elif kind == b'IDAT':
            idat += body
        elif kind == b'IEND':
            break
        pos += 12 + length
    width, height, depth, colour, _, _, interlace = header
    if depth != 8 or colour != 6 or interlace != 0:
        raise SystemExit('expected a non-interlaced 8-bit RGBA PNG')
    return width, height, unfilter(zlib.decompress(bytes(idat)), width, height)


def unfilter(raw, width, height):
    """Undo the per-scanline filters PNG applies (RFC 2083 section 6)."""
    stride, out, previous = width * 4, bytearray(), bytearray(width * 4)
    pos = 0
    for _ in range(height):
        kind = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for index in range(stride):
            left = line[index - 4] if index >= 4 else 0
            up = previous[index]
            upleft = previous[index - 4] if index >= 4 else 0
            if kind == 1:
                line[index] = (line[index] + left) & 0xFF
            elif kind == 2:
                line[index] = (line[index] + up) & 0xFF
            elif kind == 3:
                line[index] = (line[index] + (left + up) // 2) & 0xFF
            elif kind == 4:
                p = left + up - upleft
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - upleft)
                nearest = left if (pa <= pb and pa <= pc) else (
                    up if pb <= pc else upleft)
                line[index] = (line[index] + nearest) & 0xFF
        out += line
        previous = line
    return out


def downscale(pixels, width, height, target):
    """Box filter, averaging in premultiplied space so edges do not halo."""
    out = bytearray(target * target * 4)
    for y in range(target):
        y0, y1 = y * height // target, max((y + 1) * height // target,
                                           y * height // target + 1)
        for x in range(target):
            x0, x1 = x * width // target, max((x + 1) * width // target,
                                              x * width // target + 1)
            r = g = b = a = n = 0
            for sy in range(y0, y1):
                row = sy * width * 4
                for sx in range(x0, x1):
                    o = row + sx * 4
                    alpha = pixels[o + 3]
                    r += pixels[o] * alpha
                    g += pixels[o + 1] * alpha
                    b += pixels[o + 2] * alpha
                    a += alpha
                    n += 1
            o = (y * target + x) * 4
            if a:
                out[o], out[o + 1], out[o + 2] = r // a, g // a, b // a
            out[o + 3] = a // n
    return out


def encode(pixels, size):
    """Runs of identical RGBA, at most 255 long, five bytes each."""
    body, run, index = bytearray(), None, 0
    total = size * size
    while index < total:
        o = index * 4
        pixel = bytes(pixels[o:o + 4])
        if run and run[0] == pixel and run[1] < 255:
            run = (pixel, run[1] + 1)
        else:
            if run:
                body.append(run[1])
                body += run[0]
            run = (pixel, 1)
        index += 1
    if run:
        body.append(run[1])
        body += run[0]
    return body


def main():
    source = sys.argv[1] if len(sys.argv) > 1 else 'assets/seneri-logo.png'
    size = int(sys.argv[2]) if len(sys.argv) > 2 else 256
    width, height, pixels = read_png(source)
    scaled = downscale(pixels, width, height, size)
    body = encode(scaled, size)
    blob = bytearray(b'SRL1')
    blob += struct.pack('<HH', size, size)
    blob += body

    destination = sys.argv[3] if len(sys.argv) > 3 else 'build/logo.srl'
    with open(destination, 'wb') as handle:
        handle.write(blob)

    print(f'{source}: {width}x{height} -> {size}x{size}, '
          f'{len(body) // 5} runs, {len(blob)} bytes -> {destination}',
          file=sys.stderr)


if __name__ == '__main__':
    main()
