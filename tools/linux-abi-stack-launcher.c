#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define PAGE_BYTES UINT64_C(4096)
#define STACK_BASE UINT64_C(0x0000700000000000)
#define STACK_PAGES UINT64_C(3)
#define AT_NULL_VALUE UINT64_C(0)
#define AT_PAGESZ_VALUE UINT64_C(6)

extern const unsigned char _binary_busybox_start[];

__attribute__((noreturn)) void linux_abi_enter(uintptr_t entry,
                                               uintptr_t stack_pointer);

struct measured_segment {
    uintptr_t virtual_address;
    size_t file_offset;
    size_t file_bytes;
    size_t memory_bytes;
    int protection;
};

static const struct measured_segment measured_segments[] = {
    {UINT64_C(0x400001000000), 0x0000U, 0x0158U, 0x0158U, PROT_READ},
    {UINT64_C(0x400001001000), 0x1000U, 0x5563U, 0x5563U,
     PROT_READ | PROT_EXEC},
    {UINT64_C(0x400001007000), 0x7000U, 0x0ed1U, 0x0ed1U, PROT_READ},
    {UINT64_C(0x400001008000), 0x8000U, 0x00feU, 0x0b38U,
     PROT_READ | PROT_WRITE},
};

static void fail(const char *message)
{
    int saved_errno = errno;

    fprintf(stderr, "%s: %s\n", message, strerror(saved_errno));
    exit(1);
}

static uintptr_t page_down(uintptr_t value)
{
    return value & ~(uintptr_t)(PAGE_BYTES - 1U);
}

static uintptr_t page_up(uintptr_t value)
{
    return (value + PAGE_BYTES - 1U) & ~(uintptr_t)(PAGE_BYTES - 1U);
}

static void install_segment(const struct measured_segment *segment)
{
    uintptr_t page_start = page_down(segment->virtual_address);
    uintptr_t memory_end = segment->virtual_address + segment->memory_bytes;
    uintptr_t page_end = page_up(memory_end);
    size_t mapping_bytes = (size_t)(page_end - page_start);
    void *mapping = mmap((void *)page_start, mapping_bytes,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                         -1, 0);

    if (mapping == MAP_FAILED || (uintptr_t)mapping != page_start) {
        fail("measured BusyBox segment mapping failed");
    }
    memcpy((void *)segment->virtual_address,
           _binary_busybox_start + segment->file_offset,
           segment->file_bytes);
    memset((void *)(segment->virtual_address + segment->file_bytes), 0,
           segment->memory_bytes - segment->file_bytes);
    if (mprotect(mapping, mapping_bytes, segment->protection) != 0) {
        fail("measured BusyBox segment protection failed");
    }
}

static uintptr_t push_string(uintptr_t cursor, const char *string,
                             size_t bytes)
{
    cursor -= bytes;
    memcpy((void *)cursor, string, bytes);
    return cursor;
}

int main(void)
{
    static const char argv_zero[] = "busybox";
    static const char argv_one[] = "echo";
    static const char argv_two[] = "SAPOTE";
    uintptr_t stack_bytes = STACK_PAGES * PAGE_BYTES;
    void *stack = mmap((void *)STACK_BASE, stack_bytes, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                       -1, 0);

    if (stack == MAP_FAILED || (uintptr_t)stack != STACK_BASE) {
        fail("minimal Linux initial-stack mapping failed");
    }
    if (mprotect((void *)(STACK_BASE + PAGE_BYTES),
                 stack_bytes - PAGE_BYTES, PROT_READ | PROT_WRITE) != 0) {
        fail("minimal Linux initial-stack protection failed");
    }

    for (size_t index = 0U;
         index < sizeof(measured_segments) / sizeof(measured_segments[0]);
         ++index) {
        install_segment(&measured_segments[index]);
    }

    uintptr_t cursor = STACK_BASE + stack_bytes;
    cursor = push_string(cursor, argv_two, sizeof(argv_two));
    uintptr_t argv_two_pointer = cursor;
    cursor = push_string(cursor, argv_one, sizeof(argv_one));
    uintptr_t argv_one_pointer = cursor;
    cursor = push_string(cursor, argv_zero, sizeof(argv_zero));
    uintptr_t argv_zero_pointer = cursor;

    cursor &= ~(uintptr_t)15U;
    cursor = (cursor - 10U * sizeof(uint64_t)) & ~(uintptr_t)15U;
    uint64_t *initial_stack = (uint64_t *)cursor;
    initial_stack[0] = 3U;
    initial_stack[1] = argv_zero_pointer;
    initial_stack[2] = argv_one_pointer;
    initial_stack[3] = argv_two_pointer;
    initial_stack[4] = 0U;
    initial_stack[5] = 0U;
    initial_stack[6] = AT_PAGESZ_VALUE;
    initial_stack[7] = PAGE_BYTES;
    initial_stack[8] = AT_NULL_VALUE;
    initial_stack[9] = 0U;

    linux_abi_enter(UINT64_C(0x40000100107a),
        (uintptr_t)initial_stack);
}
