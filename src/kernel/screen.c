/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/font.h>
#include <seneri/framebuffer.h>
#include <seneri/screen.h>

/*
 * Text on the framebuffer.
 *
 * The whole layer is one idea: a character is a small bitmap, and drawing one
 * is copying its rows into a rectangle of pixels. Everything else here is
 * bookkeeping about where the next rectangle goes.
 *
 * Two decisions are worth stating because neither is forced.
 *
 * The first is that nothing is buffered. There is no shadow copy of the screen
 * and no dirty-region tracking; a character is drawn straight into device
 * memory when it arrives. That costs a scroll dearly - it has to read the
 * whole framebuffer back through an uncacheable mapping - and it buys the
 * property that what is on the glass is the only state there is, so
 * screen_verify_cell can check the console by reading the screen rather than
 * by consulting a copy that could agree with the bug.
 *
 * The second is that an unknown byte is drawn rather than refused. A console
 * exists to carry the message that explains what went wrong; one that stops on
 * an unexpected character will eventually swallow exactly the message somebody
 * needed.
 */

/* What an uncovered byte is drawn as. Present in ASCII, so always available. */
#define REPLACEMENT_CHARACTER '?'

/*
 * Background and foreground. Deliberately the same near-black the logo is
 * composited against, so text and image sit on one ground rather than two.
 */
#define SCREEN_BACKGROUND_RED UINT8_C(0x08)
#define SCREEN_BACKGROUND_GREEN UINT8_C(0x0A)
#define SCREEN_BACKGROUND_BLUE UINT8_C(0x0E)

#define SCREEN_FOREGROUND_RED UINT8_C(0xC8)
#define SCREEN_FOREGROUND_GREEN UINT8_C(0xD0)
#define SCREEN_FOREGROUND_BLUE UINT8_C(0xDC)

static struct screen_state state;
static uint32_t background_pixel;
static uint32_t foreground_pixel;

/*
 * The row buffer a glyph is copied into is a local in each function that needs
 * one, never a file-scope static. Thirty-two bytes on a 16 KiB stack is
 * nothing, and a shared buffer would make drawing non-reentrant: this console
 * is live while preemption is running, so a thread switched out mid-glyph
 * would hand the next caller half of somebody else's character.
 *
 * The size is the header's bound rather than the font that happens to be built
 * in, so a taller font cannot silently overrun it. src/rust/font.rs refuses
 * anything taller, and the assertion below keeps the two numbers equal.
 */
_Static_assert(
    FONT_MAX_CELL_HEIGHT == 32U,
    "the row buffer bound no longer matches MAX_HEIGHT in src/rust/font.rs"
);

static uint32_t font_first;
static uint32_t font_count;

static bool font_covers(uint32_t code)
{
    return code >= font_first && code < font_first + font_count;
}

/*
 * Draw one glyph with its top-left corner at a cell. Every pixel of the cell is
 * written, lit or not, so a character always replaces what was under it rather
 * than being drawn over it.
 */
static enum screen_status draw_cell(uint32_t column, uint32_t row, char character)
{
    uint8_t glyph_rows[FONT_MAX_CELL_HEIGHT];
    uint32_t code = (uint32_t)(unsigned char)character;

    if (!font_covers(code)) {
        code = (uint32_t)REPLACEMENT_CHARACTER;
    }

    if (seneri_font_glyph(code, glyph_rows, sizeof(glyph_rows)) !=
        FONT_STATUS_OK) {
        return SCREEN_STATUS_DRAW_FAILURE;
    }

    const uint32_t origin_x = column * state.cell_width;
    const uint32_t origin_y = row * state.cell_height;

    for (uint32_t y = 0; y < state.cell_height; ++y) {
        const uint8_t bits = glyph_rows[y];

        for (uint32_t x = 0; x < state.cell_width; ++x) {
            const bool lit = (bits & (uint8_t)(0x80U >> x)) != 0U;
            const uint32_t pixel = lit ? foreground_pixel : background_pixel;

            if (framebuffer_write_pixel(origin_x + x, origin_y + y, pixel) !=
                FRAMEBUFFER_STATUS_OK) {
                return SCREEN_STATUS_DRAW_FAILURE;
            }
        }
    }

    return SCREEN_STATUS_OK;
}

/*
 * Move everything up one line. The exposed band is one cell tall rather than
 * the remainder of the screen, because the grid is derived by division and the
 * last few pixel rows below the final cell are not part of any cell.
 */
static enum screen_status scroll_one_line(void)
{
    if (framebuffer_scroll_up(state.cell_height, background_pixel) !=
        FRAMEBUFFER_STATUS_OK) {
        return SCREEN_STATUS_DRAW_FAILURE;
    }

    state.scrolls += 1U;
    return SCREEN_STATUS_OK;
}

static enum screen_status newline(void)
{
    state.column = 0U;

    if (state.row + 1U < state.rows) {
        state.row += 1U;
        return SCREEN_STATUS_OK;
    }

    /*
     * The cursor stays on the last row and the picture moves instead, which is
     * what makes the bottom line the live one.
     */
    return scroll_one_line();
}

/*
 * How many whole cells fit. Split out from screen_initialize because it is the
 * only arithmetic here that can be wrong without a framebuffer to be wrong on,
 * and so the only part the self-test can reach before boot has one.
 *
 * A partial cell at the right or bottom edge is not a cell. Reporting one would
 * put the console's own bounds check in disagreement with the framebuffer's.
 */
static bool grid_for(
    uint32_t screen_width,
    uint32_t screen_height,
    uint32_t cell_width,
    uint32_t cell_height,
    uint32_t *columns,
    uint32_t *rows
)
{
    if (cell_width == 0U || cell_height == 0U) {
        return false;
    }

    const uint32_t across = screen_width / cell_width;
    const uint32_t down = screen_height / cell_height;

    /*
     * One column and one row is the smallest thing that is still a console.
     * Below that the arithmetic still works and the result is not a console,
     * so it is refused here rather than discovered by a caller.
     */
    if (across == 0U || down == 0U) {
        return false;
    }

    *columns = across;
    *rows = down;
    return true;
}

enum screen_status screen_initialize(void)
{
    uint32_t width = 0U;
    uint32_t height = 0U;
    uint32_t first = 0U;
    uint32_t count = 0U;

    if (state.active) {
        return SCREEN_STATUS_ALREADY_INITIALIZED;
    }

    if (!framebuffer_is_active()) {
        return SCREEN_STATUS_NO_FRAMEBUFFER;
    }

    if (seneri_font_geometry(&width, &height, &first, &count) !=
        FONT_STATUS_OK) {
        return SCREEN_STATUS_BAD_FONT;
    }

    if (height > FONT_MAX_CELL_HEIGHT) {
        return SCREEN_STATUS_CELL_TOO_LARGE;
    }

    const struct framebuffer_state framebuffer = framebuffer_get_state();
    uint32_t columns = 0U;
    uint32_t rows = 0U;

    if (!grid_for(framebuffer.width, framebuffer.height, width, height,
            &columns, &rows)) {
        return SCREEN_STATUS_NO_ROOM;
    }

    font_first = first;
    font_count = count;

    /*
     * The replacement has to be one the font covers, or an uncovered byte
     * would recurse into another uncovered byte.
     */
    if (!font_covers((uint32_t)REPLACEMENT_CHARACTER)) {
        return SCREEN_STATUS_BAD_FONT;
    }

    background_pixel = framebuffer_pack(SCREEN_BACKGROUND_RED,
        SCREEN_BACKGROUND_GREEN, SCREEN_BACKGROUND_BLUE);
    foreground_pixel = framebuffer_pack(SCREEN_FOREGROUND_RED,
        SCREEN_FOREGROUND_GREEN, SCREEN_FOREGROUND_BLUE);

    state.columns = columns;
    state.rows = rows;
    state.cell_width = width;
    state.cell_height = height;
    state.column = 0U;
    state.row = 0U;
    state.characters = 0U;
    state.scrolls = 0U;
    state.active = true;

    return screen_clear();
}

bool screen_is_active(void)
{
    return state.active;
}

enum screen_status screen_clear(void)
{
    if (!state.active) {
        return SCREEN_STATUS_NOT_INITIALIZED;
    }

    if (framebuffer_fill(background_pixel) != FRAMEBUFFER_STATUS_OK) {
        return SCREEN_STATUS_DRAW_FAILURE;
    }

    state.column = 0U;
    state.row = 0U;
    return SCREEN_STATUS_OK;
}

enum screen_status screen_putc(char character)
{
    if (!state.active) {
        return SCREEN_STATUS_NOT_INITIALIZED;
    }

    if (character == '\n') {
        return newline();
    }

    /*
     * A carriage return on its own returns to the start of the line. The serial
     * console emits one before every newline, and without this the pair would
     * cost two lines instead of one.
     */
    if (character == '\r') {
        state.column = 0U;
        return SCREEN_STATUS_OK;
    }

    if (state.column >= state.columns) {
        const enum screen_status status = newline();

        if (status != SCREEN_STATUS_OK) {
            return status;
        }
    }

    const enum screen_status status =
        draw_cell(state.column, state.row, character);

    if (status != SCREEN_STATUS_OK) {
        return status;
    }

    state.column += 1U;
    state.characters += 1U;
    return SCREEN_STATUS_OK;
}

enum screen_status screen_write(const char *text)
{
    if (text == NULL) {
        return SCREEN_STATUS_OK;
    }

    for (size_t index = 0; text[index] != '\0'; ++index) {
        const enum screen_status status = screen_putc(text[index]);

        if (status != SCREEN_STATUS_OK) {
            return status;
        }
    }

    return SCREEN_STATUS_OK;
}

struct screen_state screen_get_state(void)
{
    return state;
}

enum screen_status screen_verify_cell(
    uint32_t column,
    uint32_t row,
    char expected
)
{
    uint8_t glyph_rows[FONT_MAX_CELL_HEIGHT];

    if (!state.active) {
        return SCREEN_STATUS_NOT_INITIALIZED;
    }

    if (column >= state.columns || row >= state.rows) {
        return SCREEN_STATUS_NO_ROOM;
    }

    uint32_t code = (uint32_t)(unsigned char)expected;

    if (!font_covers(code)) {
        code = (uint32_t)REPLACEMENT_CHARACTER;
    }

    if (seneri_font_glyph(code, glyph_rows, sizeof(glyph_rows)) !=
        FONT_STATUS_OK) {
        return SCREEN_STATUS_DRAW_FAILURE;
    }

    const uint32_t origin_x = column * state.cell_width;
    const uint32_t origin_y = row * state.cell_height;
    const uint32_t mask = framebuffer_visible_mask();

    for (uint32_t y = 0; y < state.cell_height; ++y) {
        const uint8_t bits = glyph_rows[y];

        for (uint32_t x = 0; x < state.cell_width; ++x) {
            const bool lit = (bits & (uint8_t)(0x80U >> x)) != 0U;
            const uint32_t want = lit ? foreground_pixel : background_pixel;
            uint32_t got = 0U;

            if (framebuffer_read_pixel(origin_x + x, origin_y + y, &got) !=
                FRAMEBUFFER_STATUS_OK) {
                return SCREEN_STATUS_DRAW_FAILURE;
            }

            if ((got & mask) != (want & mask)) {
                return SCREEN_STATUS_DRAW_FAILURE;
            }
        }
    }

    return SCREEN_STATUS_OK;
}

const char *screen_status_string(enum screen_status status)
{
    static const char *const messages[] = {
        "ok",
        "no framebuffer to put text on",
        "screen console is already initialized",
        "screen console is not initialized",
        "font table is unusable for a console",
        "font cell is taller than the row buffer",
        "framebuffer has no room for a character grid",
        "screen console failed to draw"
    };

    _Static_assert(
        sizeof(messages) / sizeof(messages[0]) ==
            (size_t)SCREEN_STATUS_DRAW_FAILURE + 1U,
        "screen status messages are out of sync with enum screen_status"
    );

    if ((size_t)status >= sizeof(messages) / sizeof(messages[0])) {
        return "unknown screen status";
    }

    return messages[status];
}

/*
 * What this layer must refuse, checked before boot has a framebuffer.
 *
 * Only two things here can be tested without one: the grid arithmetic, and the
 * refusal to draw before initialization. Everything else about a console is a
 * claim about pixels, and pixels are checked by prove_screen_console in
 * src/kernel/boot_proofs.c reading them back off the glass.
 */
static bool grid_is_right(void)
{
    uint32_t columns = 0U;
    uint32_t rows = 0U;

    /* The mode boot actually runs in. */
    if (!grid_for(1024U, 768U, 8U, 16U, &columns, &rows)) {
        return false;
    }

    if (columns != 128U || rows != 48U) {
        return false;
    }

    /* The smallest mode this kernel accepts. */
    if (!grid_for(640U, 480U, 8U, 16U, &columns, &rows)) {
        return false;
    }

    if (columns != 80U || rows != 30U) {
        return false;
    }

    /*
     * A partial cell at either edge is not a cell. 1023 pixels across an
     * 8-pixel cell is 127 whole ones and seven pixels of nothing.
     */
    if (!grid_for(1023U, 767U, 8U, 16U, &columns, &rows)) {
        return false;
    }

    if (columns != 127U || rows != 47U) {
        return false;
    }

    /* Narrower or shorter than one cell is not a console. */
    if (grid_for(7U, 768U, 8U, 16U, &columns, &rows)) {
        return false;
    }

    if (grid_for(1024U, 15U, 8U, 16U, &columns, &rows)) {
        return false;
    }

    /* A cell with no extent would divide by zero. */
    if (grid_for(1024U, 768U, 0U, 16U, &columns, &rows)) {
        return false;
    }

    if (grid_for(1024U, 768U, 8U, 0U, &columns, &rows)) {
        return false;
    }

    return true;
}

static bool refusals_are_named(void)
{
    static const enum screen_status every[] = {
        SCREEN_STATUS_OK,
        SCREEN_STATUS_NO_FRAMEBUFFER,
        SCREEN_STATUS_ALREADY_INITIALIZED,
        SCREEN_STATUS_NOT_INITIALIZED,
        SCREEN_STATUS_BAD_FONT,
        SCREEN_STATUS_CELL_TOO_LARGE,
        SCREEN_STATUS_NO_ROOM,
        SCREEN_STATUS_DRAW_FAILURE
    };

    for (size_t index = 0; index < sizeof(every) / sizeof(every[0]); ++index) {
        const char *message = screen_status_string(every[index]);

        if (message == NULL || message[0] == '\0') {
            return false;
        }
    }

    /* A value outside the enum must be named rather than indexed. */
    return screen_status_string((enum screen_status)99) != NULL;
}

bool screen_self_test(void)
{
    if (!grid_is_right()) {
        return false;
    }

    if (!refusals_are_named()) {
        return false;
    }

    /*
     * Before initialization every entry point refuses. This runs before the
     * framebuffer is adopted, so it is the real state rather than a simulated
     * one - which also means it can only be checked once, and only here.
     */
    if (!state.active) {
        if (screen_putc('x') != SCREEN_STATUS_NOT_INITIALIZED) {
            return false;
        }

        if (screen_clear() != SCREEN_STATUS_NOT_INITIALIZED) {
            return false;
        }

        if (screen_verify_cell(0U, 0U, 'x') != SCREEN_STATUS_NOT_INITIALIZED) {
            return false;
        }
    }

    return true;
}
