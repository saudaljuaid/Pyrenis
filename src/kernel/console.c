/* SPDX-License-Identifier: GPL-3.0-only */
#include <stddef.h>
#include <stdint.h>

#include <seneri/console.h>
#include <seneri/screen.h>

#define VGA_WIDTH 80U
#define VGA_HEIGHT 25U
#define VGA_MEMORY ((volatile uint16_t *)(uintptr_t)0xB8000U)
#define VGA_COLOR UINT8_C(0x0B)
#define COM1 UINT16_C(0x03F8)
#define SERIAL_TRANSMIT_READY UINT8_C(0x20)
#define SERIAL_WAIT_LIMIT UINT32_C(100000)

static size_t console_row;
static size_t console_column;

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;

    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static void serial_initialize(void)
{
    outb(COM1 + 1U, 0U);
    outb(COM1 + 3U, 0x80U);
    outb(COM1, 3U);
    outb(COM1 + 1U, 0U);
    outb(COM1 + 3U, 0x03U);
    outb(COM1 + 2U, 0xC7U);
    outb(COM1 + 4U, 0x0BU);
}

static void serial_putc(char character)
{
    for (uint32_t attempt = 0; attempt < SERIAL_WAIT_LIMIT; ++attempt) {
        if ((inb(COM1 + 5U) & SERIAL_TRANSMIT_READY) != 0U) {
            outb(COM1, (uint8_t)character);
            return;
        }
    }
}

static uint16_t vga_entry(char character)
{
    return (uint16_t)(uint8_t)character | ((uint16_t)VGA_COLOR << 8U);
}

static void console_clear(void)
{
    for (size_t index = 0; index < VGA_WIDTH * VGA_HEIGHT; ++index) {
        VGA_MEMORY[index] = vga_entry(' ');
    }

    console_row = 0;
    console_column = 0;
}

static void console_scroll(void)
{
    for (size_t row = 1; row < VGA_HEIGHT; ++row) {
        for (size_t column = 0; column < VGA_WIDTH; ++column) {
            VGA_MEMORY[(row - 1U) * VGA_WIDTH + column] =
                VGA_MEMORY[row * VGA_WIDTH + column];
        }
    }

    for (size_t column = 0; column < VGA_WIDTH; ++column) {
        VGA_MEMORY[(VGA_HEIGHT - 1U) * VGA_WIDTH + column] = vga_entry(' ');
    }

    console_row = VGA_HEIGHT - 1U;
}

void console_initialize(void)
{
    serial_initialize();
    console_clear();
}

void console_putc(char character)
{
    serial_putc(character);

    /*
     * And the framebuffer, once there is one. This is deliberately additive:
     * the serial port and the VGA text buffer keep working exactly as they did,
     * because the test harness reads the serial log and a change that quietly
     * moved the transcript would break every scenario at once.
     *
     * screen_putc refuses cleanly before src/kernel/screen.c is initialized, so
     * this needs no guard of its own and the early boot path is unchanged.
     */
    if (screen_is_active()) {
        (void)screen_putc(character);
    }

    if (character == '\n') {
        console_column = 0;
        ++console_row;
    } else {
        VGA_MEMORY[console_row * VGA_WIDTH + console_column] = vga_entry(character);
        ++console_column;

        if (console_column == VGA_WIDTH) {
            console_column = 0;
            ++console_row;
        }
    }

    if (console_row == VGA_HEIGHT) {
        console_scroll();
    }
}

void console_write(const char *text)
{
    if (text == NULL) {
        console_write("<null>");
        return;
    }

    while (*text != '\0') {
        console_putc(*text);
        ++text;
    }
}

void console_write_n(const char *text, size_t length)
{
    if (text == NULL) {
        console_write("<null>");
        return;
    }

    for (size_t index = 0; index < length; ++index) {
        console_putc(text[index]);
    }
}

void console_write_hex(uint64_t value)
{
    static const char digits[] = "0123456789ABCDEF";

    console_write("0x");

    for (unsigned int shift = 60U;; shift -= 4U) {
        console_putc(digits[(value >> shift) & UINT64_C(0xF)]);

        if (shift == 0U) {
            break;
        }
    }
}

void console_write_u64(uint64_t value)
{
    char digits[20];
    size_t length = 0;

    if (value == 0U) {
        console_putc('0');
        return;
    }

    while (value != 0U) {
        digits[length] = (char)('0' + (value % 10U));
        value /= 10U;
        ++length;
    }

    while (length != 0U) {
        --length;
        console_putc(digits[length]);
    }
}

_Noreturn void console_halt(void)
{
    for (;;) {
        __asm__ volatile ("cli; hlt" : : : "memory");
    }
}

_Noreturn void console_panic(const char *message)
{
    console_write("Seneri OS PANIC: ");
    console_write(message);
    console_putc('\n');
    console_halt();
}
