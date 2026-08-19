/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/clock.h>
#include <seneri/console.h>
#include <seneri/cpu.h>
#include <seneri/heap.h>
#include <seneri/keyboard.h>
#include <seneri/memory.h>
#include <seneri/pci.h>
#include <seneri/screen.h>
#include <seneri/shell.h>
#include <seneri/thread.h>

/*
 * A command line.
 *
 * The shape of this file is one decision: the part that parses and dispatches
 * knows nothing about a keyboard, and the part that reads keys does nothing
 * else. shell_feed takes a character from anywhere - a key, a boot proof, a
 * scenario - and shell_run is a loop that supplies them.
 *
 * That split is why boot can prove this at all. A shell tested by pretending to
 * type is a shell whose parser was never separated from its input, and the
 * pretending is the part that rots.
 *
 * The other decision is that nothing here panics. Everything below this layer
 * refuses loudly because a wrong answer would be worse than a stopped machine.
 * A shell is the opposite: it exists to be operated by someone who will make
 * mistakes, so an unknown command is a line of output and not an incident.
 */

#define SHELL_PROMPT "seneri> "

/* What splits a command from its arguments. Nothing exotic; space and tab. */
static bool is_separator(char character)
{
    return character == ' ' || character == '\t';
}

static struct shell_state state;
static char line[SHELL_LINE_LIMIT + 1U];

static bool matches(const char *text, const char *name)
{
    size_t index = 0U;

    while (text[index] != '\0' && name[index] != '\0') {
        if (text[index] != name[index]) {
            return false;
        }

        index += 1U;
    }

    /*
     * The command ends where the name does, and the rest of the line has to
     * begin with a separator or nothing at all. Without this, "e" would match
     * "echo" and "echoes" would too.
     */
    if (name[index] != '\0') {
        return false;
    }

    return text[index] == '\0' || is_separator(text[index]);
}

/* Everything after the command word, with leading separators removed. */
static const char *arguments_of(const char *text)
{
    size_t index = 0U;

    while (text[index] != '\0' && !is_separator(text[index])) {
        index += 1U;
    }

    while (text[index] != '\0' && is_separator(text[index])) {
        index += 1U;
    }

    return &text[index];
}

static void print_size(uint64_t bytes)
{
    console_write_u64(bytes);
    console_write(" bytes");

    if (bytes >= 1024U) {
        console_write(" (");
        console_write_u64(bytes / 1024U);
        console_write(" KiB)");
    }
}

static void command_help(void)
{
    console_write("  help      this list\n");
    console_write("  echo      print the rest of the line\n");
    console_write("  clear     clear the screen\n");
    console_write("  uptime    nanoseconds since the clock started\n");
    console_write("  mem       physical frames and kernel heap\n");
    console_write("  pci       every function enumeration found\n");
    console_write("  keys      keyboard counters\n");
    console_write("  threads   scheduler counters\n");
    console_write("  version   what this is\n");
}

static void command_echo(const char *arguments)
{
    console_write(arguments);
    console_putc('\n');
}

static void command_uptime(void)
{
    const uint64_t now = clock_monotonic_ns();

    console_write_u64(now);
    console_write(" ns (");
    console_write_u64(now / UINT64_C(1000000));
    console_write(" ms)\n");
}

static void command_mem(void)
{
    const struct frame_allocator_stats frames = frame_allocator_get_stats();
    const struct heap_state heap = heap_get_state();

    console_write("frames  free ");
    console_write_u64(frames.free_frames);
    console_write(" of ");
    console_write_u64(frames.allocatable_frames);
    console_write(" allocatable, ");
    console_write_u64(frames.reserved_frames);
    console_write(" reserved\n");

    console_write("heap    ");
    print_size(heap.allocated_bytes);
    console_write(" live in ");
    console_write_u64(heap.live_allocations);
    console_write(" allocations, ");
    print_size(heap.committed_bytes);
    console_write(" committed of ");
    print_size(heap.size);
    console_putc('\n');
}

static void command_pci(void)
{
    const size_t count = pci_function_count();

    for (size_t index = 0; index < count; ++index) {
        const struct pci_function *function = pci_function_at(index);

        if (function == NULL) {
            continue;
        }

        console_write_u64(function->address.bus);
        console_putc(':');
        console_write_u64(function->address.device);
        console_putc('.');
        console_write_u64(function->address.function);
        console_write("  ");
        console_write_hex(function->vendor_id);
        console_putc(':');
        console_write_hex(function->device_id);
        console_write("  ");
        console_write(pci_class_string(function->class_code));
        console_putc('\n');
    }

    console_write_u64(count);
    console_write(" functions\n");
}

static void command_keys(void)
{
    const struct keyboard_state keyboard = keyboard_get_state();

    console_write("interrupts ");
    console_write_u64(keyboard.interrupts);
    console_write("  events ");
    console_write_u64(keyboard.events);
    console_write("  dropped ");
    console_write_u64(keyboard.dropped);
    console_write("  waiting ");
    console_write_u64(keyboard.queued);
    console_putc('\n');
}

static void command_threads(void)
{
    const struct thread_system_state threads = thread_get_state();

    console_write("switches ");
    console_write_u64(threads.switches);
    console_write("  preemptions ");
    console_write_u64(threads.preemptions);
    console_write("  preemptive ");
    console_write(threads.preemptive ? "yes" : "no");
    console_putc('\n');
}

static void command_version(void)
{
    const struct screen_state screen = screen_get_state();

    console_write("Seneri OS, an x86_64 kernel built from first principles.\n");
    console_write("console ");
    console_write_u64(screen.columns);
    console_putc('x');
    console_write_u64(screen.rows);
    console_write(" characters\n");
}

enum shell_status shell_execute(const char *text)
{
    size_t start = 0U;

    if (text == NULL) {
        return SHELL_STATUS_BAD_ARGUMENT;
    }

    while (text[start] != '\0' && is_separator(text[start])) {
        start += 1U;
    }

    text = &text[start];
    state.lines += 1U;

    /* An empty line is not a mistake and is not a command. */
    if (text[0] == '\0') {
        return SHELL_STATUS_OK;
    }

    if (matches(text, "help")) {
        command_help();
    } else if (matches(text, "echo")) {
        command_echo(arguments_of(text));
    } else if (matches(text, "clear")) {
        if (screen_is_active()) {
            (void)screen_clear();
        }
    } else if (matches(text, "uptime")) {
        command_uptime();
    } else if (matches(text, "mem")) {
        command_mem();
    } else if (matches(text, "pci")) {
        command_pci();
    } else if (matches(text, "keys")) {
        command_keys();
    } else if (matches(text, "threads")) {
        command_threads();
    } else if (matches(text, "version")) {
        command_version();
    } else {
        state.unknown += 1U;
        console_write("no such command: ");
        console_write(text);
        console_write("\n");
        return SHELL_STATUS_UNKNOWN_COMMAND;
    }

    state.commands += 1U;
    return SHELL_STATUS_OK;
}

enum shell_status shell_feed(char character)
{
    enum shell_status status;

    if (!state.active) {
        return SHELL_STATUS_NOT_INITIALIZED;
    }

    if (character == '\n' || character == '\r') {
        console_putc('\n');
        line[state.length] = '\0';
        state.length = 0U;
        status = shell_execute(line);
        console_write(SHELL_PROMPT);
        return status;
    }

    if (character == '\b') {
        if (state.length == 0U) {
            return SHELL_STATUS_OK;
        }

        state.length -= 1U;

        /*
         * Erasing is three characters: step back, write a space over what was
         * there, step back again. A lone backspace moves the cursor and leaves
         * the character on the screen.
         */
        console_putc('\b');
        console_putc(' ');
        console_putc('\b');
        return SHELL_STATUS_OK;
    }

    /* Anything the font cannot draw is not put in a line either. */
    if (character < ' ' || character > '~') {
        return SHELL_STATUS_OK;
    }

    if (state.length >= SHELL_LINE_LIMIT) {
        /*
         * Refused at the keystroke that would overflow, rather than truncated
         * when the line is run. A truncated command is a different command.
         */
        state.rejected += 1U;
        return SHELL_STATUS_LINE_TOO_LONG;
    }

    line[state.length] = character;
    state.length += 1U;
    console_putc(character);
    return SHELL_STATUS_OK;
}

enum shell_status shell_initialize(void)
{
    state.active = true;
    state.length = 0U;
    line[0] = '\0';
    return SHELL_STATUS_OK;
}

bool shell_is_active(void)
{
    return state.active;
}

struct shell_state shell_get_state(void)
{
    return state;
}

_Noreturn void shell_run(void)
{
    struct keyboard_event event;

    if (!state.active) {
        (void)shell_initialize();
    }

    console_write("\n");
    console_write(SHELL_PROMPT);

    for (;;) {
        while (keyboard_read(&event) == KEYBOARD_STATUS_OK) {
            if (event.pressed && event.character != '\0') {
                (void)shell_feed(event.character);
            }
        }

        /*
         * Halt rather than spin. keyboard_read does not block - there is no way
         * yet for an interrupt to wake a thread - so this waits the only way it
         * can: it stops the processor until an interrupt arrives, then looks
         * again.
         *
         * cpu_enable_and_halt is sti followed by hlt, and the pairing is the
         * point. sti does not take effect until after the instruction following
         * it, so no interrupt can be delivered in the gap between enabling and
         * halting - which is exactly the race that would otherwise leave the
         * machine asleep with a keystroke already waiting.
         */
        cpu_enable_and_halt();
    }
}

const char *shell_status_string(enum shell_status status)
{
    static const char *const messages[] = {
        "ok",
        "the shell is not initialized",
        "the line is longer than the shell will accept",
        "no such command",
        "bad argument"
    };

    _Static_assert(
        sizeof(messages) / sizeof(messages[0]) ==
            (size_t)SHELL_STATUS_BAD_ARGUMENT + 1U,
        "shell status messages are out of sync with enum shell_status"
    );

    if ((size_t)status >= sizeof(messages) / sizeof(messages[0])) {
        return "unknown shell status";
    }

    return messages[status];
}

/*
 * What the line editor and the dispatcher must get right, checked with no
 * keyboard, no screen, and nothing to type on.
 *
 * This is the half of a shell that has no hardware in it, and separating it
 * from the input loop is what makes it reachable at all. A shell tested by
 * pretending to type is a shell whose parser was never separated from its
 * input, and the pretending is the part that rots.
 */
static bool matching_is_right(void)
{
    /* A name matches itself and nothing longer or shorter. */
    if (!matches("help", "help")) {
        return false;
    }

    if (matches("hel", "help") || matches("helpme", "help")) {
        return false;
    }

    /* A name ends at a separator, and the arguments start after it. */
    if (!matches("echo hello", "echo")) {
        return false;
    }

    if (!matches("echo\thello", "echo")) {
        return false;
    }

    /*
     * A prefix must not match a longer command. This is the check that stops
     * "e" running "echo", which is the classic way a hand-written dispatcher
     * goes wrong.
     */
    if (matches("e", "echo")) {
        return false;
    }

    return true;
}

static bool argument_splitting_is_right(void)
{
    /* Everything after the word, with the separators between them removed. */
    if (!matches(arguments_of("echo hello"), "hello")) {
        return false;
    }

    if (arguments_of("echo")[0] != '\0') {
        return false;
    }

    if (arguments_of("echo   ")[0] != '\0') {
        return false;
    }

    /* Separators inside the arguments are the caller's, not the splitter's. */
    if (!matches(arguments_of("echo  a b"), "a")) {
        return false;
    }

    return true;
}

static bool line_editing_is_right(void)
{
    const struct shell_state before = shell_get_state();

    if (!state.active) {
        return false;
    }

    if (state.length != 0U) {
        return false;
    }

    /* Characters accumulate. */
    if (shell_feed('a') != SHELL_STATUS_OK ||
        shell_feed('b') != SHELL_STATUS_OK) {
        return false;
    }

    if (state.length != 2U) {
        return false;
    }

    /* Backspace removes one, and on an empty line removes nothing. */
    if (shell_feed('\b') != SHELL_STATUS_OK || state.length != 1U) {
        return false;
    }

    if (shell_feed('\b') != SHELL_STATUS_OK || state.length != 0U) {
        return false;
    }

    if (shell_feed('\b') != SHELL_STATUS_OK || state.length != 0U) {
        return false;
    }

    /*
     * A line at the limit refuses the keystroke that would overflow it, and
     * refuses every one after, and does not grow.
     */
    for (size_t index = 0; index < SHELL_LINE_LIMIT; ++index) {
        if (shell_feed('x') != SHELL_STATUS_OK) {
            return false;
        }
    }

    if (state.length != SHELL_LINE_LIMIT) {
        return false;
    }

    if (shell_feed('x') != SHELL_STATUS_LINE_TOO_LONG) {
        return false;
    }

    if (shell_feed('x') != SHELL_STATUS_LINE_TOO_LONG) {
        return false;
    }

    if (state.length != SHELL_LINE_LIMIT) {
        return false;
    }

    /* Clear it back out the way a person would. */
    for (size_t index = 0; index < SHELL_LINE_LIMIT; ++index) {
        if (shell_feed('\b') != SHELL_STATUS_OK) {
            return false;
        }
    }

    if (state.length != 0U) {
        return false;
    }

    /* Anything unprintable is neither buffered nor echoed. */
    if (shell_feed('\x01') != SHELL_STATUS_OK || state.length != 0U) {
        return false;
    }

    if (shell_feed('\x7F') != SHELL_STATUS_OK || state.length != 0U) {
        return false;
    }

    /* None of the above should have counted as a line. */
    return shell_get_state().lines == before.lines;
}

static bool dispatch_is_right(void)
{
    const struct shell_state before = shell_get_state();

    /* An empty line runs nothing and is not an error. */
    if (shell_execute("") != SHELL_STATUS_OK) {
        return false;
    }

    if (shell_execute("   ") != SHELL_STATUS_OK) {
        return false;
    }

    if (shell_get_state().commands != before.commands) {
        return false;
    }

    /* An unknown command is reported and counted, and does not stop anything. */
    if (shell_execute("nonesuch") != SHELL_STATUS_UNKNOWN_COMMAND) {
        return false;
    }

    if (shell_get_state().unknown != before.unknown + 1U) {
        return false;
    }

    /* A null line is refused rather than dereferenced. */
    if (shell_execute(NULL) != SHELL_STATUS_BAD_ARGUMENT) {
        return false;
    }

    /* Leading whitespace does not change which command a line names. */
    if (shell_execute("   echo") != SHELL_STATUS_OK) {
        return false;
    }

    return shell_get_state().lines == before.lines + 4U;
}

static bool refusals_are_named(void)
{
    static const enum shell_status every[] = {
        SHELL_STATUS_OK,
        SHELL_STATUS_NOT_INITIALIZED,
        SHELL_STATUS_LINE_TOO_LONG,
        SHELL_STATUS_UNKNOWN_COMMAND,
        SHELL_STATUS_BAD_ARGUMENT
    };

    for (size_t index = 0; index < sizeof(every) / sizeof(every[0]); ++index) {
        const char *message = shell_status_string(every[index]);

        if (message == NULL || message[0] == '\0') {
            return false;
        }
    }

    return shell_status_string((enum shell_status)99) != NULL;
}

bool shell_self_test(void)
{
    struct shell_state saved;

    if (!matching_is_right()) {
        return false;
    }

    if (!argument_splitting_is_right()) {
        return false;
    }

    if (!refusals_are_named()) {
        return false;
    }

    /*
     * Before initialization, feeding a character is refused. Checked first,
     * because the rest of this needs the shell up and there is no way back.
     */
    if (!state.active) {
        if (shell_feed('a') != SHELL_STATUS_NOT_INITIALIZED) {
            return false;
        }
    }

    saved = state;

    if (shell_initialize() != SHELL_STATUS_OK) {
        return false;
    }

    if (!line_editing_is_right() || !dispatch_is_right()) {
        return false;
    }

    /*
     * Put the counters back. This runs before boot has finished, and a shell
     * that starts life claiming to have run five commands is a shell whose
     * statistics are already a lie.
     */
    state = saved;
    state.active = saved.active;
    state.length = 0U;
    line[0] = '\0';
    return true;
}
