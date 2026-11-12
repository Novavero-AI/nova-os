/** @file
    A line of the debug log, built whole by code that makes no system
    call (docs/spec/log.md T1): text and numbers are added to it piece by
    piece, and it is then ended and handed to a function the caller
    gives, which writes it with one call, so that no other process's line
    lands among its pieces. lib/endpoint.c and lib/budget.c build their
    lines with it, and a host build writes the bytes nova-os does. */

#ifndef NOVA_LINE_H
#define NOVA_LINE_H

#include "native_abi.h"

/** Writes one log line, NUL-terminated and ending in "\r\n". */
typedef void (*LINE_WRITE)(const char *line);

/** A line being built: as many bytes as one debug_write takes, so that
    the kernel writes it in one piece. A line starts with its length set
    to 0. */
typedef struct {
    char text[DEBUG_WRITE_MAX_BYTES]; /**< its bytes, and then its end */
    UINT32 length;                    /**< how many bytes it holds */
} LINE;

/** Add the NUL-terminated piece to the line. A line longer than the
    buffer is cut short, before the bytes its end and the NUL take. */
void line_add(LINE *l, const char *piece);

/** Add n to the line, in decimal. */
void line_number(LINE *l, UINT64 n);

/** End the line with "\r\n" and a NUL, and hand it to write. */
void line_write(LINE *l, LINE_WRITE write);

#endif
