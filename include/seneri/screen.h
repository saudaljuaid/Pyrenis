/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SENERI_SCREEN_H
#define SENERI_SCREEN_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Text on the framebuffer.
 *
 * Until this existed, Seneri could draw pixels and could write words, but not
 * both: the console spoke to a serial port and a VGA text buffer, and the
 * framebuffer knew nothing about characters. This is the layer between them.
 *
 * It owns no policy about what gets printed. src/kernel/console.c decides that
 * and calls in; this decides only where a character lands and what happens
 * when the screen fills.
 */

enum screen_status {
    SCREEN_STATUS_OK = 0,
    SCREEN_STATUS_NO_FRAMEBUFFER,
    SCREEN_STATUS_ALREADY_INITIALIZED,
    SCREEN_STATUS_NOT_INITIALIZED,
    SCREEN_STATUS_BAD_FONT,
    SCREEN_STATUS_CELL_TOO_LARGE,
    SCREEN_STATUS_NO_ROOM,
    SCREEN_STATUS_SURFACE_FAILURE,
    SCREEN_STATUS_DRAW_FAILURE
};

struct screen_state {
    bool active;
    uint32_t columns;
    uint32_t rows;
    uint32_t cell_width;
    uint32_t cell_height;
    uint32_t column;      /* the cursor */
    uint32_t row;
    uint64_t characters;  /* how many have been drawn */
    uint64_t scrolls;
};

/*
 * Take the framebuffer and the built-in font and work out the character grid.
 * Refuses rather than truncates: a screen that cannot hold one full row of one
 * full line of text is not a console, and saying so is better than silently
 * drawing half a character.
 */
enum screen_status screen_initialize(void);

/* Give the owned surface back to the heap and stop mirroring console output. */
enum screen_status screen_release(void);

bool screen_is_active(void);

/*
 * Draw one character at the cursor and advance it. A newline moves to column
 * zero of the next row; anything the font does not cover is drawn as the
 * replacement the font does cover, because a console that refuses to print an
 * unexpected byte is a console that loses the message explaining it.
 */
enum screen_status screen_putc(char character);

enum screen_status screen_write(const char *text);

/* Clear the screen and return the cursor to the origin. */
enum screen_status screen_clear(void);

struct screen_state screen_get_state(void);

/*
 * Re-read one drawn character straight out of the framebuffer and compare it
 * with what the font says it should be. This is what makes the console's claim
 * checkable: everything else here is write-only, and a write-only path proves
 * nothing about what is on the glass.
 */
enum screen_status screen_verify_cell(uint32_t column, uint32_t row, char expected);

bool screen_self_test(void);
const char *screen_status_string(enum screen_status status);

#endif
