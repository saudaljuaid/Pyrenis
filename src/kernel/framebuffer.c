/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/boot.h>
#include <seneri/framebuffer.h>
#include <seneri/paging.h>

/*
 * Every pixel on the screen, addressable, and nothing above that.
 *
 * The interesting part of this file is not the writing, which is one store. It
 * is that the address of a pixel is computed from three numbers a boot loader
 * chose - pitch, width and height - and getting any of them wrong produces a
 * picture rather than a fault. A row stride mistaken for a width is a sheared
 * image; an off-by-one in the height is a write past the mapping. Neither
 * announces itself, so both are checked here rather than looked at.
 */

static struct framebuffer_state state;

/*
 * The bytes between the start of the framebuffer and a pixel. The pitch is the
 * distance between rows and is not necessarily the width in bytes: a loader may
 * pad rows out to whatever its hardware prefers, and a kernel that assumed
 * otherwise would draw a picture that slants.
 */
static uint64_t pixel_offset(uint32_t x, uint32_t y)
{
    return (uint64_t)y * state.pitch +
        (uint64_t)x * FRAMEBUFFER_BYTES_PER_PIXEL;
}

static bool inside(uint32_t x, uint32_t y)
{
    return x < state.width && y < state.height;
}

/*
 * Volatile because these are stores to device memory whose effect the compiler
 * cannot see: nothing in this kernel ever reads most of the framebuffer back,
 * so a non-volatile write loop is dead code the optimiser is entitled to
 * delete. The window is write-combining: stores may gather in processor buffers
 * until the caller ends a batch with cpu_store_fence().
 */
static volatile uint32_t *pixel_at(uint32_t x, uint32_t y)
{
    return (volatile uint32_t *)(uintptr_t)(state.address + pixel_offset(x, y));
}

uint32_t framebuffer_pack(uint8_t red, uint8_t green, uint8_t blue)
{
    return ((uint32_t)red << state.red_position) |
        ((uint32_t)green << state.green_position) |
        ((uint32_t)blue << state.blue_position);
}

uint32_t framebuffer_visible_mask(void)
{
    return (FRAMEBUFFER_CHANNEL_MASK << state.red_position) |
        (FRAMEBUFFER_CHANNEL_MASK << state.green_position) |
        (FRAMEBUFFER_CHANNEL_MASK << state.blue_position);
}

enum framebuffer_status framebuffer_initialize(
    const struct boot_framebuffer *framebuffer
)
{
    struct paging_state paging;

    if (framebuffer == NULL) {
        return FRAMEBUFFER_STATUS_NULL_ARGUMENT;
    }

    if (state.active) {
        return FRAMEBUFFER_STATUS_ALREADY_INITIALIZED;
    }

    if (!framebuffer->present) {
        return FRAMEBUFFER_STATUS_ABSENT;
    }

    if (!paging_is_active()) {
        return FRAMEBUFFER_STATUS_NOT_MAPPED;
    }

    /*
     * Paging owns the address space and decides what it could carve out, so
     * this asks rather than assuming. A framebuffer the loader described but
     * paging could not map is one this layer must refuse, because writing to it
     * would be writing through whatever the identity map happens to say.
     */
    paging = paging_get_state();

    if (paging.framebuffer_size == 0U ||
        framebuffer->address < paging.framebuffer_base ||
        framebuffer->address + framebuffer->size >
            paging.framebuffer_base + paging.framebuffer_size) {
        return FRAMEBUFFER_STATUS_NOT_MAPPED;
    }

    /*
     * multiboot2.c already refused a short pitch, a zero dimension and a span
     * outside the early map. Re-derived here because this is the file that
     * computes addresses from them, and a bound that is only checked where it
     * is parsed is a bound that moves when the parser does.
     */
    if (framebuffer->width == 0U || framebuffer->height == 0U ||
        framebuffer->pitch <
            (uint64_t)framebuffer->width * FRAMEBUFFER_BYTES_PER_PIXEL ||
        (uint64_t)framebuffer->pitch * framebuffer->height !=
            framebuffer->size) {
        return FRAMEBUFFER_STATUS_BAD_GEOMETRY;
    }

    state.address = framebuffer->address;
    state.size = framebuffer->size;
    state.pitch = framebuffer->pitch;
    state.width = framebuffer->width;
    state.height = framebuffer->height;
    state.red_position = framebuffer->red_position;
    state.green_position = framebuffer->green_position;
    state.blue_position = framebuffer->blue_position;
    state.active = true;
    return FRAMEBUFFER_STATUS_OK;
}

bool framebuffer_is_active(void)
{
    return state.active;
}

enum framebuffer_status framebuffer_write_pixel(
    uint32_t x,
    uint32_t y,
    uint32_t pixel
)
{
    if (!state.active) {
        return FRAMEBUFFER_STATUS_NOT_INITIALIZED;
    }

    if (!inside(x, y)) {
        return FRAMEBUFFER_STATUS_OUT_OF_BOUNDS;
    }

    *pixel_at(x, y) = pixel;
    return FRAMEBUFFER_STATUS_OK;
}

enum framebuffer_status framebuffer_read_pixel(
    uint32_t x,
    uint32_t y,
    uint32_t *pixel
)
{
    if (pixel == NULL) {
        return FRAMEBUFFER_STATUS_NULL_ARGUMENT;
    }

    *pixel = 0U;

    if (!state.active) {
        return FRAMEBUFFER_STATUS_NOT_INITIALIZED;
    }

    if (!inside(x, y)) {
        return FRAMEBUFFER_STATUS_OUT_OF_BOUNDS;
    }

    *pixel = *pixel_at(x, y);
    return FRAMEBUFFER_STATUS_OK;
}

/*
 * Row by row rather than as one span, because the padding a pitch may leave at
 * the end of each row is not the framebuffer's to write. On a mode where the
 * pitch exceeds the width, filling straight through would put colour in memory
 * the loader did not say belonged to the picture.
 */
enum framebuffer_status framebuffer_fill(uint32_t pixel)
{
    if (!state.active) {
        return FRAMEBUFFER_STATUS_NOT_INITIALIZED;
    }

    for (uint32_t y = 0; y < state.height; ++y) {
        volatile uint32_t *row = pixel_at(0U, y);

        for (uint32_t x = 0; x < state.width; ++x) {
            row[x] = pixel;
        }
    }

    return FRAMEBUFFER_STATUS_OK;
}

enum framebuffer_status framebuffer_scroll_up(uint32_t rows, uint32_t fill)
{
    if (!state.active) {
        return FRAMEBUFFER_STATUS_NOT_INITIALIZED;
    }

    if (rows == 0U) {
        return FRAMEBUFFER_STATUS_OK;
    }

    /*
     * Scrolling by the whole screen or more leaves nothing to move. Clearing
     * is the correct answer rather than a refusal: a caller asking to discard
     * every row has asked for a blank screen, and answering with an error
     * would make the console's own edge case its caller's problem.
     */
    if (rows >= state.height) {
        return framebuffer_fill(fill);
    }

    /*
     * Upward, front to back. The destination row is always above the source,
     * so a forward walk never reads a row this loop has already overwritten.
     */
    for (uint32_t y = 0; y + rows < state.height; ++y) {
        volatile uint32_t *destination = pixel_at(0U, y);
        const volatile uint32_t *source = pixel_at(0U, y + rows);

        for (uint32_t x = 0; x < state.width; ++x) {
            destination[x] = source[x];
        }
    }

    for (uint32_t y = state.height - rows; y < state.height; ++y) {
        volatile uint32_t *row = pixel_at(0U, y);

        for (uint32_t x = 0; x < state.width; ++x) {
            row[x] = fill;
        }
    }

    return FRAMEBUFFER_STATUS_OK;
}

struct framebuffer_state framebuffer_get_state(void)
{
    return state;
}

/*
 * Re-derive the mapping from the page tables. Called at the end of boot for the
 * same reason paging_verify and heap_verify are: everything that drew ran
 * through this mapping, and a subsystem that turned a page of it back to
 * write-back memory would show up nowhere else.
 */
enum framebuffer_status framebuffer_verify(void)
{
    const uint64_t last = state.address + state.size - 1U;

    if (!state.active) {
        return FRAMEBUFFER_STATUS_NOT_INITIALIZED;
    }

    if ((uint64_t)state.pitch * state.height != state.size ||
        state.pitch < (uint64_t)state.width * FRAMEBUFFER_BYTES_PER_PIXEL) {
        return FRAMEBUFFER_STATUS_BAD_GEOMETRY;
    }

    /*
     * Every page of the picture, not a sample: a framebuffer that is
     * write-combining for its first region and write-back for its second could
     * draw correctly in one mode and still be wrong.
     */
    for (uint64_t address = state.address & ~(PAGING_PAGE_SIZE - 1U);
         address <= last;
         address += PAGING_PAGE_SIZE) {
        struct paging_translation translation;

        if (paging_translate(address, &translation) != PAGING_STATUS_OK) {
            return FRAMEBUFFER_STATUS_NOT_MAPPED;
        }

        if (translation.physical_address != address ||
            translation.level != 1U ||
            translation.permissions !=
                (PAGING_WRITE | PAGING_WRITE_COMBINING) ||
            translation.memory_type != PAGING_MEMORY_WRITE_COMBINING) {
            return FRAMEBUFFER_STATUS_VALIDATION_FAILURE;
        }
    }

    return FRAMEBUFFER_STATUS_OK;
}

const char *framebuffer_status_string(enum framebuffer_status status)
{
    static const char *const messages[] = {
        "ok",
        "null framebuffer argument",
        "framebuffer is already initialized",
        "framebuffer is not initialized",
        "the boot loader set no framebuffer",
        "the framebuffer is not mapped as device memory",
        "framebuffer geometry is inconsistent",
        "coordinate is outside the framebuffer",
        "framebuffer does not match the address space"
    };

    _Static_assert(
        sizeof(messages) / sizeof(messages[0]) ==
            (size_t)FRAMEBUFFER_STATUS_VALIDATION_FAILURE + 1U,
        "framebuffer status messages are out of sync"
    );

    if ((size_t)status >= sizeof(messages) / sizeof(messages[0])) {
        return "unknown framebuffer status";
    }

    return messages[status];
}

/*
 * Arithmetic only, over geometry no machine here produces. The tested target
 * reports a pitch exactly equal to its width in bytes, so the padded case -
 * which is the one that shears a picture when it is got wrong - can only be
 * reached from a synthetic mode. That is the whole reason this exists.
 */
static bool offsets_are_right(void)
{
    const struct framebuffer_state saved = state;
    bool correct;

    state.active = true;
    state.address = UINT64_C(0xFD000000);
    state.width = 100U;
    state.height = 50U;
    state.pitch = 512U;
    state.size = (uint64_t)state.pitch * state.height;
    state.red_position = 16U;
    state.green_position = 8U;
    state.blue_position = 0U;

    correct = pixel_offset(0U, 0U) == 0U &&
        pixel_offset(1U, 0U) == 4U &&
        pixel_offset(99U, 0U) == 396U;

    /* The row stride is the pitch, not the width. This is the whole point. */
    correct = correct && pixel_offset(0U, 1U) == 512U &&
        pixel_offset(0U, 2U) == 1024U &&
        pixel_offset(1U, 1U) == 516U;

    /* The last visible pixel must lie inside the declared span. */
    correct = correct &&
        pixel_offset(state.width - 1U, state.height - 1U) +
            FRAMEBUFFER_BYTES_PER_PIXEL <= state.size;

    /* Bounds are exclusive on both axes, and checked on both. */
    correct = correct && inside(99U, 49U) && !inside(100U, 49U) &&
        !inside(99U, 50U) && !inside(100U, 50U) && inside(0U, 0U);

    /*
     * A pitch equal to the width in bytes is the case the machine actually
     * produces, so it is checked too rather than only the interesting one.
     */
    state.pitch = state.width * FRAMEBUFFER_BYTES_PER_PIXEL;
    correct = correct && pixel_offset(0U, 1U) == 400U;

    state = saved;
    return correct;
}

static bool packing_is_right(void)
{
    const struct framebuffer_state saved = state;
    bool correct;

    state.red_position = 16U;
    state.green_position = 8U;
    state.blue_position = 0U;

    correct = framebuffer_pack(0xFFU, 0U, 0U) == UINT32_C(0x00FF0000) &&
        framebuffer_pack(0U, 0xFFU, 0U) == UINT32_C(0x0000FF00) &&
        framebuffer_pack(0U, 0U, 0xFFU) == UINT32_C(0x000000FF) &&
        framebuffer_pack(0x12U, 0x34U, 0x56U) == UINT32_C(0x00123456) &&
        framebuffer_visible_mask() == UINT32_C(0x00FFFFFF);

    /*
     * The positions come from the loader, so a different order must produce a
     * different word. A packer that ignored them would pass every check above.
     */
    state.red_position = 0U;
    state.blue_position = 16U;
    correct = correct &&
        framebuffer_pack(0x12U, 0x34U, 0x56U) == UINT32_C(0x00563412) &&
        framebuffer_visible_mask() == UINT32_C(0x00FFFFFF);

    /* Nothing outside the three channels is ever set. */
    correct = correct &&
        (framebuffer_pack(0xFFU, 0xFFU, 0xFFU) &
            ~framebuffer_visible_mask()) == 0U;

    state = saved;
    return correct;
}

static bool refusals_are_named(void)
{
    const struct framebuffer_state saved = state;
    struct boot_framebuffer absent;
    uint32_t pixel = 0U;
    bool correct;

    state.active = false;

    correct = framebuffer_initialize(NULL) ==
            FRAMEBUFFER_STATUS_NULL_ARGUMENT &&
        framebuffer_read_pixel(0U, 0U, NULL) ==
            FRAMEBUFFER_STATUS_NULL_ARGUMENT;

    /* Every entry point refuses by name before there is a framebuffer. */
    correct = correct &&
        framebuffer_write_pixel(0U, 0U, 0U) ==
            FRAMEBUFFER_STATUS_NOT_INITIALIZED &&
        framebuffer_read_pixel(0U, 0U, &pixel) ==
            FRAMEBUFFER_STATUS_NOT_INITIALIZED &&
        framebuffer_fill(0U) == FRAMEBUFFER_STATUS_NOT_INITIALIZED &&
        framebuffer_verify() == FRAMEBUFFER_STATUS_NOT_INITIALIZED &&
        !framebuffer_is_active();

    /* A loader that set no mode is described, not blamed. */
    absent.present = false;
    absent.address = 0U;
    absent.size = 0U;
    absent.pitch = 0U;
    absent.width = 0U;
    absent.height = 0U;
    absent.bits_per_pixel = 0U;
    absent.red_position = 0U;
    absent.green_position = 0U;
    absent.blue_position = 0U;
    correct = correct &&
        framebuffer_initialize(&absent) == FRAMEBUFFER_STATUS_ABSENT;

    /* With a framebuffer, a coordinate one past either edge is refused. */
    state.active = true;
    state.width = 100U;
    state.height = 50U;
    state.pitch = 400U;
    state.size = 20000U;

    correct = correct &&
        framebuffer_write_pixel(100U, 0U, 0U) ==
            FRAMEBUFFER_STATUS_OUT_OF_BOUNDS &&
        framebuffer_write_pixel(0U, 50U, 0U) ==
            FRAMEBUFFER_STATUS_OUT_OF_BOUNDS &&
        framebuffer_read_pixel(100U, 0U, &pixel) ==
            FRAMEBUFFER_STATUS_OUT_OF_BOUNDS &&
        framebuffer_read_pixel(0U, 50U, &pixel) ==
            FRAMEBUFFER_STATUS_OUT_OF_BOUNDS &&
        pixel == 0U;

    state = saved;
    return correct;
}

bool framebuffer_self_test(void)
{
    return offsets_are_right() && packing_is_right() && refusals_are_named();
}
