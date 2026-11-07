/* The kernel's log, written to the first serial port. A line is
   written in pieces, and once threads run, a thread can be preempted
   between two pieces and another write to the port in between; a line
   that can be written while another thread writes is therefore written
   between serial_lock and serial_unlock, as debug_write writes a user
   process's bytes. */

#ifndef NOVA_SERIAL_H
#define NOVA_SERIAL_H

#include "types.h"

/* Hold the port until serial_unlock(flags), with the flags serial_lock
   returned: no other thread writes to it meanwhile. On one processor
   the lock is interrupts disabled, as for every other piece of shared
   kernel state (object.h), so a line holds off interrupts for as long
   as the port takes to send it. */
UINT64 serial_lock(void);
void serial_unlock(UINT64 flags);

void serial_write(const char *s);

/* count bytes, as they are. */
void serial_write_bytes(const UINT8 *bytes, UINT64 count);

/* n in decimal. */
void serial_number(UINT64 n);

/* n in hexadecimal, with a 0x prefix. */
void serial_hex(UINT64 n);

/* n in hexadecimal, zero-padded to at least width digits, with no
   prefix. */
void serial_hex_width(UINT64 n, UINT32 width);

#endif
