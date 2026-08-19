# Where everything is

Seneri is thirty-six source files and twenty-three documents. This page exists
so you never have to find your way through that by opening files at random.

If you are here because the code looked impenetrable: it is not that you are
missing something. A kernel is dense in a way application code is not, because
almost every line is either a hardware register with a fixed meaning or an
ordering constraint that cannot be reordered. What makes it readable is knowing
*why* before you read *what*, and that is what the documents are for.

## Start here, in this order

1. **`src/kernel/kernel.c`** — 399 lines, and almost all of it is a list. This is
   the order boot happens in, top to bottom, and every step is one call. Read it
   first even if none of the calls mean anything yet, because everything else is
   a detail of one of these lines.
2. **`docs/DAY_ONE.md`** — what the machine looks like at the instant the loader
   hands it over.
3. **`src/kernel/logo.c`** — 39 lines. The smallest complete file in the kernel.
4. **Any one document in `docs/`, then its `.c` file.** Not the other way round.

## What each file is

Sizes are lines, which is a poor proxy for difficulty but a good proxy for how
long you will be in there.

### The sequence

| File | | |
| --- | ---: | --- |
| `kernel.c` | 399 | The order boot happens in. Nothing else. |
| `boot_report.c` | 271 | Turns what was discovered into the transcript. Never decides anything. |
| `boot_proofs.c` | 1648 | Every proof and bring-up boot runs. Panics rather than returning a status. |

### Getting off the ground

| File | | |
| --- | ---: | --- |
| `arch/x86_64/boot.S` | 190 | Multiboot2 header, 32-bit entry, the first page tables, the jump to long mode. |
| `multiboot2.c` | 562 | Parsing what the loader left in memory. Refuses malformed input rather than trusting it. |
| `console.c` | 186 | Serial port and VGA text. The only way the kernel speaks until the framebuffer exists. |
| `cpu.c`, `arch/x86_64/cpu.S` | 338 + 297 | Descriptor tables, control registers, and the instructions C cannot express. |

### Not dying

| File | | |
| --- | ---: | --- |
| `interrupts.c` | 523 | The interrupt descriptor table and the dispatcher. Read `docs/NEVER_TRIPLE_FAULT.md` first. |
| `arch/x86_64/interrupts.S` | 327 | The stubs that save state before C can run. The ABI is in the comments. |
| `interrupt_self_test.c` | 162 | Deliberately causing faults to prove they are contained. |

### Knowing what machine this is

| File | | |
| --- | ---: | --- |
| `acpi.c` | 302 | Finding the firmware's root pointer without trusting it. |
| `acpi_tables.c` | 1612 | RSDT, XSDT, FADT, MCFG. Bounds and checksums on everything. |
| `acpi_madt.c` | 1058 | The interrupt topology: which APICs exist and how legacy IRQs were rerouted. |
| `acpi_util.c` | 83 | The checks the above three share. |
| `pci.c` | 1254 | Every device on every bus, read two independent ways so each checks the other. |

### Interrupt hardware

| File | | |
| --- | ---: | --- |
| `apic.c` | 571 | The local APIC. |
| `ioapic.c` | 450 | Routing external interrupts to it. |
| `pic.c` | 244 | The 8259 pair, and latching them permanently shut. |

### Telling the time

| File | | |
| --- | ---: | --- |
| `pit.c` | 305 | The 8254. Used to calibrate the others, then retired. |
| `pm_timer.c` | 645 | The ACPI timer — the one reference nothing else calibrated. |
| `apic_timer.c` | 762 | Calibrated against the above; drives preemption. |
| `tsc.c` | 366 | The time-stamp counter, and what it may not claim. |
| `clock.c` | 166 | One monotonic instant, chosen from whichever of the above is best. |
| `timer.c` | 691 | Deadlines. `timer_arm` is what preemption is built on. |

### Memory

| File | | |
| --- | ---: | --- |
| `physical_memory.c` | 454 | Which physical frames exist and which are free. |
| `paging.c` | 2293 | Four-level page tables, W^X enforced by hardware, device windows uncached. The densest file here. |
| `heap.c` | 792 | A bounded, guarded allocator. The first thing that is not a fixed array. |

### More than one thing at a time

| File | | |
| --- | ---: | --- |
| `thread.c` | 1165 | Threads, guarded stacks, the scheduler, and preemption. |
| `arch/x86_64/thread.S` | 107 | The context switch itself. Six registers and a stack pointer. |

### Pixels

| File | | |
| --- | ---: | --- |
| `framebuffer.c` | 460 | The linear framebuffer, validated field by field, mapped uncached. |
| `screen.c` | 525 | Text on the framebuffer: cells, wrapping, scrolling, and reading it back. |
| `font.c` | 39 | The C side of the font: names for what the reader can refuse. |
| `rust/font.rs` | 276 | The glyph table reader. Rust, on the first hot path in this kernel. |
| `logo.c` | 39 | The C side of the logo: three lines of glue. |
| `rust/logo.rs` | 330 | The decoder. Rust, because it parses bytes the kernel did not produce. |
| `rust/abi.rs`, `rust/lib.rs` | 103 + 42 | What the two languages promise each other. |

### Proving it

| File | | |
| --- | ---: | --- |
| `test.c` | 2817 | The twenty-three QEMU scenarios and what each must print. |
| `self_test.c` | 588 | Checks that run on synthetic data every boot, before any hardware is touched. |

## The boot sequence, in order

Each line is one call in `kernel_main`. This is the whole kernel, in the order
it happens.

    console_initialize            speak
    interrupts_initialize         stop dying
    <sixteen self-tests>          prove the arithmetic before trusting the hardware
    boot_context_parse            read what the loader left
    acpi_root_discover            find the firmware tables
    acpi_madt / fadt / mcfg       read them
    pm_timer_initialize           the reference clock
    apic_bring_online             the local APIC
    ioapic_initialize             external interrupt routing
    frame_allocator_initialize    own physical memory
    prove_frame_lifecycle
    install_page_tables           own the address space, W^X on
    prove_paging_lifecycle
    bring_up_heap                 allocation that is not a fixed array
    prove_heap_lifecycle
    kernel_test_run               the scenario, if one was selected
    prove_timer_route x3          the PIT over both delivery paths
    retire_legacy_interrupt_path  the 8259 latched shut
    prove_pm_timer                the reference
    prove_apic_timer              calibrated against it
    prove_tsc                     calibrated against it
    retire_pit                    only now may the 8254 go
    prove_clocks_without_pit      and the three still agree
    prove_monotonic_time          instants and deadlines
    bring_up_pci                  count the machine
    prove_threads                 more than one thread of control
    prove_preemption              threads that never yield still lose the CPU
    prove_framebuffer             every one of 786,432 pixels
    draw_logo                     the splash
    prove_screen_console          text on the screen, read back off the glass
    paging_verify                 re-walk the tables at the end, not the middle
    heap_verify
    pci_verify
    framebuffer_verify

The order is an argument in three places, and each is commented where it
happens: page tables need frames, so they come after the allocator; the 8254 may
only be retired once something independent of it can time an interval; and the
closing verification block runs *after* the last thing that writes through each
mapping, which is why `framebuffer_verify` sits below `draw_logo` rather than
inside the framebuffer's own proof.

## When you want to know

| | |
| --- | --- |
| "what does this subsystem promise?" | the matching `docs/*.md`, and its *Deferred work* list for what it does not |
| "what does this function actually do?" | its `*_self_test` — it is the shortest complete description, because it has to be |
| "why is this line here?" | `git log -S'<the line>' -- <file>` — most non-obvious lines were explained by whoever added them |
| "what is this kernel carrying?" | `docs/DEBT.md`, measured rather than remembered |
| "how do I change something?" | `docs/WORKING_ON_SENERI.md` |
| "what is the plan?" | `docs/HARDWARE_AND_APPLICATIONS.md` |

## How to re-measure this page

    wc -l src/kernel/*.c src/arch/x86_64/*.S src/rust/*.rs | sort -rn
    grep -oE '^ +[a-z_]+\(' src/kernel/kernel.c | tr -d ' ('

The second prints the boot sequence in order. It also catches the calls nested
inside conditionals - `draw_logo` and the closing verification block only run
when the framebuffer came up - so it prints slightly more than the list above.
If the two disagree about *order*, the list above is wrong.
