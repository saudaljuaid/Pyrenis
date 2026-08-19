#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Pack tools/font8x16.txt into the console font blob the kernel embeds.

The source is ASCII art - a '#' is a lit pixel - because a font is the one
asset in this kernel a person may actually want to edit by hand, and hex bytes
cannot be proofread. This turns that art into eight header bytes and one byte
per glyph row.

    python3 tools/make-font-asset.py tools/font8x16.txt build/font.snf

The result is a build artifact and is not committed; src/rust/abi.rs includes it
at compile time. The wire format is four magic bytes, then glyph width, glyph
height, first code point and glyph count as single bytes, then
count * height bytes with the most significant bit as the leftmost pixel.

Requires only the Python standard library. In particular it does not need a
font library or the original OTF: tools/font8x16.txt is the source of truth and
is committed, so a clone can build the kernel with nothing but Python.
"""
import sys

MAGIC = b'SNF1'
CELL_WIDTH = 8
CELL_HEIGHT = 16


def parse(path):
    """Read the art into {code point: [16 row strings]}, refusing anything odd."""
    glyphs, order = {}, []
    code, rows = None, []

    for number, raw in enumerate(open(path, encoding='utf-8'), 1):
        line = raw.rstrip('\n')

        if line.startswith('#') or (not line and code is None):
            continue

        if line.startswith('U+'):
            if code is not None and len(rows) != CELL_HEIGHT:
                raise SystemExit(
                    f'{path}:{number}: U+{code:04X} has {len(rows)} rows, '
                    f'expected {CELL_HEIGHT}')
            code = int(line[2:6], 16)
            if code in glyphs:
                raise SystemExit(f'{path}:{number}: U+{code:04X} appears twice')
            rows = []
            glyphs[code] = rows
            order.append(code)
            continue

        if not line:
            continue

        if set(line) - {'.', '#'}:
            raise SystemExit(
                f'{path}:{number}: a glyph row may only contain . and #')
        if len(line) != CELL_WIDTH:
            raise SystemExit(
                f'{path}:{number}: row is {len(line)} wide, expected {CELL_WIDTH}')
        if code is None:
            raise SystemExit(f'{path}:{number}: a row before any U+ line')
        if len(rows) == CELL_HEIGHT:
            raise SystemExit(f'{path}:{number}: U+{code:04X} has too many rows')
        rows.append(line)

    if code is not None and len(rows) != CELL_HEIGHT:
        raise SystemExit(f'{path}: U+{code:04X} has {len(rows)} rows')

    if not order:
        raise SystemExit(f'{path}: no glyphs')

    # The kernel indexes by subtraction, so the set has to be a dense range.
    first, count = order[0], len(order)
    expected = list(range(first, first + count))
    if order != expected:
        missing = sorted(set(expected) - set(order))
        raise SystemExit(
            f'{path}: code points must be a gapless ascending range; '
            f'first missing or out of order at U+{missing[0]:04X}'
            if missing else f'{path}: code points are out of order')
    if first > 0xFF or first + count > 0x100:
        raise SystemExit(f'{path}: code points must fit in one byte')

    return first, count, [glyphs[c] for c in order]


def pack(rows):
    """One byte per row, most significant bit leftmost."""
    out = bytearray()
    for row in rows:
        byte = 0
        for column, cell in enumerate(row):
            if cell == '#':
                byte |= 0x80 >> column
        out.append(byte)
    return out


def main():
    if len(sys.argv) != 3:
        raise SystemExit('usage: make-font-asset.py <font8x16.txt> <out.snf>')

    source, target = sys.argv[1], sys.argv[2]
    first, count, glyphs = parse(source)

    blob = bytearray(MAGIC)
    blob += bytes((CELL_WIDTH, CELL_HEIGHT, first, count))
    for rows in glyphs:
        blob += pack(rows)

    expected = 8 + count * CELL_HEIGHT
    if len(blob) != expected:
        raise SystemExit(f'internal error: {len(blob)} bytes, expected {expected}')

    open(target, 'wb').write(blob)
    print(f'{source}: {count} glyphs from U+{first:04X}, '
          f'{CELL_WIDTH}x{CELL_HEIGHT}, {len(blob)} bytes -> {target}')


if __name__ == '__main__':
    main()
