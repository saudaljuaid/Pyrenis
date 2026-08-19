/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/framebuffer.h>
#include <seneri/heap.h>
#include <seneri/surface.h>

/*
 * A cached picture and the one rectangle that changed.
 *
 * Damage is deliberately one bounding rectangle rather than a list. Text
 * changes are small and a scroll changes almost the whole screen, so a fixed,
 * allocation-free union is the useful point between copying every pixel and
 * building a compositor before the kernel has one.
 */

static uint32_t minimum(uint32_t left, uint32_t right)
{
    return left < right ? left : right;
}

static uint32_t *row_at(struct surface *surface, uint32_t y)
{
    return (uint32_t *)(void *)(
        (uint8_t *)(void *)surface->pixels + (uint64_t)y * surface->pitch
    );
}

static const uint32_t *const_row_at(const struct surface *surface, uint32_t y)
{
    return (const uint32_t *)(const void *)(
        (const uint8_t *)(const void *)surface->pixels +
        (uint64_t)y * surface->pitch
    );
}

static void clear_damage(struct surface *surface)
{
    surface->damage.pending = false;
    surface->damage.rectangle.x = 0U;
    surface->damage.rectangle.y = 0U;
    surface->damage.rectangle.width = 0U;
    surface->damage.rectangle.height = 0U;
}

/* Every caller has already clipped rectangle inside the surface. */
static void add_damage(struct surface *surface, struct surface_rect rectangle)
{
    uint32_t left;
    uint32_t top;
    uint32_t right;
    uint32_t bottom;

    if (rectangle.width == 0U || rectangle.height == 0U) {
        return;
    }

    if (!surface->damage.pending) {
        surface->damage.pending = true;
        surface->damage.rectangle = rectangle;
        return;
    }

    left = minimum(surface->damage.rectangle.x, rectangle.x);
    top = minimum(surface->damage.rectangle.y, rectangle.y);
    right = surface->damage.rectangle.x +
        surface->damage.rectangle.width;
    bottom = surface->damage.rectangle.y +
        surface->damage.rectangle.height;

    if (rectangle.x + rectangle.width > right) {
        right = rectangle.x + rectangle.width;
    }

    if (rectangle.y + rectangle.height > bottom) {
        bottom = rectangle.y + rectangle.height;
    }

    surface->damage.rectangle.x = left;
    surface->damage.rectangle.y = top;
    surface->damage.rectangle.width = right - left;
    surface->damage.rectangle.height = bottom - top;
}

static enum surface_status clip_rectangle(
    const struct surface *surface,
    struct surface_rect requested,
    struct surface_rect *clipped
)
{
    *clipped = requested;

    if (requested.width == 0U || requested.height == 0U) {
        clipped->width = 0U;
        clipped->height = 0U;
        return SURFACE_STATUS_OK;
    }

    if (requested.x >= surface->width || requested.y >= surface->height) {
        return SURFACE_STATUS_OUT_OF_BOUNDS;
    }

    /* Refuse the arithmetic mistake even when clipping could hide it. */
    if (requested.width > UINT32_MAX - requested.x ||
        requested.height > UINT32_MAX - requested.y) {
        return SURFACE_STATUS_RECTANGLE_OVERFLOW;
    }

    clipped->width = minimum(requested.width, surface->width - requested.x);
    clipped->height = minimum(requested.height, surface->height - requested.y);
    return SURFACE_STATUS_OK;
}

static enum surface_status bind_surface(
    struct surface *surface,
    uint32_t width,
    uint32_t height,
    uint32_t pitch,
    uint32_t *pixels
)
{
    if (surface == NULL || pixels == NULL) {
        return SURFACE_STATUS_NULL_ARGUMENT;
    }

    if (width == 0U || height == 0U ||
        width > UINT32_MAX / SURFACE_BYTES_PER_PIXEL ||
        pitch < width * SURFACE_BYTES_PER_PIXEL ||
        pitch % SURFACE_BYTES_PER_PIXEL != 0U) {
        return SURFACE_STATUS_BAD_GEOMETRY;
    }

    surface->active = true;
    surface->width = width;
    surface->height = height;
    surface->pitch = pitch;
    surface->pixels = pixels;
    surface->presents = 0U;
    surface->last_present_pixels = 0U;
    surface->presented_pixels = 0U;
    clear_damage(surface);
    return SURFACE_STATUS_OK;
}

enum surface_status surface_initialize(
    struct surface *surface,
    uint32_t width,
    uint32_t height
)
{
    uint32_t pitch;
    uint64_t size;
    void *allocation = NULL;

    if (surface == NULL) {
        return SURFACE_STATUS_NULL_ARGUMENT;
    }

    if (surface->active) {
        return SURFACE_STATUS_ALREADY_INITIALIZED;
    }

    if (width == 0U || height == 0U) {
        return SURFACE_STATUS_BAD_GEOMETRY;
    }

    if (width > UINT32_MAX / SURFACE_BYTES_PER_PIXEL) {
        return SURFACE_STATUS_SIZE_OVERFLOW;
    }

    pitch = width * SURFACE_BYTES_PER_PIXEL;
    size = (uint64_t)pitch * height;

    if (size > HEAP_SIZE) {
        return SURFACE_STATUS_SIZE_OVERFLOW;
    }

    if (heap_allocate(size, &allocation) != HEAP_STATUS_OK) {
        return SURFACE_STATUS_ALLOCATION_FAILURE;
    }

    return bind_surface(surface, width, height, pitch,
        (uint32_t *)allocation);
}

enum surface_status surface_release(struct surface *surface)
{
    if (surface == NULL) {
        return SURFACE_STATUS_NULL_ARGUMENT;
    }

    if (!surface->active) {
        return SURFACE_STATUS_NOT_INITIALIZED;
    }

    if (heap_free(surface->pixels) != HEAP_STATUS_OK) {
        return SURFACE_STATUS_RELEASE_FAILURE;
    }

    surface->active = false;
    surface->width = 0U;
    surface->height = 0U;
    surface->pitch = 0U;
    surface->pixels = NULL;
    surface->presents = 0U;
    surface->last_present_pixels = 0U;
    surface->presented_pixels = 0U;
    clear_damage(surface);
    return SURFACE_STATUS_OK;
}

enum surface_status surface_fill_rect(
    struct surface *surface,
    struct surface_rect rectangle,
    uint32_t pixel
)
{
    struct surface_rect clipped;
    enum surface_status status;

    if (surface == NULL) {
        return SURFACE_STATUS_NULL_ARGUMENT;
    }

    if (!surface->active) {
        return SURFACE_STATUS_NOT_INITIALIZED;
    }

    status = clip_rectangle(surface, rectangle, &clipped);

    if (status != SURFACE_STATUS_OK || clipped.width == 0U ||
        clipped.height == 0U) {
        return status;
    }

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        uint32_t *row = row_at(surface, clipped.y + y);

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            row[clipped.x + x] = pixel;
        }
    }

    add_damage(surface, clipped);
    return SURFACE_STATUS_OK;
}

enum surface_status surface_blit(
    struct surface *surface,
    uint32_t destination_x,
    uint32_t destination_y,
    const uint32_t *source,
    uint32_t source_width,
    uint32_t source_height,
    uint32_t source_pitch
)
{
    struct surface_rect requested;
    struct surface_rect clipped;
    enum surface_status status;

    if (surface == NULL || source == NULL) {
        return SURFACE_STATUS_NULL_ARGUMENT;
    }

    if (!surface->active) {
        return SURFACE_STATUS_NOT_INITIALIZED;
    }

    if (source_width == 0U || source_height == 0U) {
        return SURFACE_STATUS_OK;
    }

    if (source_width > UINT32_MAX / SURFACE_BYTES_PER_PIXEL ||
        source_pitch < source_width * SURFACE_BYTES_PER_PIXEL ||
        source_pitch % SURFACE_BYTES_PER_PIXEL != 0U) {
        return SURFACE_STATUS_BAD_SOURCE_PITCH;
    }

    requested.x = destination_x;
    requested.y = destination_y;
    requested.width = source_width;
    requested.height = source_height;
    status = clip_rectangle(surface, requested, &clipped);

    if (status != SURFACE_STATUS_OK) {
        return status;
    }

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        uint32_t *destination = row_at(surface, clipped.y + y);
        const uint32_t *source_row = (const uint32_t *)(const void *)(
            (const uint8_t *)(const void *)source + (uint64_t)y * source_pitch
        );

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            destination[clipped.x + x] = source_row[x];
        }
    }

    add_damage(surface, clipped);
    return SURFACE_STATUS_OK;
}

enum surface_status surface_copy_rect(
    struct surface *surface,
    struct surface_rect source,
    uint32_t destination_x,
    uint32_t destination_y
)
{
    struct surface_rect source_clipped;
    struct surface_rect destination;
    struct surface_rect destination_clipped;
    uint32_t copy_width;
    uint32_t copy_height;
    enum surface_status status;

    if (surface == NULL) {
        return SURFACE_STATUS_NULL_ARGUMENT;
    }

    if (!surface->active) {
        return SURFACE_STATUS_NOT_INITIALIZED;
    }

    status = clip_rectangle(surface, source, &source_clipped);

    if (status != SURFACE_STATUS_OK || source_clipped.width == 0U ||
        source_clipped.height == 0U) {
        return status;
    }

    destination.x = destination_x;
    destination.y = destination_y;
    destination.width = source.width;
    destination.height = source.height;
    status = clip_rectangle(surface, destination, &destination_clipped);

    if (status != SURFACE_STATUS_OK) {
        return status;
    }

    copy_width = minimum(source_clipped.width, destination_clipped.width);
    copy_height = minimum(source_clipped.height, destination_clipped.height);
    destination_clipped.width = copy_width;
    destination_clipped.height = copy_height;

    /*
     * A destination below its source must start at the bottom. Equal rows then
     * choose horizontally: moving right starts at the right edge. These are
     * the two decisions that make every overlap behave like memmove.
     */
    if (destination_y > source.y) {
        for (uint32_t remaining = copy_height; remaining > 0U; --remaining) {
            const uint32_t y = remaining - 1U;
            uint32_t *destination_row = row_at(surface, destination_y + y);
            const uint32_t *source_row = const_row_at(surface, source.y + y);

            if (destination_y == source.y && destination_x > source.x) {
                for (uint32_t x_remaining = copy_width;
                     x_remaining > 0U;
                     --x_remaining) {
                    const uint32_t x = x_remaining - 1U;
                    destination_row[destination_x + x] =
                        source_row[source.x + x];
                }
            } else {
                for (uint32_t x = 0U; x < copy_width; ++x) {
                    destination_row[destination_x + x] =
                        source_row[source.x + x];
                }
            }
        }
    } else {
        for (uint32_t y = 0U; y < copy_height; ++y) {
            uint32_t *destination_row = row_at(surface, destination_y + y);
            const uint32_t *source_row = const_row_at(surface, source.y + y);

            if (destination_y == source.y && destination_x > source.x) {
                for (uint32_t remaining = copy_width;
                     remaining > 0U;
                     --remaining) {
                    const uint32_t x = remaining - 1U;
                    destination_row[destination_x + x] =
                        source_row[source.x + x];
                }
            } else {
                for (uint32_t x = 0U; x < copy_width; ++x) {
                    destination_row[destination_x + x] =
                        source_row[source.x + x];
                }
            }
        }
    }

    add_damage(surface, destination_clipped);
    return SURFACE_STATUS_OK;
}

enum surface_status surface_pixel(
    struct surface *surface,
    uint32_t x,
    uint32_t y,
    uint32_t pixel
)
{
    struct surface_rect damage;

    if (surface == NULL) {
        return SURFACE_STATUS_NULL_ARGUMENT;
    }

    if (!surface->active) {
        return SURFACE_STATUS_NOT_INITIALIZED;
    }

    if (x >= surface->width || y >= surface->height) {
        return SURFACE_STATUS_OUT_OF_BOUNDS;
    }

    row_at(surface, y)[x] = pixel;
    damage.x = x;
    damage.y = y;
    damage.width = 1U;
    damage.height = 1U;
    add_damage(surface, damage);
    return SURFACE_STATUS_OK;
}

enum surface_status surface_read_pixel(
    const struct surface *surface,
    uint32_t x,
    uint32_t y,
    uint32_t *pixel
)
{
    if (surface == NULL || pixel == NULL) {
        return SURFACE_STATUS_NULL_ARGUMENT;
    }

    *pixel = 0U;

    if (!surface->active) {
        return SURFACE_STATUS_NOT_INITIALIZED;
    }

    if (x >= surface->width || y >= surface->height) {
        return SURFACE_STATUS_OUT_OF_BOUNDS;
    }

    *pixel = const_row_at(surface, y)[x];
    return SURFACE_STATUS_OK;
}

enum surface_status surface_present(struct surface *surface)
{
    struct framebuffer_state framebuffer;
    const struct surface_rect damage = surface == NULL ?
        (struct surface_rect){ 0U, 0U, 0U, 0U } :
        surface->damage.rectangle;
    uint64_t copied = 0U;

    if (surface == NULL) {
        return SURFACE_STATUS_NULL_ARGUMENT;
    }

    if (!surface->active) {
        return SURFACE_STATUS_NOT_INITIALIZED;
    }

    if (!framebuffer_is_active()) {
        return SURFACE_STATUS_NO_FRAMEBUFFER;
    }

    framebuffer = framebuffer_get_state();

    if (framebuffer.width != surface->width ||
        framebuffer.height != surface->height) {
        return SURFACE_STATUS_FRAMEBUFFER_MISMATCH;
    }

    if (!surface->damage.pending) {
        surface->last_present_pixels = 0U;
        return SURFACE_STATUS_OK;
    }

    for (uint32_t y = 0U; y < damage.height; ++y) {
        const uint32_t *source = const_row_at(surface, damage.y + y);
        volatile uint32_t *destination =
            (volatile uint32_t *)(uintptr_t)(framebuffer.address +
                (uint64_t)(damage.y + y) * framebuffer.pitch) + damage.x;

        /*
         * The rectangle was bounded against both matching geometries above,
         * so repeating the framebuffer API's coordinate checks for every
         * pixel would add a function call without adding another boundary.
         * Volatile is still essential: this destination is device memory and
         * the compiler cannot otherwise observe that these stores matter.
         */
        for (uint32_t x = 0U; x < damage.width; ++x) {
            destination[x] = source[damage.x + x];
            ++copied;
        }
    }

    surface->presents += 1U;
    surface->last_present_pixels = copied;
    surface->presented_pixels += copied;
    clear_damage(surface);
    return SURFACE_STATUS_OK;
}

enum surface_status surface_verify(const struct surface *surface)
{
    struct heap_state heap;
    uint64_t address;
    uint64_t size;

    if (surface == NULL) {
        return SURFACE_STATUS_NULL_ARGUMENT;
    }

    if (!surface->active) {
        return SURFACE_STATUS_NOT_INITIALIZED;
    }

    if (surface->pixels == NULL || surface->width == 0U ||
        surface->height == 0U ||
        surface->width > UINT32_MAX / SURFACE_BYTES_PER_PIXEL ||
        surface->pitch != surface->width * SURFACE_BYTES_PER_PIXEL) {
        return SURFACE_STATUS_BAD_GEOMETRY;
    }

    if (surface->damage.pending &&
        (surface->damage.rectangle.width == 0U ||
         surface->damage.rectangle.height == 0U ||
         surface->damage.rectangle.x >= surface->width ||
         surface->damage.rectangle.y >= surface->height ||
         surface->damage.rectangle.width >
            surface->width - surface->damage.rectangle.x ||
         surface->damage.rectangle.height >
            surface->height - surface->damage.rectangle.y)) {
        return SURFACE_STATUS_VALIDATION_FAILURE;
    }

    heap = heap_get_state();
    address = (uint64_t)(uintptr_t)surface->pixels;
    size = (uint64_t)surface->pitch * surface->height;

    if (!heap.active || address < heap.base_address ||
        address > heap.base_address + heap.committed_bytes ||
        size > heap.base_address + heap.committed_bytes - address) {
        return SURFACE_STATUS_VALIDATION_FAILURE;
    }

    return SURFACE_STATUS_OK;
}

const char *surface_status_string(enum surface_status status)
{
    static const char *const messages[] = {
        "ok",
        "null surface argument",
        "surface is already initialized",
        "surface is not initialized",
        "surface geometry is inconsistent",
        "surface size arithmetic overflowed",
        "surface pixel buffer could not be allocated",
        "surface pixel buffer could not be released",
        "coordinate is outside the surface",
        "surface rectangle arithmetic overflowed",
        "source pitch cannot hold one row",
        "no framebuffer to present to",
        "surface and framebuffer dimensions differ",
        "surface damage could not be presented",
        "surface state is inconsistent"
    };

    _Static_assert(
        sizeof(messages) / sizeof(messages[0]) ==
            (size_t)SURFACE_STATUS_VALIDATION_FAILURE + 1U,
        "surface status messages are out of sync"
    );

    if ((size_t)status >= sizeof(messages) / sizeof(messages[0])) {
        return "unknown surface status";
    }

    return messages[status];
}

static void seed_fixture(struct surface *surface, uint32_t *pixels)
{
    for (uint32_t y = 0U; y < surface->height; ++y) {
        uint32_t *row = row_at(surface, y);

        for (uint32_t x = 0U; x < surface->width; ++x) {
            row[x] = y * 16U + x;
        }

        /* Padding is not a pixel and no primitive may touch it. */
        row[6] = UINT32_C(0xA5A5A5A5);
        row[7] = UINT32_C(0x5A5A5A5A);
    }

    (void)pixels;
    clear_damage(surface);
}

static bool fill_and_damage_are_right(void)
{
    uint32_t pixels[5U * 8U];
    struct surface surface = { 0 };
    struct surface_rect rectangle = { 4U, 3U, 4U, 4U };

    if (bind_surface(&surface, 6U, 5U, 8U * SURFACE_BYTES_PER_PIXEL,
            pixels) != SURFACE_STATUS_OK) {
        return false;
    }

    seed_fixture(&surface, pixels);

    if (surface_fill_rect(&surface, rectangle, UINT32_C(0x11223344)) !=
        SURFACE_STATUS_OK) {
        return false;
    }

    if (!surface.damage.pending || surface.damage.rectangle.x != 4U ||
        surface.damage.rectangle.y != 3U ||
        surface.damage.rectangle.width != 2U ||
        surface.damage.rectangle.height != 2U) {
        return false;
    }

    for (uint32_t y = 0U; y < surface.height; ++y) {
        const uint32_t *row = const_row_at(&surface, y);

        if (row[6] != UINT32_C(0xA5A5A5A5) ||
            row[7] != UINT32_C(0x5A5A5A5A)) {
            return false;
        }
    }

    rectangle.x = 5U;
    rectangle.y = 0U;
    rectangle.width = UINT32_MAX;
    rectangle.height = 1U;
    return surface_fill_rect(&surface, rectangle, 0U) ==
        SURFACE_STATUS_RECTANGLE_OVERFLOW;
}

static bool blit_stride_is_right(void)
{
    static const uint32_t source[] = {
        11U, 12U, UINT32_C(0xDEADBEEF),
        21U, 22U, UINT32_C(0xCAFEBABE)
    };
    uint32_t pixels[5U * 8U];
    struct surface surface = { 0 };

    if (bind_surface(&surface, 6U, 5U, 8U * SURFACE_BYTES_PER_PIXEL,
            pixels) != SURFACE_STATUS_OK) {
        return false;
    }

    seed_fixture(&surface, pixels);

    if (surface_blit(&surface, 1U, 1U, source, 2U, 2U,
            3U * SURFACE_BYTES_PER_PIXEL) != SURFACE_STATUS_OK) {
        return false;
    }

    return const_row_at(&surface, 1U)[1] == 11U &&
        const_row_at(&surface, 1U)[2] == 12U &&
        const_row_at(&surface, 2U)[1] == 21U &&
        const_row_at(&surface, 2U)[2] == 22U;
}

static bool overlap_directions_are_right(void)
{
    uint32_t pixels[5U * 8U];
    struct surface surface = { 0 };
    struct surface_rect source = { 0U, 0U, 4U, 4U };

    if (bind_surface(&surface, 6U, 5U, 8U * SURFACE_BYTES_PER_PIXEL,
            pixels) != SURFACE_STATUS_OK) {
        return false;
    }

    seed_fixture(&surface, pixels);

    if (surface_copy_rect(&surface, source, 1U, 1U) != SURFACE_STATUS_OK) {
        return false;
    }

    for (uint32_t y = 0U; y < 4U; ++y) {
        for (uint32_t x = 0U; x < 4U; ++x) {
            if (const_row_at(&surface, y + 1U)[x + 1U] != y * 16U + x) {
                return false;
            }
        }
    }

    seed_fixture(&surface, pixels);
    source.x = 1U;
    source.y = 1U;

    if (surface_copy_rect(&surface, source, 0U, 0U) != SURFACE_STATUS_OK) {
        return false;
    }

    for (uint32_t y = 0U; y < 4U; ++y) {
        for (uint32_t x = 0U; x < 4U; ++x) {
            if (const_row_at(&surface, y)[x] !=
                (y + 1U) * 16U + x + 1U) {
                return false;
            }
        }
    }

    return true;
}

static bool pixels_and_refusals_are_right(void)
{
    static const enum surface_status every[] = {
        SURFACE_STATUS_OK,
        SURFACE_STATUS_NULL_ARGUMENT,
        SURFACE_STATUS_ALREADY_INITIALIZED,
        SURFACE_STATUS_NOT_INITIALIZED,
        SURFACE_STATUS_BAD_GEOMETRY,
        SURFACE_STATUS_SIZE_OVERFLOW,
        SURFACE_STATUS_ALLOCATION_FAILURE,
        SURFACE_STATUS_RELEASE_FAILURE,
        SURFACE_STATUS_OUT_OF_BOUNDS,
        SURFACE_STATUS_RECTANGLE_OVERFLOW,
        SURFACE_STATUS_BAD_SOURCE_PITCH,
        SURFACE_STATUS_NO_FRAMEBUFFER,
        SURFACE_STATUS_FRAMEBUFFER_MISMATCH,
        SURFACE_STATUS_PRESENT_FAILURE,
        SURFACE_STATUS_VALIDATION_FAILURE
    };
    uint32_t pixels[5U * 8U];
    struct surface surface = { 0 };
    uint32_t pixel = 0U;

    if (surface_pixel(&surface, 0U, 0U, 0U) !=
            SURFACE_STATUS_NOT_INITIALIZED ||
        surface_read_pixel(&surface, 0U, 0U, &pixel) !=
            SURFACE_STATUS_NOT_INITIALIZED ||
        surface_read_pixel(NULL, 0U, 0U, &pixel) !=
            SURFACE_STATUS_NULL_ARGUMENT ||
        surface_read_pixel(&surface, 0U, 0U, NULL) !=
            SURFACE_STATUS_NULL_ARGUMENT) {
        return false;
    }

    if (bind_surface(&surface, 6U, 5U, 8U * SURFACE_BYTES_PER_PIXEL,
            pixels) != SURFACE_STATUS_OK) {
        return false;
    }

    seed_fixture(&surface, pixels);

    if (surface_pixel(&surface, 5U, 4U, UINT32_C(0x12345678)) !=
            SURFACE_STATUS_OK ||
        surface_read_pixel(&surface, 5U, 4U, &pixel) != SURFACE_STATUS_OK ||
        pixel != UINT32_C(0x12345678) ||
        surface_pixel(&surface, 6U, 0U, 0U) !=
            SURFACE_STATUS_OUT_OF_BOUNDS ||
        surface_read_pixel(&surface, 0U, 5U, &pixel) !=
            SURFACE_STATUS_OUT_OF_BOUNDS) {
        return false;
    }

    for (size_t index = 0U; index < sizeof(every) / sizeof(every[0]);
         ++index) {
        const char *message = surface_status_string(every[index]);

        if (message == NULL || message[0] == '\0') {
            return false;
        }
    }

    return surface_status_string((enum surface_status)99) != NULL;
}

bool surface_self_test(void)
{
    return fill_and_damage_are_right() && blit_stride_is_right() &&
        overlap_directions_are_right() && pixels_and_refusals_are_right();
}
