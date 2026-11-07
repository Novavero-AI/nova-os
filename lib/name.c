/* Name checks (name.h). UTF-8 as RFC 3629, 4 defines it: no overlong
   form, no surrogate, nothing past U+10FFFF. */

#include "name.h"

#define ASCII_LIMIT 0x80
#define CONTINUATION_MASK 0xc0
#define CONTINUATION 0x80
#define CONTINUATION_BITS 6
#define CODE_POINT_MAX 0x10ffff
#define SURROGATE_FIRST 0xd800
#define SURROGATE_LAST 0xdfff

/* The lead byte's form for a sequence of 2, 3 and 4 bytes: the mask, the
   form under it, and the least code point the sequence may encode. */
static const UINT8 lead_mask[] = {0xe0, 0xf0, 0xf8};
static const UINT8 lead_form[] = {0xc0, 0xe0, 0xf0};
static const UINT32 least[] = {0x80, 0x800, 0x10000};

/* The length of the UTF-8 sequence at s, at most left bytes, or 0 if it
   is not well formed. */
static UINT32 sequence(const UINT8 *s, UINT32 left)
{
    UINT32 n, k, c;

    if (s[0] < ASCII_LIMIT)
        return 1;
    for (n = 0; n < sizeof lead_mask; n++)
        if ((s[0] & lead_mask[n]) == lead_form[n])
            break;
    if (n == sizeof lead_mask || n + 2 > left)
        return 0;
    c = s[0] & (UINT8)~lead_mask[n];
    for (k = 1; k <= n + 1; k++) {
        if ((s[k] & CONTINUATION_MASK) != CONTINUATION)
            return 0;
        c = c << CONTINUATION_BITS | (s[k] & (UINT8)~CONTINUATION_MASK);
    }
    if (c < least[n] || c > CODE_POINT_MAX ||
        (c >= SURROGATE_FIRST && c <= SURROGATE_LAST))
        return 0;
    return n + 2;
}

int name_valid(const UINT8 *name, UINT32 length)
{
    UINT32 i, n;

    if (length == 0 || length > NAME_MAX_BYTES ||
        (length == 1 && name[0] == '.') ||
        (length == 2 && name[0] == '.' && name[1] == '.'))
        return 0;
    for (i = 0; i < length; i += n) {
        if (name[i] == '/' || name[i] == 0)
            return 0;
        n = sequence(name + i, length - i);
        if (!n)
            return 0;
    }
    return 1;
}
