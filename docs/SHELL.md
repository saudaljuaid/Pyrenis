# The shell

This is the first thing in Seneri that exists to be *operated* rather than to be
correct. Everything under it refuses, verifies and panics, because a wrong
answer from a page table is worse than a stopped machine. A shell is the
opposite: it is driven by somebody who will make mistakes, so an unknown command
is a line of output and never an incident. **Nothing in this layer panics.**

## The shape, and why

The part that parses and dispatches knows nothing about a keyboard, and the part
that reads keys does nothing else. `shell_feed` takes one character from
anywhere — a key, a boot proof, a scenario — and `shell_run` is a loop that
supplies them.

That split is the only reason boot can prove this. **A shell tested by pretending
to type is a shell whose parser was never separated from its input, and the
pretending is the part that rots.**

## What it claims

- **A command name matches exactly.** `hel` does not run `help`, `helpful` does
  not run `help`, and `echoes` does not run `echo`. A name has to end where the
  line does or at a separator. This is the classic way a hand-written dispatcher
  goes wrong and it is checked in both directions.
- **A line at the limit is refused at the keystroke that would overflow it**,
  not truncated when it runs. A truncated command is a different command.
- **Backspace erases.** The sequence is back, space, back — three characters,
  because a lone backspace moves the cursor and leaves the character where it
  was. See *Deliberate breakage*, which is where this rule came from.
- **Nothing unprintable enters a line.** If the font cannot draw it, it is not
  buffered and not echoed.
- **An empty line is not a mistake.** It runs nothing, reports nothing, and
  counts as a line.
- **The interactive loop halts rather than spins.** `cpu_enable_and_halt` is
  `sti` followed by `hlt`, and the pairing is the point: `sti` does not take
  effect until after the instruction following it, so no interrupt can arrive in
  the gap between enabling and halting. That gap is exactly the race that would
  leave the machine asleep with a keystroke already waiting.

## Commands

    help      this list
    echo      print the rest of the line
    clear     clear the screen
    uptime    nanoseconds since the clock started
    mem       physical frames and kernel heap
    pci       every function enumeration found
    keys      keyboard counters
    threads   scheduler counters
    version   what this is

Each one reads live kernel state through the same interface anything else would.
None of them are stubs.

## Where it runs

Boot ends with `shell_run()` instead of `console_halt()` — but only when no test
scenario was selected. A scenario has to finish and a shell does not, so every
scenario leaves through `kernel_test_complete_normal` above that line. In
practice: `make run` gives you a prompt, `make qemu-tests` never reaches one.

## Proved end to end

This is the first proof in Seneri that runs the whole chain at once, and it is
worth doing that way because **the chain is the product**. A keystroke that
decodes perfectly and never reaches a command is worthless, and a command that
runs while nothing appears on screen is worthless.

`prove_shell` pushes the scancodes for `echo hi` through the 8042's own `0xD2`
command. They are taken by IRQ 1, decoded by the keyboard driver, drained out of
its queue, fed to the shell one character at a time exactly as `shell_run` does,
executed, written to the console, drawn by the screen console — and then **read
back out of the framebuffer, pixel by pixel, and compared against the font**.

Nothing in that sentence is simulated except the finger.

    echo hi
    hi
    seneri>
    Seneri OS: shell ran "echo hi" from 8 injected scancodes
    Seneri OS: shell output verified on screen

The three lines checked on the glass are the echo at row 0, the command's output
at row 1, and the prompt at row 2. The screen is cleared first, so every cell is
at a known place.

## Deliberate breakage

| Control | Result |
| --- | --- |
| Match on prefix only, so `e` would run `echo`. | `PANIC: shell line and dispatch self-test failed` |
| Echo the character but never buffer it. | Same panic — the line editor's own state is checked, not just the screen. |
| Buffer the character but never echo it. | `PANIC: the shell did not echo what was typed`, caught by reading the framebuffer. |
| Accept a line one character past the limit. | Same self-test panic. |
| **Reduce the erase to a bare backspace, so the cursor moves and the character stays.** | **Passed.** See below. |
| The same reduction, after the gap it exposed was closed. | `ST FAIL shell: backspace did not erase the character on screen` |
| Remove the screen console's backspace handling entirely. | Same failure. |

The fifth row is the one that mattered. **Nothing checked the screen after an
erase**, so removing two thirds of the erase sequence changed no test — and
chasing that found a real bug underneath it: `screen_putc` did not handle `'\b'`
at all. Backspace is 0x08, below the font's first code point, so it fell through
to the drawing path and was rendered as the replacement character. **Every
correction a person made would have left a `?` on the screen.**

Two things were wrong and one control found both: a missing check, and the bug
the missing check was hiding. That is the second time in this project a control
that failed to fail has been worth more than the ones that fired.

## Proved where

`prove_shell` in `src/kernel/boot_proofs.c` runs on every boot.

The `shell` scenario in `src/kernel/test.c` does what boot cannot. Boot has to
keep its transcript, so it cannot run `clear` or the commands that print pages;
nothing after the scenario needs a console. It runs **every built-in command**,
checks the dispatcher in both directions, types a line with a correction in it
and requires the correction to have taken, verifies the erase on the glass, and
confirms an unknown command is reported and counted without stopping anything.

    make qemu-test-shell         # exit value 0x2A

## Deferred work

- **No history, no arrow keys, no tab completion.** The keyboard driver flattens
  extended scancodes, so the arrows arrive as whatever they share a number with;
  history needs them first.
- **No arguments beyond the raw remainder of the line.** `echo` prints what
  follows it. Nothing tokenises, so nothing takes flags.
- **No pipes, no redirection, no programs.** Every command is a C function
  compiled into the kernel, because there is no user mode to run anything else
  in and no filesystem to load it from.
- **The line is a fixed 128 bytes** in `.bss`, not on the heap. It is one shell
  on one console; the day there are two, this is per-session state.
- **`shell_run` polls a queue it cannot wait on.** It halts until any interrupt
  and then looks, because there is still no way for an interrupt to wake a
  specific thread. That is the scheduler change `docs/KEYBOARD.md` names, and it
  is the last thing standing between this and a shell that genuinely sleeps.
