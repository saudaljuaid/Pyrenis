# Working on Seneri

This is for the person who owns this repository, working on it alone.

Everything else in `docs/` explains what a layer *is*. This explains how to
change one and get the change onto `main` without anyone standing behind you.
It assumes nothing except that you can open a terminal in the repository.

## Once, on a machine that has never built this

    sudo apt-get install binutils gcc grub-common grub-pc-bin make mtools \
        qemu-system-x86 xorriso
    rustup target add x86_64-unknown-none
    make hooks

`make hooks` is the important one and it takes a second. It points git at
`.githooks/`, and from then on this repository refuses commits and pushes that
do not pass. You only ever run it once per clone — but a fresh clone has *not*
run it, so run it again there.

Check it took:

    git config core.hooksPath      # must print .githooks

## The loop

Five commands, in this order, every time.

    make verify        #  8 s from clean.  Build, link, and inspect the image.
    make smoke         #  2 s.  Boot the kernel in QEMU and require the transcript.
    make qemu-tests    # 29 s.  All twenty-three scenarios.
    git commit         # the pre-commit hook runs make verify again
    git push           # the pre-push hook runs make qemu-tests again

You do not have to remember which of `smoke` and `qemu-tests` your change needs,
because `git push` runs `qemu-tests` regardless. The reason to run them by hand
first is speed: finding out in eight seconds is better than finding out ninety
seconds into a push you thought was finished.

**Run `make verify` before you believe anything.** It is the cheapest thing in
this list and it catches the largest class of mistakes: a warning (this build
treats every warning as an error), an undefined symbol, a global offset table, a
writable-and-executable segment, a section the linker script does not recognise.

## Starting a change

    git fetch origin
    git switch -c seneri-os-<what-it-does> origin/main

Branch names describe the change, not who made it. `seneri-os-pci-enumeration`,
`seneri-os-pit-retirement`, `seneri-os-monotonic-clock`. Never a tool's name and
never a person's.

Always branch from `origin/main`, never from whatever your local `main` happens
to be. A stale local `main` has already cost this project a day: a baseline was
measured against a commit from before the rebrand, everything passed, and the
result meant nothing. `git fetch origin` first, every time.

## Writing the commit

Subject line: a subsystem, a colon, and what the commit does in the imperative.

    acpi: discover the memory-mapped configuration window
    thread: take the processor back
    pci: enumerate the configuration space through both mechanisms

Then a blank line, then the body — and the body is where this project is
different from most. It is not a summary of the diff; git already has the diff.
It is the **reasoning that is not visible in the code**: what you assumed, what
the hardware specification actually says, what you tried that did not work, and
what you deliberately did not do. Every non-obvious line in this kernel exists
because someone wrote down why in a commit message, and the next person found it
with `git log -S`.

One commit does one thing. If you cannot describe it in a subject line without
the word "and", it is two commits.

## Pushing, and the pull request

    git push -u origin seneri-os-<what-it-does>

Then open a pull request against `main` on GitHub. **You cannot push to `main`
directly and you should not want to** — the branch is protected, and the point of
the protection is that `build-and-boot` gets to run against your change before it
becomes the thing everyone builds from.

The pull request needs three sections. `docs/DEBT.md` and the open pull requests
are worked examples, but the shape is:

- **Change** — what each commit does, in order, and why in that order.
- **Evidence** — what you ran and what it printed. Not "tests pass". The numbers.
- **Risk** — what could go wrong that your tests would not catch. This section is
  the one that makes a review possible. If you cannot think of anything, you have
  not thought about it for long enough; "verified under QEMU only" is true of
  everything in this repository and belongs in every one of these sections.

Wait for the green check. Do not merge around a red one, a skipped one, or one
that ran against an older commit than the branch's current head.

## When it refuses you

| What you see | What it means |
| --- | --- |
| `error: ... [-Werror=...]` | A warning. This build has no warnings, only errors. Fix it; do not silence it. |
| `the kernel gained a global offset table` | Something in the change needs relocations at load time — usually a Rust panic path that pulled in `core`'s formatting machinery. Find the index or arithmetic that can now fail. |
| `a gap separates kernel data and bss` | The linker placed a section `linker.ld` does not mention. Name it in the script or discard it; do not widen the assertion. |
| `writable and executable mapping` | A page is both. `paging.c` checks this against the tables the processor is actually using, so it is not a false alarm. |
| `QEMU scenario <name> failed` | The boot transcript did not contain a line the `Makefile` requires. Run `make run` and read the console. |
| A scenario **hangs** instead of failing | Usually an interrupt that is never delivered or never acknowledged. `docs/THREADS.md` lists the two known controls with this shape. Ctrl-A then X exits QEMU. |
| `PANIC: <something> self-test failed` | A self-test caught it before boot got any further. The message names which. |

That first row was checked rather than assumed, twice, because a document that
tells you the build will stop you is worthless if the build does not:

| Control | Result |
| --- | --- |
| Add one unused variable to `src/kernel/logo.c` and run `make verify`. | Exit status 2. `error: unused variable 'seneri_control_unused' [-Werror=unused-variable]`. |
| Stage that same file and try to commit it. | `git commit` exits 1, `HEAD` does not move, and the hook prints the same error. The commit does not exist. |

The one move that is never correct is `--no-verify`. If the hook is wrong, fix
the hook in its own commit and say why.

## The habit that matters more than any of the above

When something passes, do not believe it yet. **Break it on purpose and check
that it fails.**

A test that passes tells you the code and the test agree. It does not tell you
the test is capable of disagreeing. Every layer in this repository was checked
this way, and the practice has caught real bugs in code that was already green
— including a verification that ran before the last thing that wrote to the
memory it was verifying, and a thread that could never be interrupted because it
started with interrupts disabled. Both were passing when they were found.

The method is mechanical:

1. Make the claim precise enough to break. "Preemption works" is not; "a thread
   that never yields still loses the processor" is.
2. Change one thing that should make it fail — delete the line, invert the
   condition, skip the call.
3. Run it. If it still passes, **the test was not testing that**, and finding
   that out is worth more than the original green.
4. Revert. Write down what happened, including the ones that did not fail —
   especially those.

Every document in `docs/` ends with a table of these. Adding to that table is
part of finishing a change, not something to do afterwards.

## Reading your way in

When you do not understand a subsystem, the order that works:

1. `docs/<SUBSYSTEM>.md` — what it claims and what it deliberately does not do.
2. `include/seneri/<subsystem>.h` — the surface, and the comments above each
   declaration, which say what the caller must guarantee.
3. The `*_self_test` function in the `.c` file — it is the shortest complete
   description of what the code is supposed to do, because it has to be.
4. The rest of the `.c` file.
5. `git log --oneline -- src/kernel/<subsystem>.c`, then read the messages.

Step 5 is the one people skip. Most of the questions worth asking about this
kernel were answered by whoever wrote the line, in the commit that added it.
