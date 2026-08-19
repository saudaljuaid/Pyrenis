/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SENERI_SHELL_H
#define SENERI_SHELL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A command line.
 *
 * This is the first thing in Seneri that exists to be operated rather than to
 * be correct. Everything under it refuses, verifies and panics; this one takes
 * whatever a person types and has to keep going regardless.
 *
 * It is deliberately split so that almost none of it needs a keyboard. Feeding
 * characters and running a line are ordinary functions over a buffer, and the
 * interactive loop is a thin layer that reads keys and calls them. That is what
 * makes the shell testable at all: boot proves it by feeding a scripted line,
 * not by pretending to type.
 */

enum shell_status {
    SHELL_STATUS_OK = 0,
    SHELL_STATUS_NOT_INITIALIZED,
    SHELL_STATUS_LINE_TOO_LONG,
    SHELL_STATUS_UNKNOWN_COMMAND,
    SHELL_STATUS_BAD_ARGUMENT
};

/*
 * The longest line the shell will accept. A line that reaches this is refused
 * at the keystroke that would overflow it rather than truncated silently,
 * because a truncated command is a different command.
 */
#define SHELL_LINE_LIMIT 128U

struct shell_state {
    bool active;
    uint64_t lines;        /* lines submitted */
    uint64_t commands;     /* lines that named a command that ran */
    uint64_t unknown;      /* lines that named nothing this shell has */
    uint64_t rejected;     /* keystrokes refused for overflowing the line */
    size_t length;         /* characters in the line being typed now */
};

enum shell_status shell_initialize(void);
bool shell_is_active(void);

/*
 * Feed one character as though it were typed. Printable characters are echoed
 * and buffered, backspace removes one, and a newline submits the line and runs
 * it. Returns what running the line concluded, or SHELL_STATUS_OK when the
 * character did not complete one.
 */
enum shell_status shell_feed(char character);

/* Run one line as though it had been typed and submitted. */
enum shell_status shell_execute(const char *line);

struct shell_state shell_get_state(void);

/*
 * Read keys until the machine is switched off. Never returns. Only reached when
 * no test scenario was selected, because a scenario has to end and this does
 * not.
 */
_Noreturn void shell_run(void);

bool shell_self_test(void);
const char *shell_status_string(enum shell_status status);

#endif
