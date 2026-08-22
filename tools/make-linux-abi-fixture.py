#!/usr/bin/env python3
"""Build and independently verify the static BusyBox FAT16/NVMe fixture."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import stat
import struct
import sys


BLOCK_BYTES = 4096
TOTAL_SECTORS = 4096
IMAGE_BYTES = BLOCK_BYTES * TOTAL_SECTORS
RESERVED_SECTORS = 1
FAT_COUNT = 1
FAT_SECTORS = 2
ROOT_ENTRIES = 128
ROOT_SECTORS = 1
FIRST_FAT_SECTOR = 1
FIRST_ROOT_SECTOR = 3
FIRST_DATA_SECTOR = 4
FILE_CLUSTER = 2
FILE_BYTES = 33_584
FILE_CLUSTERS = 9
MEDIA = 0xF8
SHORT_NAME = b"BUSYBOX    "
BUSYBOX_SHA256 = "B308F2CAD5B5CD0EEB92A622DEC8D71C1A08F628A22CDC5BCDE2B98B53220746"
IMAGE_SHA256 = "41513E5D6F4C33F898F887D4F40F37149A29B1AE13B5E8A600495C18A38C7A6F"


def put_u16(image: bytearray, offset: int, value: int) -> None:
    image[offset : offset + 2] = value.to_bytes(2, "little")


def put_u32(image: bytearray, offset: int, value: int) -> None:
    image[offset : offset + 4] = value.to_bytes(4, "little")


def u16(image: bytes, offset: int) -> int:
    return int.from_bytes(image[offset : offset + 2], "little")


def u32(image: bytes, offset: int) -> int:
    return int.from_bytes(image[offset : offset + 4], "little")


def verify_busybox(binary: bytes) -> None:
    """Reject a changed binary or a changed measured ELF conjunction."""
    if len(binary) != FILE_BYTES:
        raise ValueError("BusyBox length is not exactly 33,584 bytes")
    if hashlib.sha256(binary).hexdigest().upper() != BUSYBOX_SHA256:
        raise ValueError("BusyBox SHA-256 differs from the committed executable")
    if binary[:16] != b"\x7fELF\x02\x01\x01\x00" + b"\x00" * 8:
        raise ValueError("BusyBox ELF identification is invalid")
    header = struct.unpack_from("<HHIQQQIHHHHHH", binary, 16)
    elf_type, machine, version, entry, phoff, shoff, flags = header[:7]
    ehsize, phentsize, phnum, shentsize, shnum, shstrndx = header[7:]
    if (
        elf_type,
        machine,
        version,
        entry,
        phoff,
        shoff,
        flags,
        ehsize,
        phentsize,
        phnum,
        shentsize,
        shnum,
        shstrndx,
    ) != (
        2,
        62,
        1,
        0x40000100107A,
        64,
        33072,
        0,
        64,
        56,
        5,
        64,
        8,
        7,
    ):
        raise ValueError("BusyBox ELF header differs from the measured contract")
    headers = [
        struct.unpack_from("<IIQQQQQQ", binary, phoff + index * phentsize)
        for index in range(phnum)
    ]
    expected = [
        (1, 4, 0x0, 0x400001000000, 0x400001000000, 0x158, 0x158, 0x1000),
        (1, 5, 0x1000, 0x400001001000, 0x400001001000, 0x5563, 0x5563, 0x1000),
        (1, 4, 0x7000, 0x400001007000, 0x400001007000, 0xED1, 0xED1, 0x1000),
        (1, 6, 0x8000, 0x400001008000, 0x400001008000, 0xFE, 0xB38, 0x1000),
        (0x6474E551, 6, 0, 0, 0, 0, 0, 0x10),
    ]
    if headers != expected:
        raise ValueError("BusyBox program headers differ from the measured contract")


def build_image(binary: bytes) -> bytes:
    """Construct one deterministic superfloppy around the verified binary."""
    verify_busybox(binary)
    image = bytearray(IMAGE_BYTES)
    image[0:3] = b"\xEB\x3C\x90"
    image[3:11] = b"SAPOTE  "
    put_u16(image, 11, BLOCK_BYTES)
    image[13] = 1
    put_u16(image, 14, RESERVED_SECTORS)
    image[16] = FAT_COUNT
    put_u16(image, 17, ROOT_ENTRIES)
    put_u16(image, 19, TOTAL_SECTORS)
    image[21] = MEDIA
    put_u16(image, 22, FAT_SECTORS)
    put_u16(image, 24, 1)
    put_u16(image, 26, 1)
    image[36] = 0x80
    image[38] = 0x29
    put_u32(image, 39, 0x0800_0001)
    image[43:54] = b"SAPOTE     "
    image[54:62] = b"FAT16   "
    image[510:512] = b"\x55\xAA"

    fat = FIRST_FAT_SECTOR * BLOCK_BYTES
    put_u16(image, fat, 0xFFF8)
    put_u16(image, fat + 2, 0xFFFF)
    for index in range(FILE_CLUSTERS):
        cluster = FILE_CLUSTER + index
        value = cluster + 1 if index + 1 < FILE_CLUSTERS else 0xFFFF
        put_u16(image, fat + cluster * 2, value)

    root = FIRST_ROOT_SECTOR * BLOCK_BYTES
    image[root : root + 11] = SHORT_NAME
    image[root + 11] = 0x20
    put_u16(image, root + 26, FILE_CLUSTER)
    put_u32(image, root + 28, FILE_BYTES)

    data = FIRST_DATA_SECTOR * BLOCK_BYTES
    image[data : data + FILE_BYTES] = binary
    return bytes(image)


def verify_image(image: bytes, binary: bytes) -> str:
    """Reopen and derive every extent before comparing every fixture byte."""
    if len(image) != IMAGE_BYTES or image[510:512] != b"\x55\xAA":
        raise ValueError("fixture length or boot signature is invalid")
    bytes_per_sector = u16(image, 11)
    root_entries = u16(image, 17)
    root_sectors = (root_entries * 32 + bytes_per_sector - 1) // bytes_per_sector
    first_fat = u16(image, 14)
    first_root = first_fat + image[16] * u16(image, 22)
    first_data = first_root + root_sectors
    clusters = (u16(image, 19) - first_data) // image[13]
    geometry = (
        bytes_per_sector,
        image[13],
        first_fat,
        image[16],
        root_entries,
        u16(image, 19),
        image[21],
        u32(image, 32),
        u16(image, 22),
        u32(image, 28),
        root_sectors,
        first_root,
        first_data,
        clusters,
    )
    expected = (4096, 1, 1, 1, 128, 4096, 0xF8, 0, 2, 0, 1, 3, 4, 4092)
    if geometry != expected or not (4085 <= clusters < 65525):
        raise ValueError("derived FAT16 geometry differs from the contract")
    fat = first_fat * bytes_per_sector
    if (u16(image, fat), u16(image, fat + 2)) != (0xFFF8, 0xFFFF):
        raise ValueError("FAT reserved entries are invalid")
    for index in range(FILE_CLUSTERS):
        cluster = FILE_CLUSTER + index
        expected_next = cluster + 1 if index + 1 < FILE_CLUSTERS else 0xFFFF
        if u16(image, fat + cluster * 2) != expected_next:
            raise ValueError("BusyBox FAT chain is invalid")
    root = first_root * bytes_per_sector
    root_state = (
        image[root : root + 11],
        image[root + 11],
        u16(image, root + 20),
        u16(image, root + 26),
        u32(image, root + 28),
        image[root + 32],
    )
    if root_state != (SHORT_NAME, 0x20, 0, FILE_CLUSTER, FILE_BYTES, 0):
        raise ValueError("canonical BusyBox root entry is invalid")
    body = bytearray()
    cluster = FILE_CLUSTER
    for _ in range(FILE_CLUSTERS):
        lba = first_data + cluster - 2
        body.extend(image[lba * bytes_per_sector : (lba + 1) * bytes_per_sector])
        next_cluster = u16(image, fat + cluster * 2)
        cluster = next_cluster
    if cluster < 0xFFF8 or bytes(body[:FILE_BYTES]) != binary:
        raise ValueError("multi-cluster BusyBox content is invalid")
    if any(body[FILE_BYTES:]):
        raise ValueError("unused bytes after BusyBox are not zero")
    rebuilt = build_image(binary)
    if image != rebuilt:
        raise ValueError("fixture contains an unexpected byte")
    digest = hashlib.sha256(image).hexdigest().upper()
    if digest != IMAGE_SHA256:
        raise ValueError("fixture image SHA-256 differs from the committed value")
    return digest


def checked_file(argument: str, output: bool) -> Path:
    """Confine both ordinary files below this repository's build tree."""
    repository = Path(__file__).resolve().parents[1]
    build = (repository / "build").resolve()
    path = Path(argument)
    if not path.is_absolute():
        path = repository / path
    resolved = path.resolve(strict=False)
    if build != resolved and build not in resolved.parents:
        raise ValueError("fixture files must remain under the repository build directory")
    if path.exists():
        mode = path.lstat().st_mode
        if stat.S_ISLNK(mode) or not stat.S_ISREG(mode):
            raise ValueError("fixture files must be ordinary non-symlink files")
    elif not output:
        raise ValueError("BusyBox input does not exist")
    if output:
        path.parent.mkdir(parents=True, exist_ok=True)
    return path


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {Path(sys.argv[0]).name} BUSYBOX OUTPUT", file=sys.stderr)
        return 2
    output = None
    try:
        source = checked_file(sys.argv[1], output=False)
        output = checked_file(sys.argv[2], output=True)
        binary = source.read_bytes()
        image = build_image(binary)
        with output.open("wb") as stream:
            stream.write(image)
            stream.flush()
            os.fsync(stream.fileno())
        digest = verify_image(output.read_bytes(), binary)
    except (OSError, ValueError, struct.error) as error:
        if output is not None:
            try:
                output.unlink(missing_ok=True)
            except OSError:
                pass
        print(f"Linux ABI fixture refused: {error}", file=sys.stderr)
        return 1
    print(
        f"{output}: {TOTAL_SECTORS} sectors x {BLOCK_BYTES} bytes, "
        f"BUSYBOX {FILE_BYTES} bytes in {FILE_CLUSTERS} clusters, "
        f"binary SHA-256 {BUSYBOX_SHA256}, image SHA-256 {digest}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
