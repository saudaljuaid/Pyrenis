# The keyboard

This is the first device Seneri talks to that a person operates. Everything
before it was discovered, counted, or timed. This one waits.

## What the 8042 actually is

It is not a keyboard, and on anything built this century it is not a chip
either. It is a controller with two device ports, a command register at `0x64`
and a data register at `0x60`, and on modern hardware it is emulated by the
platform controller hub or by a system management handler running behind the
scenes.

Three consequences shape this driver, and none of them are visible in a register
list.

**Its timings bear no relation to any datasheet.** A driver that waits for it
without a bound is a driver that can hang the machine on a platform that decided
not to answer. Every wait here counts down from `KEYBOARD_WAIT_LIMIT`, and
reaching the bound is `the 8042 controller did not answer within its bound`
rather than a spin.

**Its two ready bits point opposite ways.** Status bit 0 set means there is a
byte to read. Status bit 1 set means the input buffer is *still full* and a write
would be lost. Reading `0x60` when bit 0 is clear returns stale data rather than
failing, which is why nothing here reads without checking first.

**Scancodes are not characters.** Set 1 numbers keys by *position*, so the table
in `src/kernel/keyboard.c` maps positions on a US layout. A different layout is a
different table and nothing else changes.

## What it claims

- **Translation is enabled at the controller**, so it hands back set 1 whatever
  set the keyboard itself is using. Without that, a keyboard defaulting to set 2
  produces plausible nonsense rather than an obvious failure.
- **Caps lock is not a second shift.** It applies to letters and nothing else, so
  it is resolved *after* the table lookup rather than by choosing a different
  table: shift and caps together on a letter cancel, and caps lock on a digit
  does nothing.
- **Both edges are reported.** Presses and releases both become events, because a
  shell needs characters but anything that ever draws a cursor or handles a held
  key needs the edges.
- **The queue is bounded and overflow is counted.** Sixty-four slots, sixty-three
  usable, and what does not fit increments `dropped` rather than overwriting what
  is waiting. A full queue is visible, never silent.
- **Bring-up refuses to run with interrupts on.** The controller is configured
  across half a dozen port writes; an interrupt arriving in the middle would
  reach a handler that is not installed yet. The driver refuses rather than
  disabling interrupts behind its caller's back.
- **The order of the last three steps is load-bearing.** Handler installed, then
  the I/O APIC routed, then the port enabled. Every other order has a window in
  which a keystroke reaches an unrouted vector.

## Proving it without a person

Every other device Seneri brings up either announces itself or can be asked a
question. A keyboard does neither — it says nothing until somebody presses a key,
and boot cannot wait for that.

The way through is a real controller command rather than a test hook. **8042
command `0xD2` writes a byte into the output buffer exactly as though the
keyboard had sent it**, which raises IRQ 1. The entire path runs for real —
controller, I/O APIC routing, vector, handler, decode, queue — and the only thing
simulated is the finger.

Boot injects five scancodes: `h`, `i`, left shift down, `i`, left shift up, and
requires `"hiI"` to come out. The capital is the point: it is only correct if the
modifier state was carried across three separate interrupts.

    Seneri OS: keyboard 8042 online, IRQ 1 routed, 5 interrupts for 5 events
    Seneri OS: keyboard decoded "hiI" from injected scancodes

## Deliberate breakage

| Control | Result |
| --- | --- |
| Never route IRQ 1 in the I/O APIC. | `PANIC: the keyboard delivered no interrupt` |
| Route it, but never set the interrupt-enable bit in the controller's configuration byte. | Same panic. Two independent things have to be right and each is checked by its absence. |
| Stop stripping the release bit before the table lookup. | `PANIC: keyboard translation self-test failed`, before the controller is touched at all. |
| Let a full queue overwrite the oldest event instead of counting the drop. | `ST FAIL keyboard: a flooded keyboard queue dropped nothing` |

One more result is worth recording because it was a bug in the *test* rather than
the driver. The overflow check originally toggled interrupts around each
injection with `cpu_interrupt_enable()` immediately followed by
`cpu_interrupt_disable()`, and nothing was ever delivered: `sti` does not take
effect until after the instruction following it, so that pair leaves a window of
exactly zero instructions. Interrupts now stay on for the whole flood.

## Proved where

`prove_keyboard` in `src/kernel/boot_proofs.c` runs on every boot.

The `keyboard` scenario in `src/kernel/test.c` does what boot cannot. Boot needs
the keyboard afterwards, so it can neither fill the queue nor latch caps lock and
leave the machine in a state nothing asked for. The scenario floods 200 events
into a 63-slot queue and requires `queued + dropped` to account for all 200,
toggles caps lock twice and checks it capitalises a letter and not a digit,
refuses a second bring-up, and refuses a read through a null pointer.

    make qemu-test-keyboard      # exit value 0x29

Boot's proof reports 5 interrupts for 5 events; the scenario reports
`queued 63 and dropped 137 of a 200 event flood`.

## Deferred work

- **Nothing reads it yet.** The events go into a queue and boot drains it. There
  is no shell, no line editor and no echo to the screen, so a key pressed after
  boot is decoded, queued, and eventually dropped.
- **`keyboard_read` never blocks.** It returns `no key event is waiting` rather
  than sleeping, because there is no way yet for an interrupt to wake a thread.
  That is the next thing this layer needs and it is a scheduler change, not a
  driver one.
- **Extended codes are flattened.** A `0xE0` prefix is counted and then dropped,
  so keypad enter arrives as return and the arrow keys arrive as whatever they
  share a number with. Correct for a US layout typing ASCII; wrong the moment
  anything wants an arrow key.
- **No LEDs.** Caps lock is tracked in software and the light on the keyboard
  does not follow it.
- **US layout only**, and the layout is a compile-time table.
- **The queue is a single-reader, single-writer ring with interrupts masked
  around the read.** That is correct on one processor and becomes a lock on two.
