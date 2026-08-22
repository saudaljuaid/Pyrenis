<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Static BusyBox proof provenance and reproducible build

This document freezes the userspace input to the first Linux ABI increment.
BusyBox and musl remain separate userspace works; neither is copied into or
linked with the GPL-3.0-only Sapote kernel.  The executable is built from an
unmodified BusyBox source tree with the committed configuration.

## Pinned inputs

| input | source | SHA-256 |
| --- | --- | --- |
| BusyBox 1.38.0 source | `https://busybox.net/downloads/busybox-1.38.0.tar.bz2` | `34F9EA6FF8636F2C9241153B9114EEFA9E65674A45318AE1EF95BB5F31C53BB2` |
| musl 1.2.6 source | `https://musl.libc.org/releases/musl-1.2.6.tar.gz` | `D585FD3B613C66151FC3249E8ED44F77020CB5E6C1E635A616D3F9F82460512A` |
| byte-identical musl mirror used by CI | `https://sources.buildroot.net/musl/musl-1.2.6.tar.gz` | `D585FD3B613C66151FC3249E8ED44F77020CB5E6C1E635A616D3F9F82460512A` |
| committed BusyBox configuration | `userspace/busybox/busybox.config` | `3FBC0403C6A4865FC4397240961C367EE9B36D6D350CC6CEB2D22CBBBEA28480` |
| BusyBox `LICENSE` | source archive | `BBFC9843646D483C334664F651C208B9839626891D8F17604DB2146962F43548` |
| musl `COPYRIGHT` | source archive | `B870108EC5E7790E9F9919064F1B9421D62D5F9B0E6C230C6ADF7EA2DA62E97B` |

The recorded builder is Ubuntu 24.04, GCC
`13.3.0-6ubuntu2~24.04.1`, GNU binutils 2.42, and the musl 1.2.6
`musl-gcc` wrapper built by `tools/build-busybox-proof.sh`.  The script rejects
changed input, configuration, output, ELF shape, dynamic dependencies,
relocations, and W+X load segments.  `KCONFIG_NOTIMESTAMP=1` removes the
configuration timestamp.  Two independent clean source and toolchain builds
must compare byte-for-byte.

The generated musl wrapper normally selects position-independent `Scrt1.o` for
all non-shared links.  The build script deterministically selects musl's own
installed `crt1.o` instead because this executable is deliberately non-PIE
`ET_EXEC`.  The musl source tree is unchanged; the installed wrapper choice is
recorded and checked before BusyBox is linked.  The fixed high link also
disables linker relaxation and gives crt1's ignored weak `_DYNAMIC` probe a
nearby absolute definition; no dynamic header, relocation, dependency, or
loader is created, and the structural checks refuse any such output.  Both
musl and BusyBox use the x86-64 large code model so every fixed high-user
address is representable.  The wrapper also omits GCC's unused constructor CRT
bookends; musl's own `crti.o`/`crtn.o` remain, while BusyBox constructors and
dynamic initialization stay disabled by configuration and contract.

## Frozen executable

The result is a stripped, static, position-fixed x86-64 `ET_EXEC` file:

| property | measured value |
| --- | --- |
| byte length | 33,584 |
| SHA-256 | `B308F2CAD5B5CD0EEB92A622DEC8D71C1A08F628A22CDC5BCDE2B98B53220746` |
| entry | `0x40000100107a` |
| FAT16 data clusters at 4096 bytes | 9 |
| program headers / `PT_LOAD` | 5 / 4 |
| interpreter, dynamic section, runtime relocations | none |

The four load segments are `R` at `0x400001000000`, `RX` at
`0x400001001000`, `R` at `0x400001007000`, and `RW/NX` at
`0x400001008000`.  Their file/memory extents are `0x158/0x158`,
`0x5563/0x5563`, `0xed1/0xed1`, and `0xfe/0xb38`.  The only non-load header is
non-executable `PT_GNU_STACK`.  There is no `PT_GNU_RELRO` or `PT_TLS`;
musl's small builtin thread area resides in the final data/BSS load segment.

The configuration enables only the BusyBox multicall core, static build
support, stack buffers, `echo`, and the `SH_IS_NONE`/`BASH_IS_NONE` selectors.
Fancy echo, shells, usage text, dynamic linking, PIE, and every other applet are
disabled.  The complete configuration is the authority; a shorter seed is not
used to reconstruct it.

## License and release record

BusyBox is GPL-2.0-or-later and musl is MIT licensed.  Every release that
distributes the executable must also distribute the exact BusyBox source
archive, complete configuration, BusyBox license, musl source/toolchain record,
musl copyright notice, build script, and checksum manifest.  The release must
identify these as userspace/source assets, not Sapote kernel code.  A binary is
not published unless all of those records are present and their hashes match.
