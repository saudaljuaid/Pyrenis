/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SENERI_FRAMEBUFFER_H
#define SENERI_FRAMEBUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/boot.h>

/*
 * Every pixel on the screen, addressable.
 *
 * This is the lowest layer of anything graphical and it deliberately stops
 * there: it can put a colour at a coordinate and read it back, and it has no
 * idea what a character, a window or a cursor is. Those are built on this, and
 * each is its own increment.
 *
 * The claim it makes is narrow and completely checkable without looking at a
 * screen, which matters because CONTRIBUTING.md says screenshots are not proof:
 * for every visible coordinate, the colour written there is the colour read
 * back, and no coordinate aliases another.
 */

/*
 * A pixel is four bytes and the three channels are whole bytes at byte
 * boundaries, which multiboot2.c has already refused anything else for. The
 * fourth byte is not a channel the loader described, so nothing is claimed
 * about it and it is excluded from every comparison.
 */
#define FRAMEBUFFER_BYTES_PER_PIXEL BOOT_FRAMEBUFFER_BYTES_PER_PIXEL
#define FRAMEBUFFER_CHANNEL_MASK UINT32_C(0xFF)

enum framebuffer_status {
    FRAMEBUFFER_STATUS_OK = 0,
    FRAMEBUFFER_STATUS_NULL_ARGUMENT,
    FRAMEBUFFER_STATUS_ALREADY_INITIALIZED,
    FRAMEBUFFER_STATUS_NOT_INITIALIZED,
    FRAMEBUFFER_STATUS_ABSENT,
    FRAMEBUFFER_STATUS_NOT_MAPPED,
    FRAMEBUFFER_STATUS_BAD_GEOMETRY,
    FRAMEBUFFER_STATUS_OUT_OF_BOUNDS,
    FRAMEBUFFER_STATUS_VALIDATION_FAILURE
};

struct framebuffer_state {
    bool active;
    uint64_t address;
    uint64_t size;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t red_position;
    uint8_t green_position;
    uint8_t blue_position;
};

/*
 * Adopt the framebuffer the loader set, if paging was able to make it device
 * memory. Absence is not a failure: a loader that set no graphics mode leaves
 * the kernel on the serial console it has used since day one.
 */
enum framebuffer_status framebuffer_initialize(
    const struct boot_framebuffer *framebuffer
);

bool framebuffer_is_active(void);

/*
 * Compose a pixel from three channels using the positions the loader reported,
 * rather than assuming a byte order. The bits outside the three channels are
 * left zero and never compared.
 */
uint32_t framebuffer_pack(uint8_t red, uint8_t green, uint8_t blue);

/* Every bit any channel occupies, and therefore every bit worth comparing. */
uint32_t framebuffer_visible_mask(void);

enum framebuffer_status framebuffer_write_pixel(
    uint32_t x,
    uint32_t y,
    uint32_t pixel
);
enum framebuffer_status framebuffer_read_pixel(
    uint32_t x,
    uint32_t y,
    uint32_t *pixel
);
enum framebuffer_status framebuffer_fill(uint32_t pixel);

struct framebuffer_state framebuffer_get_state(void);
enum framebuffer_status framebuffer_verify(void);
bool framebuffer_self_test(void);
const char *framebuffer_status_string(enum framebuffer_status status);

#endif
