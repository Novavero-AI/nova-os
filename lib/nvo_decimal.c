/* nvo's decimal (nvo_decimal.h): docs/spec/shell.md L3, with the
   arithmetic of the General Decimal Arithmetic Specification 1.70.

   Every operation loads its operands into NUMs, numbers of up to
   NUM_DIGITS decimal digits, one digit to a byte, least significant
   first. The digit-at-a-time arithmetic on them is plain school
   arithmetic: no carry or borrow can be wrong in a way a limb's
   base-10^9 arithmetic hides, and no machine integer wider than 64 bits
   is needed. An operation computes its exact result, or one with a
   sticky digit standing for what it leaves out, and hands it to finish,
   which rounds once, to 34 digits and no finer than NVO_DECIMAL_ETINY,
   half-even; checks the exponent's range; and clamps. Rounding once is
   what keeps a subnormal result from being rounded twice. */

#include "nvo_decimal.h"

/* The digits a working number holds. The widest is a sum: an operand of
   34 digits shifted up by the gap to a smaller one that still reaches
   NVO_GUARD digits below it (34 + 69), plus the other operand's carry. */
#define NUM_DIGITS 112u

/* The digits kept of a number read from text: the 34 of a coefficient,
   the digit the rounding looks at, and one that says whether anything
   nonzero followed. */
#define TEXT_KEEP (NVO_DECIMAL_DIGITS + 2)

/* How many digits below an operand's last digit another operand must
   have its first to be no more than a sticky digit in a sum of the two:
   more than the 34 kept and the one rounding looks at. */
#define NVO_GUARD 36

/* The padding a sum with a zero operand adds to a coefficient: more than
   the 34 digits a result can hold, so finish drops the excess. */
#define ZERO_PAD 40

/* The most an exponent read from text is taken as: far past what any
   number holds, and far from overflowing an INT64 when added to a
   count of digits. */
#define TEXT_EXPONENT_LIMIT 1000000000000000LL

/* The greatest decimal digits of an INT64's magnitude. */
#define INT64_DIGITS 19

typedef struct {
    UINT8 d[NUM_DIGITS]; /* the digits; every d[i] from n up is 0 */
    UINT32 n;            /* the significant digits: 0 for zero */
} NUM;

static INT64 larger64(INT64 a, INT64 b) { return a > b ? a : b; }

static INT64 smaller64(INT64 a, INT64 b) { return a < b ? a : b; }

/* ------------------------------------------------------------------------
   Digit arithmetic */

static void num_clear(NUM *x)
{
    UINT32 i;

    for (i = 0; i < NUM_DIGITS; i++)
        x->d[i] = 0;
    x->n = 0;
}

static void num_copy(NUM *to, const NUM *from)
{
    UINT32 i;

    for (i = 0; i < NUM_DIGITS; i++)
        to->d[i] = from->d[i];
    to->n = from->n;
}

/* Set n to the position of the highest nonzero digit, plus one. */
static void num_trim(NUM *x)
{
    while (x->n > 0 && x->d[x->n - 1] == 0)
        x->n--;
}

/* x = x * 10 + digit, which the caller's bounds keep within NUM_DIGITS. */
static void num_push(NUM *x, UINT32 digit)
{
    UINT32 i;

    if (x->n + 1 > NUM_DIGITS)
        return;
    for (i = x->n; i > 0; i--)
        x->d[i] = x->d[i - 1];
    x->d[0] = (UINT8)digit;
    if (x->n > 0 || digit != 0)
        x->n++;
}

/* x = x * 10^k. Returns 0, with x unchanged, if it would not fit. */
static int num_shift_left(NUM *x, UINT32 k)
{
    UINT32 i;

    if (x->n == 0 || k == 0)
        return 1;
    if (x->n + k > NUM_DIGITS)
        return 0;
    for (i = x->n; i > 0; i--)
        x->d[i - 1 + k] = x->d[i - 1];
    for (i = 0; i < k; i++)
        x->d[i] = 0;
    x->n += k;
    return 1;
}

/* x = x / 10^k, dropping the digits below. */
static void num_shift_right(NUM *x, UINT32 k)
{
    UINT32 i;

    if (k >= x->n) {
        num_clear(x);
        return;
    }
    for (i = 0; i + k < x->n; i++)
        x->d[i] = x->d[i + k];
    for (i = x->n - k; i < x->n; i++)
        x->d[i] = 0;
    x->n -= k;
}

/* Returns -1, 0 or 1 as a is less than, equal to or greater than b. */
static int num_compare(const NUM *a, const NUM *b)
{
    UINT32 i;

    if (a->n != b->n)
        return a->n < b->n ? -1 : 1;
    for (i = a->n; i > 0; i--)
        if (a->d[i - 1] != b->d[i - 1])
            return a->d[i - 1] < b->d[i - 1] ? -1 : 1;
    return 0;
}

/* r = a + b. r may be a or b. The sum fits NUM_DIGITS by the caller. */
static void num_add(NUM *r, const NUM *a, const NUM *b)
{
    UINT32 i, carry = 0;

    for (i = 0; i < NUM_DIGITS; i++) {
        UINT32 s = (UINT32)a->d[i] + (UINT32)b->d[i] + carry;

        r->d[i] = (UINT8)(s % 10);
        carry = s / 10;
    }
    r->n = NUM_DIGITS;
    num_trim(r);
}

/* r = a - b, for a not less than b. r may be a or b. */
static void num_sub(NUM *r, const NUM *a, const NUM *b)
{
    UINT32 i, borrow = 0;

    for (i = 0; i < NUM_DIGITS; i++) {
        INT32 s = (INT32)a->d[i] - (INT32)b->d[i] - (INT32)borrow;

        borrow = s < 0;
        r->d[i] = (UINT8)(s < 0 ? s + 10 : s);
    }
    r->n = NUM_DIGITS;
    num_trim(r);
}

/* r = 1 more than r. */
static void num_increment(NUM *r)
{
    UINT32 i = 0;

    while (i < NUM_DIGITS && r->d[i] == 9)
        r->d[i++] = 0;
    if (i < NUM_DIGITS)
        r->d[i]++;
    if (i + 1 > r->n)
        r->n = i + 1;
}

/* r = a * b, for r apart from a and b and a->n + b->n within
   NUM_DIGITS. */
static void num_multiply(NUM *r, const NUM *a, const NUM *b)
{
    UINT32 column[NUM_DIGITS];
    UINT32 i, j, carry = 0;

    for (i = 0; i < NUM_DIGITS; i++)
        column[i] = 0;
    for (i = 0; i < a->n; i++)
        for (j = 0; j < b->n && i + j < NUM_DIGITS; j++)
            column[i + j] += (UINT32)a->d[i] * (UINT32)b->d[j];
    for (i = 0; i < NUM_DIGITS; i++) {
        UINT32 s = column[i] + carry;

        r->d[i] = (UINT8)(s % 10);
        carry = s / 10;
    }
    r->n = NUM_DIGITS;
    num_trim(r);
}

/* q = a / b and r = a % b, by long division, for b not zero and q and r
   apart from each other and from a and b. */
static void num_divide(NUM *q, NUM *r, const NUM *a, const NUM *b)
{
    UINT32 i;

    num_clear(q);
    num_clear(r);
    for (i = a->n; i > 0; i--) {
        UINT32 digit = 0;

        num_push(r, a->d[i - 1]);
        while (num_compare(r, b) >= 0) {
            num_sub(r, r, b);
            digit++;
        }
        q->d[i - 1] = (UINT8)digit;
    }
    q->n = a->n;
    num_trim(q);
}

/* ------------------------------------------------------------------------
   The coefficient in limbs */

static void load(NUM *x, const NVO_DECIMAL *d)
{
    UINT32 k, j, i = 0;

    num_clear(x);
    for (k = 0; k < NVO_DECIMAL_LIMBS; k++) {
        UINT32 v = d->limb[k];

        for (j = 0; j < NVO_DECIMAL_LIMB_DIGITS; j++) {
            x->d[i++] = (UINT8)(v % 10);
            v /= 10;
        }
    }
    x->n = i;
    num_trim(x);
}

/* Set d from x, which holds at most 34 digits. */
static void store(NVO_DECIMAL *d, const NUM *x, INT64 exponent,
                  UINT32 negative)
{
    UINT32 k, j;

    for (k = 0; k < NVO_DECIMAL_LIMBS; k++) {
        UINT32 v = 0;

        for (j = NVO_DECIMAL_LIMB_DIGITS; j > 0; j--)
            v = v * 10 + x->d[k * NVO_DECIMAL_LIMB_DIGITS + j - 1];
        d->limb[k] = v;
    }
    d->exponent = (INT32)exponent;
    d->negative = negative;
}

void nvo_decimal_zero(NVO_DECIMAL *x)
{
    UINT32 k;

    for (k = 0; k < NVO_DECIMAL_LIMBS; k++)
        x->limb[k] = 0;
    x->exponent = 0;
    x->negative = 0;
}

void nvo_decimal_copy(NVO_DECIMAL *to, const NVO_DECIMAL *from)
{
    UINT32 k;

    for (k = 0; k < NVO_DECIMAL_LIMBS; k++)
        to->limb[k] = from->limb[k];
    to->exponent = from->exponent;
    to->negative = from->negative;
}

int nvo_decimal_is_zero(const NVO_DECIMAL *x)
{
    UINT32 k;

    for (k = 0; k < NVO_DECIMAL_LIMBS; k++)
        if (x->limb[k] != 0)
            return 0;
    return 1;
}

/* ------------------------------------------------------------------------
   Rounding */

/* Round c x 10^exp to at most 34 digits and no exponent below
   NVO_DECIMAL_ETINY, half-even, with sticky standing for nonzero digits
   below c, and store the result in out. c is consumed. *exact, if it is
   not 0, is set to 1 if the result equals the number given and else 0.
   Returns NVO_OVERFLOW, leaving out unchanged, if the result's adjusted
   exponent is past NVO_DECIMAL_EMAX. */
static NVO_STATUS finish(NVO_DECIMAL *out, UINT32 negative, NUM *c, INT64 exp,
                         UINT32 sticky, UINT32 *exact)
{
    INT64 drop = larger64(
        larger64((INT64)c->n - NVO_DECIMAL_DIGITS, NVO_DECIMAL_ETINY - exp), 0);
    UINT32 lost = sticky;

    if (drop > 0) {
        if (drop > (INT64)c->n) {
            /* Every digit lies below half a unit of the new last place. */
            lost |= c->n > 0;
            num_clear(c);
        } else {
            UINT32 k = (UINT32)drop, i, rest = sticky;
            UINT32 guard = c->d[k - 1];
            UINT32 odd;

            for (i = 0; i + 1 < k; i++)
                if (c->d[i] != 0)
                    rest = 1;
            lost = rest | (guard != 0);
            odd = c->d[k] & 1u;
            num_shift_right(c, k);
            if (guard > 5 || (guard == 5 && (rest || odd)))
                num_increment(c);
        }
        exp += drop;
        if (c->n > NVO_DECIMAL_DIGITS) {
            /* 99...9 rounded up is 10^34: 10^33, a power higher. */
            num_shift_right(c, 1);
            exp += 1;
        }
    }
    if (c->n == 0) {
        exp = smaller64(exp, NVO_DECIMAL_EXPONENT_MAX);
    } else {
        if (exp + (INT64)c->n - 1 > NVO_DECIMAL_EMAX)
            return NVO_OVERFLOW;
        if (exp > NVO_DECIMAL_EXPONENT_MAX) {
            (void)num_shift_left(c, (UINT32)(exp - NVO_DECIMAL_EXPONENT_MAX));
            exp = NVO_DECIMAL_EXPONENT_MAX;
        }
    }
    store(out, c, exp, negative);
    if (exact)
        *exact = !lost;
    return NVO_OK;
}

/* ------------------------------------------------------------------------
   Text */

NVO_STATUS nvo_decimal_from_text(NVO_DECIMAL *x, const UINT8 *text,
                                 UINT64 length)
{
    NUM c;
    UINT64 i = 0;
    UINT32 negative = 0, point = 0, seen = 0, sticky = 0, exp_digits = 0;
    INT64 scale = 0, extra = 0, e = 0;
    NVO_DECIMAL made;
    NVO_STATUS status;

    num_clear(&c);
    if (i < length && (text[i] == '+' || text[i] == '-')) {
        negative = text[i] == '-';
        i++;
    }
    for (; i < length; i++) {
        UINT32 ch = text[i];

        if (ch >= '0' && ch <= '9') {
            seen++;
            if (point)
                scale++;
            if (c.n == 0 && ch == '0')
                continue;
            if (c.n < TEXT_KEEP) {
                num_push(&c, ch - '0');
            } else {
                extra++;
                if (ch != '0')
                    sticky = 1;
            }
        } else if (ch == '.' && !point) {
            point = 1;
        } else {
            break;
        }
    }
    if (seen == 0)
        return NVO_SYNTAX;
    if (i < length) {
        UINT32 minus = 0;

        if (text[i] != 'e' && text[i] != 'E')
            return NVO_SYNTAX;
        i++;
        if (i < length && (text[i] == '+' || text[i] == '-')) {
            minus = text[i] == '-';
            i++;
        }
        for (; i < length && text[i] >= '0' && text[i] <= '9'; i++) {
            exp_digits++;
            if (e < TEXT_EXPONENT_LIMIT)
                e = e * 10 + (text[i] - '0');
        }
        if (exp_digits == 0 || i != length)
            return NVO_SYNTAX;
        if (minus)
            e = -e;
    }
    if (sticky && c.n == TEXT_KEEP && c.d[0] == 0)
        c.d[0] = 1;
    status = finish(&made, negative, &c, e - scale + extra, 0, 0);
    if (status == NVO_OK)
        nvo_decimal_copy(x, &made);
    return status;
}

/* Write the decimal digits of v, a count, without leading zeros. */
static UINT32 put_count(UINT8 *out, INT64 v)
{
    UINT8 digits[24];
    UINT32 n = 0, i;

    do {
        digits[n++] = (UINT8)('0' + v % 10);
        v /= 10;
    } while (v > 0);
    for (i = 0; i < n; i++)
        out[i] = digits[n - 1 - i];
    return n;
}

UINT32 nvo_decimal_to_text(const NVO_DECIMAL *x, UINT8 *out)
{
    NUM c;
    UINT32 n, at = 0, i;
    INT64 exp = x->exponent, adjusted;

    load(&c, x);
    n = c.n == 0 ? 1 : c.n;
    adjusted = exp + (INT64)n - 1;
    if (x->negative)
        out[at++] = '-';
    if (exp <= 0 && adjusted >= -6) {
        INT64 before = (INT64)n + exp;

        if (exp == 0) {
            for (i = n; i > 0; i--)
                out[at++] = (UINT8)('0' + c.d[i - 1]);
        } else if (before > 0) {
            for (i = n; i > 0; i--) {
                out[at++] = (UINT8)('0' + c.d[i - 1]);
                if ((INT64)n - (INT64)i + 1 == before)
                    out[at++] = '.';
            }
        } else {
            out[at++] = '0';
            out[at++] = '.';
            for (i = 0; i < (UINT32)(-before); i++)
                out[at++] = '0';
            for (i = n; i > 0; i--)
                out[at++] = (UINT8)('0' + c.d[i - 1]);
        }
    } else {
        out[at++] = (UINT8)('0' + c.d[n - 1]);
        if (n > 1) {
            out[at++] = '.';
            for (i = n - 1; i > 0; i--)
                out[at++] = (UINT8)('0' + c.d[i - 1]);
        }
        out[at++] = 'E';
        out[at++] = adjusted < 0 ? '-' : '+';
        at += put_count(out + at, adjusted < 0 ? -adjusted : adjusted);
    }
    return at;
}

/* ------------------------------------------------------------------------
   Conversions to and from integers */

void nvo_decimal_from_int(NVO_DECIMAL *x, INT64 v)
{
    NUM c;
    UINT64 magnitude = v < 0 ? (UINT64)0 - (UINT64)v : (UINT64)v;
    UINT32 i = 0;

    num_clear(&c);
    while (magnitude > 0) {
        c.d[i++] = (UINT8)(magnitude % 10);
        magnitude /= 10;
    }
    c.n = i;
    store(x, &c, 0, v < 0);
}

NVO_STATUS nvo_decimal_to_int(const NVO_DECIMAL *x, INT64 *v)
{
    NUM c;
    UINT64 magnitude = 0;
    UINT32 i;

    load(&c, x);
    if (c.n == 0) {
        *v = 0;
        return NVO_OK;
    }
    if (x->exponent < 0) {
        UINT32 below = (UINT32)(-(INT64)x->exponent);

        for (i = 0; i < below && i < c.n; i++)
            if (c.d[i] != 0)
                return NVO_INVALID;
        num_shift_right(&c, below);
    } else if (x->exponent > 0) {
        if ((INT64)c.n + x->exponent > INT64_DIGITS)
            return NVO_OVERFLOW;
        (void)num_shift_left(&c, (UINT32)x->exponent);
    }
    if (c.n > INT64_DIGITS)
        return NVO_OVERFLOW;
    for (i = c.n; i > 0; i--)
        magnitude = magnitude * 10 + c.d[i - 1];
    if (x->negative) {
        if (magnitude > ((UINT64)1 << 63))
            return NVO_OVERFLOW;
        *v = (INT64)((UINT64)0 - magnitude);
    } else {
        if (magnitude > (((UINT64)1 << 63) - 1))
            return NVO_OVERFLOW;
        *v = (INT64)magnitude;
    }
    return NVO_OK;
}

/* ------------------------------------------------------------------------
   Arithmetic */

/* r = a + b with b's sign as given. *exact, if it is not 0, is set to 1 if
   r is the sum exactly and else to 0 (finish's). */
static NVO_STATUS add_with(NVO_DECIMAL *r, const NVO_DECIMAL *a,
                           const NVO_DECIMAL *b, UINT32 b_negative,
                           UINT32 *exact)
{
    NUM x, y, sum;
    UINT32 sx, sy;
    INT64 ex, ey;
    NVO_DECIMAL made;
    NVO_STATUS status;

    load(&x, a);
    load(&y, b);
    sx = a->negative;
    sy = b_negative;
    ex = a->exponent;
    ey = b->exponent;
    if (ex < ey) {
        /* x is the operand with the larger exponent. */
        num_copy(&sum, &x);
        num_copy(&x, &y);
        num_copy(&y, &sum);
        sx = b_negative;
        sy = a->negative;
        ex = b->exponent;
        ey = a->exponent;
    }
    if (x.n == 0 && y.n == 0) {
        status = finish(&made, sx && sy, &x, ey, 0, exact);
    } else if (x.n == 0) {
        status = finish(&made, sy, &y, ey, 0, exact);
    } else if (y.n == 0) {
        INT64 pad = smaller64(ex - ey, ZERO_PAD);

        (void)num_shift_left(&x, (UINT32)pad);
        status = finish(&made, sx, &x, ex - pad, 0, exact);
    } else {
        INT64 gap;
        UINT32 negative;

        if (ey + (INT64)y.n - 1 < ex - NVO_GUARD) {
            /* y is no more than a sticky digit below x's last. */
            num_clear(&y);
            y.d[0] = 1;
            y.n = 1;
            ey = ex - NVO_GUARD - 1;
        }
        gap = ex - ey;
        (void)num_shift_left(&x, (UINT32)gap);
        if (sx == sy) {
            num_add(&sum, &x, &y);
            negative = sx;
        } else {
            int order = num_compare(&x, &y);

            if (order == 0) {
                num_clear(&sum);
                negative = 0;
            } else if (order > 0) {
                num_sub(&sum, &x, &y);
                negative = sx;
            } else {
                num_sub(&sum, &y, &x);
                negative = sy;
            }
        }
        status = finish(&made, negative, &sum, ey, 0, exact);
    }
    if (status == NVO_OK)
        nvo_decimal_copy(r, &made);
    return status;
}

NVO_STATUS nvo_decimal_add(NVO_DECIMAL *r, const NVO_DECIMAL *a,
                           const NVO_DECIMAL *b)
{
    return add_with(r, a, b, b->negative, 0);
}

NVO_STATUS nvo_decimal_add_inexact(NVO_DECIMAL *r, const NVO_DECIMAL *a,
                                   const NVO_DECIMAL *b, int *inexact)
{
    UINT32 exact = 0;
    NVO_STATUS status = add_with(r, a, b, b->negative, &exact);

    if (status == NVO_OK)
        *inexact = !exact;
    return status;
}

NVO_STATUS nvo_decimal_subtract(NVO_DECIMAL *r, const NVO_DECIMAL *a,
                                const NVO_DECIMAL *b)
{
    return add_with(r, a, b, !b->negative, 0);
}

NVO_STATUS nvo_decimal_multiply(NVO_DECIMAL *r, const NVO_DECIMAL *a,
                                const NVO_DECIMAL *b)
{
    NUM x, y, product;
    NVO_DECIMAL made;
    NVO_STATUS status;

    load(&x, a);
    load(&y, b);
    num_multiply(&product, &x, &y);
    status = finish(&made, a->negative != b->negative, &product,
                    (INT64)a->exponent + b->exponent, 0, 0);
    if (status == NVO_OK)
        nvo_decimal_copy(r, &made);
    return status;
}

/* While the coefficient ends in a zero and the exponent is below bound,
   drop the zero and raise the exponent. */
static void strip_zeros(NVO_DECIMAL *d, INT64 bound)
{
    NUM c;

    load(&c, d);
    if (c.n == 0)
        return;
    while (d->exponent < bound && c.d[0] == 0) {
        num_shift_right(&c, 1);
        d->exponent++;
    }
    store(d, &c, d->exponent, d->negative);
}

NVO_STATUS nvo_decimal_divide(NVO_DECIMAL *r, const NVO_DECIMAL *a,
                              const NVO_DECIMAL *b)
{
    NUM x, y, q, rest;
    UINT32 negative = a->negative != b->negative, exact = 0, s;
    INT64 ideal = (INT64)a->exponent - b->exponent;
    NVO_DECIMAL made;
    NVO_STATUS status;

    load(&x, a);
    load(&y, b);
    if (y.n == 0)
        return x.n == 0 ? NVO_INVALID : NVO_DIVIDE_BY_ZERO;
    if (x.n == 0) {
        status = finish(&made, negative, &x, ideal, 0, 0);
        if (status == NVO_OK)
            nvo_decimal_copy(r, &made);
        return status;
    }
    /* Enough digits in the quotient that rounding has a guard digit and
       the remainder is the sticky one. */
    s = y.n + NVO_DECIMAL_DIGITS + 1 > x.n
            ? y.n + NVO_DECIMAL_DIGITS + 1 - x.n
            : 0;
    (void)num_shift_left(&x, s);
    num_divide(&q, &rest, &x, &y);
    status = finish(&made, negative, &q, ideal - s, rest.n != 0, &exact);
    if (status != NVO_OK)
        return status;
    if (exact)
        strip_zeros(&made, smaller64(ideal, NVO_DECIMAL_EXPONENT_MAX));
    nvo_decimal_copy(r, &made);
    return NVO_OK;
}

/* The integer quotient and remainder of a by b at the exponent the
   smaller of theirs gives, for neither operand a special case:
   *quotient and *remainder hold them. Returns NVO_INVALID if the
   quotient has more than 34 digits. */
static NVO_STATUS integer_division(NUM *quotient, NUM *remainder, INT64 *e,
                                   const NVO_DECIMAL *a, const NVO_DECIMAL *b)
{
    NUM x, y;
    INT64 ea = a->exponent, eb = b->exponent;
    INT64 adjusted_a, adjusted_b;

    load(&x, a);
    load(&y, b);
    adjusted_a = ea + (INT64)x.n - 1;
    adjusted_b = eb + (INT64)y.n - 1;
    *e = smaller64(ea, eb);
    num_clear(quotient);
    if (x.n == 0 || adjusted_a < adjusted_b) {
        /* The quotient is 0 and the remainder is a. */
        if (ea > eb)
            (void)num_shift_left(&x, (UINT32)(ea - eb));
        num_copy(remainder, &x);
        return NVO_OK;
    }
    if (adjusted_a - adjusted_b >= NVO_DECIMAL_DIGITS + 1)
        return NVO_INVALID;
    if (ea > eb)
        (void)num_shift_left(&x, (UINT32)(ea - eb));
    else
        (void)num_shift_left(&y, (UINT32)(eb - ea));
    num_divide(quotient, remainder, &x, &y);
    if (quotient->n > NVO_DECIMAL_DIGITS)
        return NVO_INVALID;
    return NVO_OK;
}

NVO_STATUS nvo_decimal_divide_integer(NVO_DECIMAL *r, const NVO_DECIMAL *a,
                                      const NVO_DECIMAL *b)
{
    NUM quotient, remainder;
    INT64 e;
    NVO_STATUS status;

    if (nvo_decimal_is_zero(b))
        return nvo_decimal_is_zero(a) ? NVO_INVALID : NVO_DIVIDE_BY_ZERO;
    status = integer_division(&quotient, &remainder, &e, a, b);
    if (status != NVO_OK)
        return status;
    store(r, &quotient, 0, a->negative != b->negative);
    return NVO_OK;
}

NVO_STATUS nvo_decimal_remainder(NVO_DECIMAL *r, const NVO_DECIMAL *a,
                                 const NVO_DECIMAL *b)
{
    NUM quotient, remainder;
    INT64 e;
    NVO_STATUS status;

    if (nvo_decimal_is_zero(b))
        return NVO_INVALID;
    status = integer_division(&quotient, &remainder, &e, a, b);
    if (status != NVO_OK)
        return status;
    store(r, &remainder, e, a->negative);
    return NVO_OK;
}

void nvo_decimal_negate(NVO_DECIMAL *r, const NVO_DECIMAL *x)
{
    UINT32 negative = nvo_decimal_is_zero(x) ? 0 : !x->negative;

    nvo_decimal_copy(r, x);
    r->negative = negative;
}

/* Returns -1, 0 or 1 as the magnitude of a, for a nonzero, is less than,
   equal to or greater than b's, for b nonzero. */
static int magnitude_compare(const NVO_DECIMAL *a, const NVO_DECIMAL *b)
{
    NUM x, y;
    INT64 adjusted_a, adjusted_b;

    load(&x, a);
    load(&y, b);
    adjusted_a = (INT64)a->exponent + (INT64)x.n - 1;
    adjusted_b = (INT64)b->exponent + (INT64)y.n - 1;
    if (adjusted_a != adjusted_b)
        return adjusted_a < adjusted_b ? -1 : 1;
    if (a->exponent > b->exponent)
        (void)num_shift_left(&x, (UINT32)(a->exponent - b->exponent));
    else
        (void)num_shift_left(&y, (UINT32)(b->exponent - a->exponent));
    return num_compare(&x, &y);
}

int nvo_decimal_compare(const NVO_DECIMAL *a, const NVO_DECIMAL *b)
{
    int za = nvo_decimal_is_zero(a), zb = nvo_decimal_is_zero(b), order;

    if (za && zb)
        return 0;
    if (za)
        return b->negative ? 1 : -1;
    if (zb)
        return a->negative ? -1 : 1;
    if (a->negative != b->negative)
        return a->negative ? -1 : 1;
    order = magnitude_compare(a, b);
    return a->negative ? -order : order;
}
