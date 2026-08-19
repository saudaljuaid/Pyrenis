/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SENERI_SURFACE_H
#define SENERI_SURFACE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Pixels in ordinary write-back memory.
 *
 * The framebuffer is device memory, so reading it is the wrong way to move a
 * picture. A surface keeps the picture in cached RAM, records the smallest
 * rectangle changed since the last present, and writes only that rectangle to
 * the framebuffer. It knows nothing about glyphs, windows or cursors.
 */

#define SURFACE_BYTES_PER_PIXEL UINT32_C(4)

enum surface_status {
    SURFACE_STATUS_OK = 0,
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

struct surface_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

struct surface_damage {
    bool pending;
    struct surface_rect rectangle;
};

/*
 * Callers zero-initialize this object before its first initialization. The
 * counters make the damage promise observable: a caller can prove a one-line
 * change copied one line rather than trusting that the picture looked right.
 */
struct surface {
    bool active;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t *pixels;
    struct surface_damage damage;
    uint64_t presents;
    uint64_t last_present_pixels;
    uint64_t presented_pixels;
};

/* Allocate a tightly packed pixel buffer from the kernel heap. */
enum surface_status surface_initialize(
    struct surface *surface,
    uint32_t width,
    uint32_t height
);

enum surface_status surface_release(struct surface *surface);

/*
 * Partly visible rectangles are clipped. A coordinate addition that would
 * wrap UINT32_MAX is refused before clipping, because wrapping is not a shape.
 * Empty rectangles are successful no-ops and do not create damage.
 */
enum surface_status surface_fill_rect(
    struct surface *surface,
    struct surface_rect rectangle,
    uint32_t pixel
);

/*
 * Copy an entire source image to a destination origin. Source pitch is bytes
 * between rows and may exceed source_width * four; destination clipping never
 * changes which source pixel belongs at the origin.
 */
enum surface_status surface_blit(
    struct surface *surface,
    uint32_t destination_x,
    uint32_t destination_y,
    const uint32_t *source,
    uint32_t source_width,
    uint32_t source_height,
    uint32_t source_pitch
);

/* Copy inside one surface, preserving both possible overlap directions. */
enum surface_status surface_copy_rect(
    struct surface *surface,
    struct surface_rect source,
    uint32_t destination_x,
    uint32_t destination_y
);

enum surface_status surface_pixel(
    struct surface *surface,
    uint32_t x,
    uint32_t y,
    uint32_t pixel
);

enum surface_status surface_read_pixel(
    const struct surface *surface,
    uint32_t x,
    uint32_t y,
    uint32_t *pixel
);

/* Copy only the damage bounding rectangle, then clear it after success. */
enum surface_status surface_present(struct surface *surface);

enum surface_status surface_verify(const struct surface *surface);
bool surface_self_test(void);
const char *surface_status_string(enum surface_status status);

#endif
