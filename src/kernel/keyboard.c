/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/cpu.h>
#include <seneri/interrupts.h>
#include <seneri/ioapic.h>
#include <seneri/keyboard.h>

/*
 * The PS/2 keyboard.
 *
 * Three things about the 8042 shape this driver and none of them are obvious
 * from a register list.
 *
 * It is not a keyboard and on modern hardware it is not even a chip. It is
 * emulated, so its timings bear no relation to any datasheet, and a driver that
 * waits for it without a bound is a driver that can hang the machine on a
 * platform that decided not to answer. Every wait here counts down.
 *
 * Its status register's two ready bits point opposite ways. Bit 0 means there
 * is a byte to read; bit 1 means the input buffer is still full and a write
 * would be lost. Reading when bit 0 is clear returns stale data rather than
 * failing, which is why nothing here reads without checking.
 *
 * And the scancodes are not characters. Set 1 numbers keys by position, so the
 * table below maps positions on a US layout; a different layout is a different
 * table and nothing else.
 */

#define KEYBOARD_DATA_PORT UINT16_C(0x0060)
#define KEYBOARD_COMMAND_PORT UINT16_C(0x0064)

#define KEYBOARD_STATUS_OUTPUT_FULL UINT8_C(0x01)
#define KEYBOARD_STATUS_INPUT_FULL UINT8_C(0x02)

#define KEYBOARD_COMMAND_READ_CONFIGURATION UINT8_C(0x20)
#define KEYBOARD_COMMAND_WRITE_CONFIGURATION UINT8_C(0x60)
#define KEYBOARD_COMMAND_DISABLE_SECOND_PORT UINT8_C(0xA7)
#define KEYBOARD_COMMAND_DISABLE_FIRST_PORT UINT8_C(0xAD)
#define KEYBOARD_COMMAND_ENABLE_FIRST_PORT UINT8_C(0xAE)
#define KEYBOARD_COMMAND_SELF_TEST UINT8_C(0xAA)
#define KEYBOARD_COMMAND_TEST_FIRST_PORT UINT8_C(0xAB)
#define KEYBOARD_COMMAND_WRITE_OUTPUT_BUFFER UINT8_C(0xD2)

#define KEYBOARD_SELF_TEST_PASSED UINT8_C(0x55)
#define KEYBOARD_PORT_TEST_PASSED UINT8_C(0x00)

#define KEYBOARD_CONFIGURATION_FIRST_INTERRUPT UINT8_C(0x01)
#define KEYBOARD_CONFIGURATION_SECOND_INTERRUPT UINT8_C(0x02)
#define KEYBOARD_CONFIGURATION_FIRST_CLOCK_OFF UINT8_C(0x10)
#define KEYBOARD_CONFIGURATION_TRANSLATE UINT8_C(0x40)

/*
 * How many status reads a wait is allowed. The controller answers in
 * microseconds when it answers at all, and this is several milliseconds of
 * port reads, so reaching the bound means the controller is not there rather
 * than that it is slow.
 */
#define KEYBOARD_WAIT_LIMIT UINT32_C(100000)

#define KEYBOARD_IRQ UINT8_C(1)
#define KEYBOARD_VECTOR ((uint8_t)(INTERRUPT_IOAPIC_BASE + KEYBOARD_IRQ))

/* Set 1 marks a release by setting the top bit of the make code. */
#define KEYBOARD_RELEASE_BIT UINT8_C(0x80)
#define KEYBOARD_EXTENDED_PREFIX UINT8_C(0xE0)

#define KEYBOARD_SCANCODE_LEFT_SHIFT UINT8_C(0x2A)
#define KEYBOARD_SCANCODE_RIGHT_SHIFT UINT8_C(0x36)
#define KEYBOARD_SCANCODE_CAPS_LOCK UINT8_C(0x3A)

/*
 * A power of two so the wrap is a mask. Sixty-four events is far more than any
 * person generates between reads, and the count of what was dropped is kept so
 * a full queue is visible rather than silent.
 */
#define KEYBOARD_QUEUE_SIZE 64U
#define KEYBOARD_QUEUE_MASK (KEYBOARD_QUEUE_SIZE - 1U)

_Static_assert(
    (KEYBOARD_QUEUE_SIZE & KEYBOARD_QUEUE_MASK) == 0U,
    "the keyboard queue size must be a power of two"
);

/*
 * Scancode set 1, US layout, unshifted then shifted. Index is the make code;
 * a zero means the key produces no character, which covers the modifiers, the
 * function keys and everything this table does not reach.
 */
static const char unshifted[128] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0, '*', 0, ' '
};

static const char shifted[128] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0, '*', 0, ' '
};

static struct keyboard_state state;
static struct keyboard_event queue[KEYBOARD_QUEUE_SIZE];
static volatile size_t queue_head;
static volatile size_t queue_tail;
static bool expecting_extended;

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

static uint8_t controller_status(void)
{
    return inb(KEYBOARD_COMMAND_PORT);
}

/* Wait until a write would not be lost. Bounded, and the bound is a refusal. */
static bool wait_to_write(void)
{
    for (uint32_t spins = 0; spins < KEYBOARD_WAIT_LIMIT; ++spins) {
        if ((controller_status() & KEYBOARD_STATUS_INPUT_FULL) == 0U) {
            return true;
        }
    }

    return false;
}

/* Wait until there is something to read. Same bound, same reason. */
static bool wait_to_read(void)
{
    for (uint32_t spins = 0; spins < KEYBOARD_WAIT_LIMIT; ++spins) {
        if ((controller_status() & KEYBOARD_STATUS_OUTPUT_FULL) != 0U) {
            return true;
        }
    }

    return false;
}

static enum keyboard_status send_command(uint8_t command)
{
    if (!wait_to_write()) {
        return KEYBOARD_STATUS_CONTROLLER_TIMEOUT;
    }

    outb(KEYBOARD_COMMAND_PORT, command);
    return KEYBOARD_STATUS_OK;
}

static enum keyboard_status send_data(uint8_t value)
{
    if (!wait_to_write()) {
        return KEYBOARD_STATUS_CONTROLLER_TIMEOUT;
    }

    outb(KEYBOARD_DATA_PORT, value);
    return KEYBOARD_STATUS_OK;
}

static enum keyboard_status receive_data(uint8_t *value)
{
    if (!wait_to_read()) {
        return KEYBOARD_STATUS_CONTROLLER_TIMEOUT;
    }

    *value = inb(KEYBOARD_DATA_PORT);
    return KEYBOARD_STATUS_OK;
}

/*
 * Read and throw away whatever the controller is holding. Firmware leaves a
 * byte in there often enough that skipping this makes the first real key look
 * like the last one the BIOS saw.
 */
static void drain(void)
{
    for (uint32_t spins = 0; spins < KEYBOARD_QUEUE_SIZE; ++spins) {
        if ((controller_status() & KEYBOARD_STATUS_OUTPUT_FULL) == 0U) {
            return;
        }

        (void)inb(KEYBOARD_DATA_PORT);
    }
}

char keyboard_character_for(uint8_t scancode, bool shift, bool caps_lock)
{
    const uint8_t make = (uint8_t)(scancode & (uint8_t)~KEYBOARD_RELEASE_BIT);
    char character;

    if (make >= sizeof(unshifted)) {
        return '\0';
    }

    character = shift ? shifted[make] : unshifted[make];

    /*
     * Caps lock is not another shift. It applies to letters and to nothing
     * else, which is why it is resolved after the table rather than by picking
     * a different one: shift+caps on a digit is still the shifted digit.
     */
    if (caps_lock) {
        if (character >= 'a' && character <= 'z') {
            character = (char)(character - 'a' + 'A');
        } else if (character >= 'A' && character <= 'Z') {
            character = (char)(character - 'A' + 'a');
        }
    }

    return character;
}

static void enqueue(const struct keyboard_event *event)
{
    const size_t next = (queue_tail + 1U) & KEYBOARD_QUEUE_MASK;

    if (next == queue_head) {
        state.dropped += 1U;
        return;
    }

    queue[queue_tail] = *event;
    queue_tail = next;
    state.events += 1U;
}

/*
 * One byte from the controller. Runs in interrupt context, so it does the least
 * it can: decode, update the modifiers, and put the event in the queue.
 */
static void handle_byte(uint8_t byte)
{
    struct keyboard_event event;
    uint8_t make;

    if (byte == KEYBOARD_EXTENDED_PREFIX) {
        expecting_extended = true;
        state.extended += 1U;
        return;
    }

    /*
     * An extended code shares its number with an ordinary key - the keypad
     * enter is 0xE0 0x1C, the same 0x1C as return. Until this driver has a
     * reason to tell them apart, the prefix is consumed and the code is treated
     * as the key it shares a number with, which is what a US layout wants.
     */
    expecting_extended = false;

    make = (uint8_t)(byte & (uint8_t)~KEYBOARD_RELEASE_BIT);
    event.pressed = (byte & KEYBOARD_RELEASE_BIT) == 0U;
    event.scancode = make;

    if (make == KEYBOARD_SCANCODE_LEFT_SHIFT ||
        make == KEYBOARD_SCANCODE_RIGHT_SHIFT) {
        state.shift = event.pressed;
    } else if (make == KEYBOARD_SCANCODE_CAPS_LOCK && event.pressed) {
        state.caps_lock = !state.caps_lock;
    }

    event.character = event.pressed
        ? keyboard_character_for(make, state.shift, state.caps_lock)
        : '\0';

    enqueue(&event);
}

static void keyboard_interrupt(struct interrupt_frame *frame, void *context)
{
    (void)frame;
    (void)context;

    state.interrupts += 1U;

    /*
     * Drain everything the controller has, not one byte. A shared or coalesced
     * interrupt leaves more than one waiting, and a handler that takes a single
     * byte per interrupt falls permanently behind the person typing.
     */
    for (uint32_t spins = 0; spins < KEYBOARD_QUEUE_SIZE; ++spins) {
        if ((controller_status() & KEYBOARD_STATUS_OUTPUT_FULL) == 0U) {
            return;
        }

        handle_byte(inb(KEYBOARD_DATA_PORT));
    }
}

enum keyboard_status keyboard_initialize(void)
{
    uint8_t configuration = 0U;
    uint8_t answer = 0U;
    enum keyboard_status status;

    if (state.active) {
        return KEYBOARD_STATUS_ALREADY_INITIALIZED;
    }

    /*
     * The controller is configured across half a dozen port writes. An
     * interrupt arriving in the middle of that would reach a handler that is
     * not installed yet, so this refuses to run with interrupts on rather than
     * disabling them behind the caller's back.
     */
    if (cpu_interrupts_enabled()) {
        return KEYBOARD_STATUS_INTERRUPTS_ENABLED;
    }

    if (!ioapic_is_initialized()) {
        return KEYBOARD_STATUS_NO_IOAPIC;
    }

    /* Both ports off first, so nothing arrives while the controller is tested. */
    status = send_command(KEYBOARD_COMMAND_DISABLE_FIRST_PORT);

    if (status != KEYBOARD_STATUS_OK) {
        return status;
    }

    status = send_command(KEYBOARD_COMMAND_DISABLE_SECOND_PORT);

    if (status != KEYBOARD_STATUS_OK) {
        return status;
    }

    drain();

    status = send_command(KEYBOARD_COMMAND_READ_CONFIGURATION);

    if (status != KEYBOARD_STATUS_OK) {
        return status;
    }

    status = receive_data(&configuration);

    if (status != KEYBOARD_STATUS_OK) {
        return status;
    }

    /*
     * Interrupts off and translation on while the controller is being tested.
     * Translation makes the controller hand back set 1 whatever set the
     * keyboard itself is using, which is what the table above expects; without
     * it a keyboard defaulting to set 2 produces plausible nonsense.
     */
    configuration &= (uint8_t)~(KEYBOARD_CONFIGURATION_FIRST_INTERRUPT |
        KEYBOARD_CONFIGURATION_SECOND_INTERRUPT |
        KEYBOARD_CONFIGURATION_FIRST_CLOCK_OFF);
    configuration |= KEYBOARD_CONFIGURATION_TRANSLATE;

    status = send_command(KEYBOARD_COMMAND_WRITE_CONFIGURATION);

    if (status != KEYBOARD_STATUS_OK) {
        return status;
    }

    status = send_data(configuration);

    if (status != KEYBOARD_STATUS_OK) {
        return status;
    }

    status = send_command(KEYBOARD_COMMAND_SELF_TEST);

    if (status != KEYBOARD_STATUS_OK) {
        return status;
    }

    status = receive_data(&answer);

    if (status != KEYBOARD_STATUS_OK) {
        return status;
    }

    if (answer != KEYBOARD_SELF_TEST_PASSED) {
        return KEYBOARD_STATUS_SELF_TEST_FAILED;
    }

    /*
     * A controller self-test is allowed to reset the configuration byte, so it
     * is written again afterwards rather than assumed to have survived.
     */
    status = send_command(KEYBOARD_COMMAND_WRITE_CONFIGURATION);

    if (status != KEYBOARD_STATUS_OK) {
        return status;
    }

    status = send_data(configuration);

    if (status != KEYBOARD_STATUS_OK) {
        return status;
    }

    status = send_command(KEYBOARD_COMMAND_TEST_FIRST_PORT);

    if (status != KEYBOARD_STATUS_OK) {
        return status;
    }

    status = receive_data(&answer);

    if (status != KEYBOARD_STATUS_OK) {
        return status;
    }

    if (answer != KEYBOARD_PORT_TEST_PASSED) {
        return KEYBOARD_STATUS_PORT_TEST_FAILED;
    }

    /*
     * The handler is installed before the interrupt is unmasked, and the port
     * is enabled after both. Every other order has a window where a keystroke
     * reaches an unrouted vector.
     */
    if (interrupt_register_handler(KEYBOARD_VECTOR, keyboard_interrupt, NULL) !=
        INTERRUPT_STATUS_OK) {
        return KEYBOARD_STATUS_HANDLER_FAILURE;
    }

    if (ioapic_route_isa_irq(KEYBOARD_IRQ, KEYBOARD_VECTOR, 0U) !=
        IOAPIC_STATUS_OK) {
        (void)interrupt_unregister_handler(KEYBOARD_VECTOR);
        return KEYBOARD_STATUS_IOAPIC_FAILURE;
    }

    status = send_command(KEYBOARD_COMMAND_ENABLE_FIRST_PORT);

    if (status != KEYBOARD_STATUS_OK) {
        return status;
    }

    configuration |= KEYBOARD_CONFIGURATION_FIRST_INTERRUPT;

    status = send_command(KEYBOARD_COMMAND_WRITE_CONFIGURATION);

    if (status != KEYBOARD_STATUS_OK) {
        return status;
    }

    status = send_data(configuration);

    if (status != KEYBOARD_STATUS_OK) {
        return status;
    }

    drain();

    queue_head = 0U;
    queue_tail = 0U;
    expecting_extended = false;
    state.active = true;

    return KEYBOARD_STATUS_OK;
}

bool keyboard_is_initialized(void)
{
    return state.active;
}

enum keyboard_status keyboard_read(struct keyboard_event *event)
{
    bool enabled;

    if (!state.active) {
        return KEYBOARD_STATUS_NOT_INITIALIZED;
    }

    if (event == NULL) {
        return KEYBOARD_STATUS_CONTROLLER_REFUSED;
    }

    /*
     * The queue is written from interrupt context and read from thread context,
     * so the read is bracketed rather than trusted to be atomic. This is one
     * processor; the day there is a second, this becomes a lock.
     */
    enabled = cpu_interrupts_enabled();

    if (enabled) {
        cpu_interrupt_disable();
    }

    if (queue_head == queue_tail) {
        if (enabled) {
            cpu_interrupt_enable();
        }

        return KEYBOARD_STATUS_EMPTY;
    }

    *event = queue[queue_head];
    queue_head = (queue_head + 1U) & KEYBOARD_QUEUE_MASK;

    if (enabled) {
        cpu_interrupt_enable();
    }

    return KEYBOARD_STATUS_OK;
}

struct keyboard_state keyboard_get_state(void)
{
    struct keyboard_state snapshot = state;

    snapshot.queued = (queue_tail - queue_head) & KEYBOARD_QUEUE_MASK;
    return snapshot;
}

enum keyboard_status keyboard_inject_scancode(uint8_t scancode)
{
    enum keyboard_status status;

    if (!state.active) {
        return KEYBOARD_STATUS_NOT_INITIALIZED;
    }

    status = send_command(KEYBOARD_COMMAND_WRITE_OUTPUT_BUFFER);

    if (status != KEYBOARD_STATUS_OK) {
        return status;
    }

    return send_data(scancode);
}

const char *keyboard_status_string(enum keyboard_status status)
{
    static const char *const messages[] = {
        "ok",
        "keyboard is already initialized",
        "keyboard is not initialized",
        "keyboard bring-up ran with interrupts enabled",
        "keyboard needs the I/O APIC and it is not online",
        "the 8042 controller did not answer within its bound",
        "the 8042 controller refused the request",
        "the 8042 controller failed its own self-test",
        "the 8042 first port failed its test",
        "the keyboard interrupt could not be routed",
        "the keyboard handler could not be installed",
        "no key event is waiting"
    };

    _Static_assert(
        sizeof(messages) / sizeof(messages[0]) ==
            (size_t)KEYBOARD_STATUS_EMPTY + 1U,
        "keyboard status messages are out of sync with enum keyboard_status"
    );

    if ((size_t)status >= sizeof(messages) / sizeof(messages[0])) {
        return "unknown keyboard status";
    }

    return messages[status];
}

/*
 * What this driver must get right about scancodes, checked before the
 * controller is touched.
 *
 * Translation is the half of a keyboard driver that has no hardware in it, so
 * it is the half a self-test can reach. Everything else - the controller
 * handshake, the interrupt - is proved by prove_keyboard in
 * src/kernel/boot_proofs.c against the real 8042.
 */
static bool translation_is_right(void)
{
    /* The letter keys, unshifted, shifted, and under caps lock. */
    if (keyboard_character_for(0x1EU, false, false) != 'a') {
        return false;
    }

    if (keyboard_character_for(0x1EU, true, false) != 'A') {
        return false;
    }

    if (keyboard_character_for(0x1EU, false, true) != 'A') {
        return false;
    }

    /*
     * Caps lock is not a second shift. Held together on a letter they cancel,
     * and on a digit caps lock does nothing at all.
     */
    if (keyboard_character_for(0x1EU, true, true) != 'a') {
        return false;
    }

    if (keyboard_character_for(0x02U, false, true) != '1') {
        return false;
    }

    if (keyboard_character_for(0x02U, true, true) != '!') {
        return false;
    }

    /* The release bit must not change which key a code names. */
    if (keyboard_character_for((uint8_t)(0x1EU | KEYBOARD_RELEASE_BIT),
            false, false) != 'a') {
        return false;
    }

    /* Space, return, backspace and tab are characters, not special cases. */
    if (keyboard_character_for(0x39U, false, false) != ' ' ||
        keyboard_character_for(0x1CU, false, false) != '\n' ||
        keyboard_character_for(0x0EU, false, false) != '\b' ||
        keyboard_character_for(0x0FU, false, false) != '\t') {
        return false;
    }

    /* A modifier produces no character, in any combination. */
    for (uint8_t shift = 0U; shift < 2U; ++shift) {
        for (uint8_t caps = 0U; caps < 2U; ++caps) {
            if (keyboard_character_for(KEYBOARD_SCANCODE_LEFT_SHIFT,
                    shift != 0U, caps != 0U) != '\0') {
                return false;
            }

            if (keyboard_character_for(KEYBOARD_SCANCODE_CAPS_LOCK,
                    shift != 0U, caps != 0U) != '\0') {
                return false;
            }
        }
    }

    /*
     * Every code the table does not reach is silent rather than out of bounds.
     * The make code is seven bits, so this covers all of them.
     */
    for (uint32_t code = 0U; code < 128U; ++code) {
        const char character =
            keyboard_character_for((uint8_t)code, false, false);

        if (character != '\0' &&
            (character < ' ' || character > '~') &&
            character != '\n' && character != '\t' && character != '\b') {
            return false;
        }
    }

    /*
     * The two tables must agree about which keys are silent. A shifted entry
     * where the unshifted one is blank would produce a character on a key that
     * does not have one.
     */
    for (size_t index = 0; index < sizeof(unshifted); ++index) {
        if ((unshifted[index] == '\0') != (shifted[index] == '\0')) {
            return false;
        }
    }

    return true;
}

static bool refusals_are_named(void)
{
    static const enum keyboard_status every[] = {
        KEYBOARD_STATUS_OK,
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

    for (size_t index = 0; index < sizeof(every) / sizeof(every[0]); ++index) {
        const char *message = keyboard_status_string(every[index]);

        if (message == NULL || message[0] == '\0') {
            return false;
        }
    }

    return keyboard_status_string((enum keyboard_status)99) != NULL;
}

bool keyboard_self_test(void)
{
    if (!translation_is_right()) {
        return false;
    }

    if (!refusals_are_named()) {
        return false;
    }

    /*
     * Before bring-up every entry point refuses. This runs before the
     * controller is touched, so it is the real state rather than a simulated
     * one, and it can only be checked here.
     */
    if (!state.active) {
        struct keyboard_event event;

        if (keyboard_read(&event) != KEYBOARD_STATUS_NOT_INITIALIZED) {
            return false;
        }

        if (keyboard_inject_scancode(0x1EU) != KEYBOARD_STATUS_NOT_INITIALIZED) {
            return false;
        }
    }

    return true;
}
