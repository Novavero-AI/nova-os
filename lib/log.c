/* The debug log, a line at a time: what say and the others write is
   kept until its line ends, then written with one debug_write, which the
   kernel writes in one piece, so that no other writer's bytes fall inside
   it (docs/spec/log.md T1). A line longer than DEBUG_WRITE_MAX_BYTES is
   written in pieces of that size. */

#include "log.h"
#include "native.h"

#define HEX_DIGITS_MAX 16
#define DECIMAL_DIGITS_MAX 20
#define PRINTABLE_FIRST 0x20
#define PRINTABLE_LAST 0x7e
#define QUOTE '"'
#define UNPRINTABLE '.'

static UINT32 log_resource = HANDLE_INVALID;
static char line[DEBUG_WRITE_MAX_BYTES];
static UINT32 line_length;

void say_open(UINT32 resource)
{
    log_resource = resource;
}

static void write_line(void)
{
    debug_write(log_resource, line, line_length);
    line_length = 0;
}

static void write_bytes(const char *bytes, UINT64 count)
{
    UINT64 i;

    if (log_resource == HANDLE_INVALID)
        return;
    for (i = 0; i < count; i++) {
        line[line_length++] = bytes[i];
        if (bytes[i] == '\n' || line_length == DEBUG_WRITE_MAX_BYTES)
            write_line();
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

static int printable(UINT8 byte)
{
    return byte >= PRINTABLE_FIRST && byte <= PRINTABLE_LAST;
}

static void say_byte(char byte)
{
    write_bytes(&byte, 1);
}

/* count bytes as text, each byte outside printable ASCII as a dot. */
static void say_printable(const UINT8 *bytes, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        say_byte(printable(bytes[i]) ? (char)bytes[i] : UNPRINTABLE);
}

void say_value_bytes(const UINT8 *bytes, UINT32 count)
{
    UINT32 i;
    int bare = count > 0;

    for (i = 0; i < count && bare; i++)
        bare = printable(bytes[i]) && bytes[i] != ' ' && bytes[i] != QUOTE;
    if (bare) {
        say_printable(bytes, count);
        return;
    }
    say_byte(QUOTE);
    for (i = 0; i < count; i++)
        say_byte(printable(bytes[i]) && bytes[i] != QUOTE ? (char)bytes[i]
                                                          : UNPRINTABLE);
    say_byte(QUOTE);
}

void say_value(const char *text)
{
    UINT32 count = 0;

    while (text[count])
        count++;
    say_value_bytes((const UINT8 *)text, count);
}
