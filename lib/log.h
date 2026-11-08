/** @file
    A user process's lines in the kernel's debug log, written with
    debug_write through the debug-log resource its creator gave it, in
    the form docs/spec/log.md T1 gives. Each line is kept until it ends
    and is written with one debug_write, so that it reaches the log
    whole. */

#ifndef NOVA_LOG_H
#define NOVA_LOG_H

#include "native_abi.h"

/** Write to the log through resource from now on. Until then nothing is
    written. */
void say_open(UINT32 resource);

/** Write the NUL-terminated text to the log. */
void say(const char *text);

/** n in decimal. */
void say_number(UINT64 n);

/** n in hexadecimal with a 0x prefix. */
void say_hex(UINT64 n);

/** n in hexadecimal, zero-padded to width digits, with no prefix. */
void say_hex_width(UINT64 n, UINT32 width);

/** count bytes as a value: bare if they are printable ASCII with no
    space or double quote, else in double quotes, with each byte outside
    printable ASCII, and each double quote, as a dot. */
void say_value_bytes(const UINT8 *bytes, UINT32 count);

/** The NUL-terminated text as a value, as say_value_bytes writes it. */
void say_value(const char *text);

#endif
