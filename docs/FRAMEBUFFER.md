# The framebuffer

Every pixel on the screen, addressable. This is the lowest layer of anything
graphical and it deliberately stops there: it can put a colour at a coordinate
and read it back, and it has no idea what a character, a window or a cursor is.

## Why this is where a project like this goes wrong

`CONTRIBUTING.md` says screenshots are not proof. Nowhere else in Seneri is that
as tempting as here, because **a framebuffer looks right long before it is
right**. A pitch mistaken for a width produces a picture that slants — and it is
still a picture. An off-by-one in the height writes past the mapping. Two
coordinates that resolve to one address lose a pixel nobody will notice.

So the claim this layer makes is narrow and completely checkable without looking
at anything: for every visible coordinate, the colour written there is the
colour read back, and no two coordinates share an address. Normal boot writes a
pattern whose colour is a function of the coordinates and reads **all 786,432
pixels** back.

## What the loader gives, and what is refused

The Multiboot2 header asks for 1024x768 at 32 bits, tagged **optional**. A
loader that cannot set a graphics mode still boots this kernel, which then
reports no framebuffer and stays on the serial console it has used since day
one — the same shape as a machine that declares no MCFG, and for the same
reason: there is a complete, tested fallback.

Multiboot2 3.1.10 makes the request a preference, not a contract. The loader may
answer with whatever mode it actually has, so **every number is read back rather
than assumed**, and each one that could make an address wrong is refused by its
own name:

- Only direct RGB is accepted. An indexed framebuffer needs a palette this
  kernel does not read and EGA text is not a framebuffer at all; both are
  refused rather than treated as the thing they resemble.
- The depth must be 32 bits, and the tag must be long enough to hold the fields
  about to be read from it.
- Width and height must be non-zero.
- **The pitch must cover a row**, and be a whole number of pixels. The pitch is
  the distance between rows and may exceed the visible width; it can never be
  less, because row *n* would then overlap row *n-1*.
- The address must be non-zero and pixel-aligned, and the whole span must lie
  inside the early identity map — a framebuffer above 4 GiB is something this
  kernel cannot reach, and saying so beats faulting at a plausible address.
- Each colour channel must be a whole byte on a byte boundary, and no two
  channels may overlap.
- Two framebuffer tags are refused: they cannot both be the screen.

## It is device memory

Configuration space and APIC registers are mapped uncacheable, and so is this,
for the same reason. `paging.c` owns the decision because `paging.c` owns the
address space: it rounds the framebuffer out to whole 2 MiB regions and carves
them out of the identity map as 4 KiB pages with `PAGING_WRITE |
PAGING_UNCACHED`, exactly as it already does for the local APIC, the I/O APICs,
the VGA buffer and the configuration window.

The framebuffer is the first device window that is **several regions wide** —
1024x768x32 is 3 MiB — which is why `PAGING_MAX_FRAMEBUFFER_REGIONS` exists. A
mode larger than that bound gets no framebuffer rather than a partly mapped one:
half a picture in device memory and half in write-back would draw correctly and
be wrong.

`framebuffer_verify` re-derives this from the page tables at the end of boot,
every page of it rather than a sample.

## Executable proof

`framebuffer_self_test` runs before anything is drawn, over geometry no machine
here produces: row offsets for a **padded** pitch, the case that shears a
picture and that the tested target cannot reach; the packer driven with two
different channel orders, so a packer that ignored the loader's positions fails;
and every bound and refusal on both axes.

`boot_parser_self_test` gained a mutable framebuffer fixture that drives every
refusal listed above, one broken field at a time. It exists because deleting the
pitch check left the whole suite green — see the controls below.

Normal boot reports:

```text
Seneri OS: framebuffer 1024x768 at 0x00000000FD000000 pitch 4096 RGB 16/8/0
Seneri OS: framebuffer verified 786432 pixels
Seneri OS: framebuffer established
```

The `framebuffer` scenario adds the check normal boot cannot make. Boot proves
that what `framebuffer_write_pixel` writes, `framebuffer_read_pixel` reads —
which stays true if both agree on the *wrong* address. The scenario computes the
physical address of sixteen coordinates from the loader's own pitch, reads them
through a raw volatile pointer sharing no code with `framebuffer.c`, and
requires the two addressings to agree. It is the argument the two PCI
configuration mechanisms make, one layer up.

### Negative controls

Each applied to a clean tree, rebuilt, run, and reverted.

| Breakage | Observed failure |
| --- | --- |
| the row stride is taken as the width, not the pitch | `PANIC: framebuffer geometry self-test failed` |
| the framebuffer is mapped write-back instead of uncacheable | `ST FAIL framebuffer: framebuffer does not match the address space` |
| the pitch is not required to cover a row | **passed — see below**, then `PANIC: Multiboot2 parser self-test failed` |
| an indexed framebuffer is accepted as direct colour | `PANIC: Multiboot2 parser self-test failed` |
| the framebuffer is not cleared when a context is reset | `PANIC: Multiboot2 parser self-test failed` |
| the readback proof covers half the screen | `PANIC: the framebuffer proof skipped part of the picture` |
| a normal-boot contract line is renamed | `normal scenario did not complete the integrated production path` |

Two of these are the point of the exercise.

**Deleting the pitch check changed nothing, and that was the finding.** QEMU
reports a pitch of exactly 4096 for a 1024-pixel row — precisely a row, with no
padding — so the "pitch must cover a row" check is **unreachable from this
machine**. Every rejection in the parser was in the same position: the tested
target reports a depth of exactly 32 and three byte-aligned channels, so nothing
here could ever drive them. The response was not to accept the non-result but to
build a fixture that can: `boot_parser_self_test` now breaks one framebuffer
field at a time across seventeen cases, and the control fails as shown.

**The clearing bug was real, and the new fixture found it immediately.** The
parser resets every field of a context before parsing, and the framebuffer was
not on that list. Nothing on a real boot noticed, because a real boot parses
once. The moment the self-test parsed twice into the same context, the second
parse saw a framebuffer left over from the first and reported a duplicate that
was not there. The fix is one more field in the reset; the value is that the
test which found it did not exist an hour earlier, which is the argument for
writing it.

## Deferred work

- **There is no text.** Nothing here knows what a character is. A font, a
  console and a cursor are the next increment, and a font can arrive as a
  Multiboot2 module — the parser already walks module tags.
- **No double buffering.** Every write goes straight to the screen, so anything
  animated will tear. That needs a back buffer in the heap and a blit, which
  needs the blit to be fast, which needs the next item.
- **The window is uncacheable, not write-combining.** Uncacheable is correct and
  slow: every store is a bus cycle. Write-combining would batch them, and needs
  a page attribute table entry set up for it — a change to `paging.c`'s memory
  types, which deserves its own increment and its own proof.
- **The VGA text console still writes to 0xB8000.** Harmless once the loader has
  set a graphics mode, because nothing displays it, but it is now dead output.
  Making `console.c` choose between the two belongs with the increment that can
  actually draw a character.
- **Nothing resizes or re-modes.** The mode is whatever the loader set, for the
  life of the boot. Changing it means talking to the display hardware, which
  means a driver, which means PCI enumeration turning into device ownership.
- **The picture is not cleared on the way out.** Boot leaves its proof pattern
  on the screen, which is the only reason there is anything to look at.
- **Verified under QEMU only**, with the `std` VGA adapter, at one mode.
