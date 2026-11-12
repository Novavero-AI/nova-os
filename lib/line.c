/* A line of the debug log, built whole and written with one call
   (line.h; docs/spec/log.md T1). */

#include "line.h"

/* The bytes a line's end takes: "\r\n" and the NUL. */
#define LINE_END 3

/* The digits of the greatest UINT64, 18446744073709551615. */
#define DECIMAL_DIGITS_MAX 20

void line_add(LINE *l, const char *piece)
{
    for (; *piece && l->length + LINE_END < sizeof l->text; piece++)
        l->text[l->length++] = *piece;
}

void line_number(LINE *l, UINT64 n)
{
    char digits[DECIMAL_DIGITS_MAX + 1];
    UINT32 at = DECIMAL_DIGITS_MAX;

    digits[at] = 0;
    do
        digits[--at] = (char)('0' + n % 10);
    while (n /= 10);
    line_add(l, digits + at);
}

void line_write(LINE *l, LINE_WRITE write)
{
    l->text[l->length++] = '\r';
    l->text[l->length++] = '\n';
    l->text[l->length] = 0;
    write(l->text);
}
