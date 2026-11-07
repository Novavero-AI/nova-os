/* The client side of a console session (docs/spec/console.md C4): write
   text, and read a line the console edits, with or without echo. */

#ifndef NOVA_CONSOLE_H
#define NOVA_CONSOLE_H

#include "console_protocol.h"

/* Whether the console shows the characters of a line as they are typed:
   a name is echoed, a password is not. */
typedef enum {
    CONSOLE_ECHO = 1,
    CONSOLE_NO_ECHO
} CONSOLE_ECHO_MODE;

/* Write the length bytes at text on session, in writes of at most
   CONSOLE_WRITE_MAX, waiting while the console's queue is full. */
UINT32 console_write(UINT32 session, const UINT8 *text, UINT32 length);

/* console_write of a NUL-terminated string. */
UINT32 console_say(UINT32 session, const char *text);

/* Read one line, edited at the console and ended by Enter, into line,
   of capacity bytes, 1 to CONSOLE_LINE_MAX: the console takes at most
   capacity bytes into it. *length is its length, without its end. */
UINT32 console_read_line(UINT32 session, CONSOLE_ECHO_MODE echo, UINT8 *line,
                         UINT32 capacity, UINT32 *length);

#endif
