/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SENERI_KEYBOARD_H
#define SENERI_KEYBOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The PS/2 keyboard, through the 8042 controller.
 *
 * This is the first device Seneri talks to that a person operates. Everything
 * before it was discovered, counted, or timed; this one waits.
 *
 * The 8042 is not a keyboard. It is a controller with two ports, a command
 * register and a status register, and it long ago stopped being an actual
 * physical part - on anything modern it is emulated by the platform controller
 * hub or by a system management handler. That matters here because it means its
 * timings are nothing like a datasheet's, so every wait in this driver is
 * bounded and every bound is a refusal rather than a spin.
 */

enum keyboard_status {
    KEYBOARD_STATUS_OK = 0,
    KEYBOARD_STATUS_ALREADY_INITIALIZED,
    KEYBOARD_STATUS_NOT_INITIALIZED,
    KEYBOARD_STATUS_INTERRUPTS_ENABLED,
    KEYBOARD_STATUS_NO_IOAPIC,
    KEYBOARD_STATUS_CONTROLLER_TIMEOUT,
    KEYBOARD_STATUS_CONTROLLER_REFUSED,
    KEYBOARD_STATUS_SELF_TEST_FAILED,
    KEYBOARD_STATUS_PORT_TEST_FAILED,
    KEYBOARD_STATUS_IOAPIC_FAILURE,
    KEYBOARD_STATUS_HANDLER_FAILURE,
    KEYBOARD_STATUS_EMPTY
};

/*
 * One key event. Seneri reports both presses and releases rather than only the
 * characters that resulted, because a shell needs characters and anything that
 * ever draws a cursor or handles a held key needs the edges.
 */
struct keyboard_event {
    uint8_t scancode;   /* set 1, with the release bit already stripped */
    bool pressed;       /* false for a release */
    char character;     /* '\0' when the key produces no character */
};

struct keyboard_state {
    bool active;
    uint64_t interrupts;    /* how many times IRQ 1 has been taken */
    uint64_t events;        /* how many events reached the queue */
    uint64_t dropped;       /* how many were lost to a full queue */
    uint64_t extended;      /* how many 0xE0 prefixes were seen */
    size_t queued;          /* how many are waiting to be read now */
    bool shift;
    bool caps_lock;
};

/*
 * Take the controller, route IRQ 1, and install the handler. Must run with
 * interrupts disabled, like every other bring-up in this kernel, because the
 * controller is configured across several writes and an interrupt arriving
 * between them would be delivered to a handler that is not installed yet.
 */
enum keyboard_status keyboard_initialize(void);

bool keyboard_is_initialized(void);

/*
 * Take the oldest event. Returns KEYBOARD_STATUS_EMPTY rather than blocking:
 * there is no scheduler contract here yet, and a driver that blocks before
 * anything can wake it is a driver that hangs.
 */
enum keyboard_status keyboard_read(struct keyboard_event *event);

struct keyboard_state keyboard_get_state(void);

/*
 * Push a byte into the controller's output buffer as though the keyboard had
 * sent it, which raises IRQ 1. This is how boot proves the interrupt path
 * without a person at the machine, and it is a real controller command rather
 * than a test hook: 0xD2, "write keyboard output buffer".
 */
enum keyboard_status keyboard_inject_scancode(uint8_t scancode);

/* Translate one set 1 scancode as if it arrived now, without touching state. */
char keyboard_character_for(uint8_t scancode, bool shift, bool caps_lock);

bool keyboard_self_test(void);
const char *keyboard_status_string(enum keyboard_status status);

#endif
