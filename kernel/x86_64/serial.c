/* COM1 as the firmware left it configured. */

#include "serial.h"
#include "cpu.h"

#define COM1 0x3f8
#define LINE_STATUS 5
#define TRANSMIT_EMPTY 0x20

static void put(UINT8 byte)
{
    while (!(inb(COM1 + LINE_STATUS) & TRANSMIT_EMPTY))
        ;
    outb(COM1, byte);
}

void serial_write(const char *s)
{
    for (; *s; s++)
        put((UINT8)*s);
}

void serial_write_bytes(const UINT8 *bytes, UINT64 count)
{
    UINT64 i;

    for (i = 0; i < count; i++)
        put(bytes[i]);
}

void serial_number(UINT64 n)
{
    char digits[21];
    int i = 20;

    digits[i] = 0;
    do {
        digits[--i] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    serial_write(digits + i);
}

void serial_hex(UINT64 n)
{
    char digits[17];
    int i = 16;

    digits[i] = 0;
    do {
        digits[--i] = "0123456789abcdef"[n & 0xf];
        n >>= 4;
    } while (n);
    serial_write("0x");
    serial_write(digits + i);
}
