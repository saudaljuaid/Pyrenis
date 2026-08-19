/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SENERI_CPU_H
#define SENERI_CPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CPU_GDT_CODE_SELECTOR UINT16_C(0x08)
#define CPU_GDT_DATA_SELECTOR UINT16_C(0x10)
#define CPU_GDT_TSS_SELECTOR UINT16_C(0x18)
#define CPU_IST_DOUBLE_FAULT UINT8_C(1)
#define CPU_IST_NMI UINT8_C(2)
#define CPU_IST_MACHINE_CHECK UINT8_C(3)
#define CPU_IST_STACK_SIZE (16U * 1024U)

struct cpuid_result {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
};

struct descriptor_table_pointer {
    uint16_t limit;
    uintptr_t base;
} __attribute__((packed));

enum cpu_status {
    CPU_STATUS_OK = 0,
    CPU_STATUS_ALREADY_PREPARED,
    CPU_STATUS_NOT_PREPARED,
    CPU_STATUS_ALREADY_ACTIVE,
    CPU_STATUS_BAD_GDT,
    CPU_STATUS_BAD_TSS,
    CPU_STATUS_BAD_STACK,
    CPU_STATUS_GDTR_MISMATCH,
    CPU_STATUS_TR_MISMATCH
};

enum cpu_status cpu_tables_prepare(void);
enum cpu_status cpu_tables_activate(void);
enum cpu_status cpu_tables_validate(void);
bool cpu_tables_active(void);
bool cpu_address_on_ist(uint8_t ist_index, uintptr_t address);
bool cpu_stack_canaries_valid(void);
const char *cpu_status_string(enum cpu_status status);

void cpu_interrupt_disable(void);
void cpu_interrupt_enable(void);
bool cpu_interrupts_enabled(void);
void cpu_halt_once(void);
void cpu_enable_and_halt(void);
uint8_t cpu_in8(uint16_t port);
uint32_t cpu_in32(uint16_t port);
void cpu_out8(uint16_t port, uint8_t value);
void cpu_out32(uint16_t port, uint32_t value);
void cpu_io_wait(void);
void cpu_cpuid(uint32_t leaf, uint32_t subleaf, struct cpuid_result *result);
uint64_t cpu_read_msr(uint32_t msr);
void cpu_write_msr(uint32_t msr, uint64_t value);
uint64_t cpu_read_tsc(void);
/* Drain all older stores, including weakly ordered write-combining stores. */
void cpu_store_fence(void);
/* Write back and invalidate every internal cache before a memory-type change. */
void cpu_write_back_and_invalidate_cache(void);
uint64_t cpu_read_cr2(void);
uint64_t cpu_read_cr0(void);
void cpu_write_cr0(uint64_t value);
uint64_t cpu_read_cr3(void);
void cpu_write_cr3(uint64_t value);
uint64_t cpu_read_cr4(void);
void cpu_invalidate_page(uintptr_t virtual_address);
uint16_t cpu_read_cs(void);
uint16_t cpu_read_task_register(void);
uintptr_t cpu_read_stack_pointer(void);
void cpu_load_gdt_and_tss(const struct descriptor_table_pointer *pointer);
void cpu_load_idt(const struct descriptor_table_pointer *pointer);
void cpu_store_gdt(struct descriptor_table_pointer *pointer);
void cpu_store_idt(struct descriptor_table_pointer *pointer);

#endif
