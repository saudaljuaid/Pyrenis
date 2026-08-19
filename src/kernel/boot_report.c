/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Turning what boot discovered into the lines a person reads.
 *
 * These functions make no decisions and change no state. They are their own
 * translation unit because the boot transcript is a contract - the Makefile
 * asserts ninety-one of these lines - and a contract is easier to keep when the
 * text satisfying it sits in one place rather than interleaved with the logic
 * that produced it.
 *
 * Nothing here panics. A report describes what was found; deciding whether what
 * was found is acceptable belongs to the caller.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/acpi.h>
#include <seneri/apic.h>
#include <seneri/apic_timer.h>
#include <seneri/boot.h>
#include <seneri/clock.h>
#include <seneri/console.h>
#include <seneri/cpu.h>
#include <seneri/framebuffer.h>
#include <seneri/heap.h>
#include <seneri/interrupts.h>
#include <seneri/logo.h>
#include <seneri/ioapic.h>
#include <seneri/memory.h>
#include <seneri/paging.h>
#include <seneri/pci.h>
#include <seneri/pic.h>
#include <seneri/pit.h>
#include <seneri/pm_timer.h>
#include <seneri/self_test.h>
#include <seneri/test.h>
#include <seneri/thread.h>
#include <seneri/timer.h>
#include <seneri/tsc.h>
#include <seneri/boot_stages.h>

/*
 * A loader may name itself with an arbitrarily long string. The transcript is a
 * contract, so the name is bounded rather than trusted.
 */
#define MAX_REPORTED_BOOT_LOADER_NAME 64U

void report_boot_context(const struct boot_context *context)
{
    console_write("Seneri OS: boot loader: ");

    if (context->boot_loader_name == NULL) {
        console_write("unnamed");
    } else {
        size_t reported_length = context->boot_loader_name_length;

        if (reported_length > MAX_REPORTED_BOOT_LOADER_NAME) {
            reported_length = MAX_REPORTED_BOOT_LOADER_NAME;
        }

        console_write_n(context->boot_loader_name, reported_length);

        if (reported_length != context->boot_loader_name_length) {
            console_write("...");
        }
    }

    console_putc('\n');

    console_write("Seneri OS: memory map entries: ");
    console_write_u64(context->memory_map_entry_count);
    console_putc('\n');

    console_write("Seneri OS: reported usable bytes: ");
    console_write_u64(context->reported_usable_bytes);
    console_putc('\n');

    console_write("Seneri OS: highest reported address: ");
    console_write_hex(context->highest_reported_address);
    console_putc('\n');
}

void report_allocator(const struct frame_allocator_stats *stats)
{
    console_write("Seneri OS: allocatable frames: ");
    console_write_u64(stats->allocatable_frames);
    console_putc('\n');

    console_write("Seneri OS: free frames: ");
    console_write_u64(stats->free_frames);
    console_putc('\n');

    console_write("Seneri OS: reserved frames: ");
    console_write_u64(stats->reserved_frames);
    console_putc('\n');

    console_write("Seneri OS: highest allocatable address: ");
    console_write_hex(stats->highest_allocatable_address);
    console_putc('\n');
}

void report_acpi_root(const struct acpi_root *root)
{
    console_write("Seneri OS: ACPI ");
    console_write(acpi_root_kind_string(root->kind));
    console_write(" at ");
    console_write_hex(root->physical_address);
    console_write(" OEM ");
    console_write_n(root->oem_id, 6U);
    console_putc('\n');
}

void report_acpi_madt(const struct acpi_madt *madt)
{
    console_write("Seneri OS: ACPI MADT at ");
    console_write_hex(madt->physical_address);
    console_write(" local APIC ");
    console_write_hex(madt->local_apic_address);
    console_write(" flags ");
    console_write_hex(madt->flags);
    console_putc('\n');

    console_write("Seneri OS: ACPI root entries: ");
    console_write_u64(madt->root_entry_count);
    console_write(" MADT OEM ");
    console_write_n(madt->oem_id, 6U);
    console_putc(' ');
    console_write_n(madt->oem_table_id, 8U);
    console_putc('\n');
}

void report_acpi_fadt(const struct acpi_fadt *fadt)
{
    console_write("Seneri OS: ACPI FADT at ");
    console_write_hex(fadt->physical_address);
    console_write(" revision ");
    console_write_u64(fadt->revision);
    console_write(" flags ");
    console_write_hex(fadt->flags);
    console_putc('\n');
}

void report_pm_timer(const struct pm_timer_state *pm_timer)
{
    console_write("Seneri OS: ACPI PM timer port ");
    console_write_hex(pm_timer->port);
    console_write(" width ");
    console_write_u64(pm_timer->counter_bits);
    console_write(" bits address ");
    console_write(pm_timer->extended_address ? "extended" : "fixed");
    console_putc('\n');
}

/*
 * The one firmware table Seneri reads whose absence is not a fault. A machine
 * with no PCI Express host bridge publishes no MCFG, and configuration space is
 * still reachable through the I/O ports the PCI specification has always
 * defined, so absence is reported and boot continues.
 */
void report_acpi_mcfg(const struct acpi_mcfg *mcfg, bool present)
{
    if (!present) {
        console_write("Seneri OS: ACPI MCFG absent\n");
        return;
    }

    console_write("Seneri OS: ACPI MCFG at ");
    console_write_hex(mcfg->physical_address);
    console_write(" windows ");
    console_write_u64(mcfg->allocation_count);
    console_putc('\n');

    for (size_t index = 0; index < mcfg->allocation_count; ++index) {
        const struct acpi_ecam_allocation *allocation =
            &mcfg->allocations[index];

        console_write("Seneri OS: ACPI ECAM segment ");
        console_write_u64(allocation->segment);
        console_write(" base ");
        console_write_hex(allocation->base_address);
        console_write(" buses ");
        console_write_u64(allocation->start_bus);
        console_write(" to ");
        console_write_u64(allocation->end_bus);
        console_write(" size ");
        console_write_u64(acpi_ecam_allocation_size(allocation));
        console_putc('\n');
    }
}

void report_acpi_topology(const struct acpi_topology *topology)
{
    console_write("Seneri OS: ACPI local APIC base ");
    console_write_hex(topology->local_apic_address);

    if (topology->local_apic_address_overridden) {
        console_write(" overridden");
    }

    console_putc('\n');

    console_write("Seneri OS: ACPI processors: ");
    console_write_u64(topology->local_apic_count);
    console_write(" enabled ");
    console_write_u64(topology->enabled_processor_count);
    console_write(" NMI entries ");
    console_write_u64(topology->nmi_entry_count);
    console_write(" unmodelled ");
    console_write_u64(topology->ignored_entry_count);
    console_putc('\n');

    for (size_t index = 0; index < topology->io_apic_count; ++index) {
        const struct acpi_io_apic *io_apic = &topology->io_apics[index];

        console_write("Seneri OS: ACPI I/O APIC id ");
        console_write_u64(io_apic->identifier);
        console_write(" at ");
        console_write_hex(io_apic->address);
        console_write(" base GSI ");
        console_write_u64(io_apic->interrupt_base);
        console_putc('\n');
    }

    for (size_t index = 0; index < topology->interrupt_override_count; ++index) {
        const struct acpi_interrupt_override *override =
            &topology->interrupt_overrides[index];

        console_write("Seneri OS: ACPI override ISA IRQ ");
        console_write_u64(override->source);
        console_write(" to GSI ");
        console_write_u64(override->global_system_interrupt);
        console_write(" flags ");
        console_write_hex(override->flags);
        console_putc('\n');
    }
}

void report_apic(const struct apic_state *apic)
{
    console_write("Seneri OS: local APIC id ");
    console_write_u64(apic->id);
    console_write(" version ");
    console_write_hex(apic->version);
    console_write(" LVT entries ");
    console_write_u64((uint64_t)apic->max_lvt_entry + 1U);
    console_write(" at ");
    console_write_hex(apic->base_address);
    console_putc('\n');

    console_write("Seneri OS: local APIC legacy routing ");
    console_write(apic->legacy_interrupts_routed ? "LINT0 ExtINT" : "masked");
    console_putc('\n');
}

void report_ioapic(const struct ioapic_state *ioapic)
{
    for (size_t index = 0; index < ioapic->count; ++index) {
        const struct ioapic_unit *unit = &ioapic->units[index];

        console_write("Seneri OS: I/O APIC id ");
        console_write_u64(unit->identifier);
        console_write(" version ");
        console_write_hex(unit->version);
        console_write(" entries ");
        console_write_u64(unit->entry_count);
        console_write(" base GSI ");
        console_write_u64(unit->interrupt_base);
        console_putc('\n');
    }
}
