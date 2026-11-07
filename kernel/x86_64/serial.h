/* The kernel's log, written to the first serial port. */

#ifndef NOVA_SERIAL_H
#define NOVA_SERIAL_H

#include "types.h"

void serial_write(const char *s);

/* count bytes, as they are. */
void serial_write_bytes(const UINT8 *bytes, UINT64 count);

/* n in decimal. */
void serial_number(UINT64 n);

/* n in hexadecimal, with a 0x prefix. */
void serial_hex(UINT64 n);

#endif
