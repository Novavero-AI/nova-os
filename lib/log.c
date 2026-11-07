/* The debug log, written in pieces of at most DEBUG_WRITE_MAX_BYTES. */

#include "log.h"
#include "native.h"

#define HEX_DIGITS_MAX 16
#define DECIMAL_DIGITS_MAX 20
#define PRINTABLE_FIRST 0x20
#define PRINTABLE_LAST 0x7e

static UINT32 log_resource = HANDLE_INVALID;

void say_open(UINT32 resource)
{
    log_resource = resource;
}

static void write_bytes(const char *bytes, UINT64 count)
{
    UINT64 piece;

    if (log_resource == HANDLE_INVALID)
        return;
    while (count) {
        piece = count < DEBUG_WRITE_MAX_BYTES ? count : DEBUG_WRITE_MAX_BYTES;
        debug_write(log_resource, bytes, piece);
        bytes += piece;
        count -= piece;
    }
}

void say(const char *text)
{
    UINT64 count = 0;

    while (text[count])
        count++;
    write_bytes(text, count);
}

void say_number(UINT64 n)
{
    char digits[DECIMAL_DIGITS_MAX];
    UINT32 i = DECIMAL_DIGITS_MAX;

    do {
        digits[--i] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    write_bytes(digits + i, DECIMAL_DIGITS_MAX - i);
}

void say_hex_width(UINT64 n, UINT32 width)
{
    char digits[HEX_DIGITS_MAX];
    UINT32 i = HEX_DIGITS_MAX;

    do {
        digits[--i] = "0123456789abcdef"[n & 0xf];
        n >>= 4;
    } while (n || (i > 0 && HEX_DIGITS_MAX - i < width));
    write_bytes(digits + i, HEX_DIGITS_MAX - i);
}

void say_hex(UINT64 n)
{
    say("0x");
    say_hex_width(n, 1);
}

void say_printable(const UINT8 *bytes, UINT32 count)
{
    char text[DEBUG_WRITE_MAX_BYTES];
    UINT32 i, n;

    while (count) {
        n = count < DEBUG_WRITE_MAX_BYTES ? count : DEBUG_WRITE_MAX_BYTES;
        for (i = 0; i < n; i++)
            text[i] = bytes[i] >= PRINTABLE_FIRST && bytes[i] <= PRINTABLE_LAST
                          ? (char)bytes[i]
                          : '.';
        write_bytes(text, n);
        bytes += n;
        count -= n;
    }
}
