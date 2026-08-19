# Cached pixels

The framebuffer is device memory. Seneri maps it uncacheable, so reading a
pixel back during a scroll can be far more expensive on a real processor than
reading the same pixel from ordinary RAM. QEMU TCG does not model that cache
difference. The surface keeps the working picture in write-back heap memory and
uses the framebuffer only as the final destination.

At the boot mode used by the tests, the surface is 1024 x 768 x 4 bytes:
3,145,728 bytes. That is one allocation from the 16 MiB guarded heap and one
entry in its 256-entry block table. Pitch is 4,096 bytes because heap surfaces
are tightly packed; external sources passed to `surface_blit` keep their own
pitch.

## Invariants

- A live surface has non-zero dimensions, a four-byte-aligned pitch large
  enough for one row, and one heap allocation covering every row.
- Rectangle addition is checked before clipping. A coordinate plus a size that
  would wrap is refused as `rectangle arithmetic overflowed` rather than being
  folded back into the surface.
- A rectangle may cross the right or bottom edge and is clipped there. Its
  start must still be inside the surface. Pixel reads and writes outside the
  surface are refused by name.
- `surface_blit` reads rows using the source pitch supplied by its caller.
  Destination pitch never changes how source bytes are found.
- `surface_copy_rect` behaves like a two-dimensional `memmove`: a destination
  below its source walks bottom-up, and a same-row destination to the right
  walks right-to-left. The opposite overlaps walk forward.
- Every successful write unions its clipped extent into one bounding damage
  rectangle. An empty present copies nothing. A non-empty present copies only
  that rectangle and clears damage only after every volatile framebuffer store
  has completed.
- A present requires an active framebuffer with exactly the same width and
  height. Framebuffer pitch remains independent and is used when finding each
  device row.
- The screen verifier is not a surface verifier. `screen_verify_cell` reads the
  framebuffer after present, so it checks what reached the glass rather than
  asking the cached picture whether it agrees with itself.

The public statuses name null arguments, repeated or missing initialization,
bad geometry, size overflow, allocation and release failure, out-of-bounds
coordinates, rectangle overflow, bad source pitch, absent or mismatched
framebuffers, present failure, and validation failure. The status-string table
is statically sized against the final enumerator.

## Proofs on every boot

`surface_self_test` binds synthetic padded storage before hardware discovery.
It checks fill clipping and padding sentinels, pixel bounds, a padded blit
source, both vertical overlap directions, and the named refusals. The real
scenario below adds the damage-union check after a framebuffer is available.

After framebuffer bring-up, `prove_surface` allocates a real full-size surface
and uses `cpu_read_tsc()` around a full fill and present, a 16-pixel-high line
update and present, and a cached scroll plus present. It reads selected pixels
back from the framebuffer and requires the copied-pixel counters to be 786,432,
16,384, and 786,432 respectively. A normal boot prints four lines of this form:

```text
Seneri OS: surface 1024x768 pitch 4096 buffer 3145728 bytes
Seneri OS: surface cycles full present <cycles> one-line update <cycles> scroll <cycles>
Seneri OS: surface copied 786432 full, 16384 line, 786432 scroll pixels
Seneri OS: cached surface established
```

The dedicated `surface` QEMU scenario uses exit value `0x2B`. It repeats the
real framebuffer checks for a full present, one pixel, one line, edge clipping,
padded-source blitting, both overlap directions, and damage union. Its compact
diagnostic is:

```text
ST SURFACE full 786432 line 16384 clipped 4 overlap both damage 20
```

## Cycle measurements

Each number below is the median of five boots; the range is in parentheses.
The before kernel is the branch head before this increment with measurement
lines added around the existing operations. The after kernel is the final row-
copy implementation. Both use `cpu_read_tsc()`. A full operation touches
786,432 visible pixels, a line touches 16,384, and a scroll moves or presents
the full 1024 x 768 picture.

| Executor | Version and host | Kernel | Full | One line | Scroll |
| --- | --- | --- | ---: | ---: | ---: |
| TCG | QEMU 8.2.2, Ubuntu 24.04 guest | before | 4,188,332 (2,522,036-7,281,872) | 1,546,274 (994,080-4,130,736) | 7,175,186 (5,155,652-10,716,026) |
| TCG | QEMU 8.2.2, Ubuntu 24.04 guest | after | 138,485,405 (90,897,978-155,415,347) | 2,509,572 (1,644,452-3,704,425) | 108,929,404 (86,505,990-154,145,190) |
| WHPX | QEMU 11.1.0, Intel Core i7-1255U host | before | 165,275,132 (105,020,176-176,827,260) | 3,300,498 (2,135,962-4,308,710) | 467,504,850 (437,159,604-564,983,710) |
| WHPX | QEMU 11.1.0, Intel Core i7-1255U host | after | 187,360,826 (130,867,630-204,592,498) | 3,286,286 (2,651,840-3,355,016) | 163,780,368 (125,221,130-171,109,064) |

TCG made the final scroll 1,418% slower. That is a result, not a speedup: TCG
models no useful cache distinction, so it charges for the extra cached copy and
gives no reward for avoiding uncacheable reads. WHPX reduced median scroll cost
by 65.0%. Its full operation was 13.4% slower because the after measurement
fills cached RAM and must still present every pixel; the one-line result was
0.4% faster while copying only its required 16,384 pixels.

WHPX executes through the host processor, but it is neither KVM nor bare metal.
KVM was attempted inside the available Ubuntu environment and could not start:
`/dev/kvm` was absent and QEMU reported `Could not access KVM kernel module`.
No bootable real-hardware target was available. Therefore the WHPX scroll
result is cache-aware virtual-hardware evidence, not a claim that KVM or bare
metal was measured.

## Flake sweep

Twenty complete, separate invocations of `make qemu-tests` passed under QEMU
8.2.2 TCG: 20/20 runs and 540/540 scenario boots. Outer run time ranged from 56
to 124 seconds. Every log ended in `all deterministic QEMU scenarios passed`;
none contained a scenario failure, `ST FAIL`, or kernel panic. The measured TSC
numbers vary by design, but no scenario changed its exit result or required
diagnostic.

## Deliberate controls

Each source was copied to a snapshot before mutation and restored from that
copy afterward. Controls ran under QEMU 8.2.2 TCG. KVM controls could not run
for the reason above; WHPX was kept as a separately labelled timing executor
rather than renamed as KVM.

| Deliberate break | Required result | Observed result |
| --- | --- | --- |
| Offset the presented destination by one row. | Fail | Boot panicked that the one-line update did not reach the framebuffer. |
| Return from damage tracking without marking anything. | Fail | The synthetic surface self-test panicked before hardware discovery. |
| Paste a damaged source rectangle at framebuffer origin. | Fail | The full present hid the error; the mid-screen one-line proof panicked. |
| Clip one pixel past the right edge. | Fail | The synthetic clipping and padding-sentinel test panicked. |
| Make only downward overlap copy forward. | Fail | The synthetic downward-overlap test panicked. |
| Make only upward overlap copy backward. | Fail | The synthetic upward-overlap test panicked. |
| Read a blit source using destination pitch. | Fail | The synthetic padded-source test panicked. |
| Point `screen_verify_cell` at the back buffer. | **Pass** | After a forced clean rebuild, the complete `screen` scenario passed. This proves the change weakens the oracle without producing a red test. |

The first attempt at the last control was discarded: a restored source file had
an older timestamp than the preceding broken object, so Make reused that object
and failed in the wrong control. A clean rebuild compiled the intended single
mutation and produced the required pass. No result from the stale object is
counted above.

## Deferred work

- More than one damage rectangle may reduce copying when two small changes are
  far apart; one union rectangle is deliberately simpler for this increment.
- A cursor, compositor, windows, colour attributes, font scaling, mouse input,
  and z-order remain separate layers.
- The screen cursor and surface damage are shared state without a lock. Local
  glyph scratch prevents torn scratch data, but concurrent writers still need a
  later serialization design.
- KVM and bare-metal cycle measurements remain open evidence gaps. In
  particular, the uncacheable framebuffer mapping has not been validated on a
  physical display controller here.
