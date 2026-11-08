/** @file
    nvo's decimal: a decimal floating-point number of 34 significant
    digits, rounded half-even (docs/spec/shell.md L3), with the arithmetic
    of the General Decimal Arithmetic Specification, version 1.70
    ("Decimal"), in its 34-digit format.

    A number is a sign, a coefficient of at most 34 digits and an exponent,
    its value coefficient x 10^exponent. The exponent runs from
    NVO_DECIMAL_ETINY to NVO_DECIMAL_EXPONENT_MAX, so the largest
    adjusted exponent is NVO_DECIMAL_EMAX and the smallest of a number with
    a full coefficient NVO_DECIMAL_EMIN: the range Decimal gives its
    34-digit format. A result with a smaller exponent than that is rounded
    to NVO_DECIMAL_ETINY, so it is subnormal or 0; one with a larger is
    padded with zeros to NVO_DECIMAL_EXPONENT_MAX, the clamp that format
    makes. There is no infinity and no NaN: an operation that would give
    one returns a status instead and leaves its result unchanged.

    Zero has a sign, as Decimal's extended arithmetic gives it, and
    equals zero of either. Integer arithmetic only: no floating point and
    no 128-bit type, so the guest and every host compute the same
    digits. */

#ifndef NOVA_NVO_DECIMAL_H
#define NOVA_NVO_DECIMAL_H

#include "types.h"

/** The significant digits of a coefficient, which is Decimal's precision
    for its 34-digit format and L3's. */
#define NVO_DECIMAL_DIGITS 34

/** The greatest adjusted exponent: the exponent a number would have
    written with one digit before its point (Decimal, 34-digit format). */
#define NVO_DECIMAL_EMAX 6144

/** The least adjusted exponent of a number whose coefficient is full
    (Decimal, 34-digit format): 1 - NVO_DECIMAL_EMAX. */
#define NVO_DECIMAL_EMIN (-6143)

/** The least exponent: a result below it is rounded to it. It is
    NVO_DECIMAL_EMIN less the digits of a coefficient, plus one. */
#define NVO_DECIMAL_ETINY (-6176)

/** The greatest exponent, which a coefficient of one digit padded to a
    full one has at NVO_DECIMAL_EMAX. */
#define NVO_DECIMAL_EXPONENT_MAX 6111

/** The decimal digits one limb of a coefficient holds. */
#define NVO_DECIMAL_LIMB_DIGITS 9

/** The limbs of a coefficient: four of nine digits hold the 34. */
#define NVO_DECIMAL_LIMBS 4

/** The bytes nvo_decimal_to_text writes at most: a sign, 34 digits, a
    point or the zeros between it and the digits, and an exponent. */
#define NVO_DECIMAL_TEXT_MAX 48

/** How a computation ends, which is what nvo raises as an error when it
    is not NVO_OK (docs/spec/shell.md L3). */
typedef enum {
    NVO_OK = 0,             /**< the result is set */
    NVO_OVERFLOW = 1,       /**< the result does not fit: `overflow` */
    NVO_DIVIDE_BY_ZERO = 2, /**< a nonzero number divided by zero */
    NVO_INVALID = 3,        /**< no result: 0 / 0, or an integer quotient
                                 past 34 digits (an invalid operation) */
    NVO_SYNTAX = 4,         /**< text that is not a number */
    NVO_TYPE = 5,           /**< operands of a type the operation does not
                                 take: `type` */
    NVO_MEMORY = 6          /**< the memory the caller gave is too small */
} NVO_STATUS;

/** A decimal: coefficient x 10^exponent, negated if negative. Every
    value nvo_decimal makes has a coefficient below 10^34 and an exponent
    from NVO_DECIMAL_ETINY to NVO_DECIMAL_EXPONENT_MAX; a caller sets one
    only through this file. */
typedef struct {
    UINT32 limb[NVO_DECIMAL_LIMBS]; /**< the coefficient, base 10^9, the
                                         least significant limb first */
    INT32 exponent;                 /**< the power of ten of the
                                         coefficient's last digit */
    UINT32 negative;                /**< 1 if the number has a minus sign
                                         (zero included), else 0 */
} NVO_DECIMAL;

/** Set x to zero. */
void nvo_decimal_zero(NVO_DECIMAL *x);

/** Set x to the integer v, exactly. */
void nvo_decimal_from_int(NVO_DECIMAL *x, INT64 v);

/** Set to to from. */
void nvo_decimal_copy(NVO_DECIMAL *to, const NVO_DECIMAL *from);

/** Set x to the number the length bytes of text spell, rounded to 34
    digits half-even: an optional sign, digits with at most one point,
    before or after which there may be none, and an exponent, as
    Decimal's numeric string (`1`, `-0.5`, `2.5e-3`, `1E+6`). Returns
    NVO_OK; NVO_SYNTAX if text is anything else; or NVO_OVERFLOW if the
    number is too large, with x unchanged in both. */
NVO_STATUS nvo_decimal_from_text(NVO_DECIMAL *x, const UINT8 *text,
                                 UINT64 length);

/** Write x as Decimal's to-scientific-string (`1.25`, `0.00123`, `1E+3`,
    `-0`) into out, which holds NVO_DECIMAL_TEXT_MAX bytes at least, with
    no terminator. Returns the bytes written. */
UINT32 nvo_decimal_to_text(const NVO_DECIMAL *x, UINT8 *out);

/** Set r to a + b. Returns NVO_OK or NVO_OVERFLOW; r is unchanged on an
    error. r may be a or b. */
NVO_STATUS nvo_decimal_add(NVO_DECIMAL *r, const NVO_DECIMAL *a,
                           const NVO_DECIMAL *b);

/** Set r to a + b as nvo_decimal_add does, and set *inexact to 1 if the sum
    had to be rounded, so that r is not a + b exactly (Decimal's Inexact
    condition), and else to 0. Digits that are zeros are not lost by
    rounding, so a sum with trailing zeros past 34 digits is exact.
    *inexact is unchanged on an error. */
NVO_STATUS nvo_decimal_add_inexact(NVO_DECIMAL *r, const NVO_DECIMAL *a,
                                   const NVO_DECIMAL *b, int *inexact);

/** Set r to a - b, which is a + (-b) with the sign of zero Decimal
    gives. Statuses and aliasing as nvo_decimal_add's. */
NVO_STATUS nvo_decimal_subtract(NVO_DECIMAL *r, const NVO_DECIMAL *a,
                                const NVO_DECIMAL *b);

/** Set r to a x b. Statuses and aliasing as nvo_decimal_add's. */
NVO_STATUS nvo_decimal_multiply(NVO_DECIMAL *r, const NVO_DECIMAL *a,
                                const NVO_DECIMAL *b);

/** Set r to a / b rounded to 34 digits: exact where it can be, with the
    exponent Decimal calls ideal. Returns NVO_OK, NVO_OVERFLOW,
    NVO_DIVIDE_BY_ZERO if b is zero and a is not, or NVO_INVALID if both
    are. Aliasing as nvo_decimal_add's. */
NVO_STATUS nvo_decimal_divide(NVO_DECIMAL *r, const NVO_DECIMAL *a,
                              const NVO_DECIMAL *b);

/** Set r to the integer part of a / b, truncated toward zero, with
    exponent 0 (Decimal's divide-integer). Returns NVO_OK;
    NVO_DIVIDE_BY_ZERO or NVO_INVALID as nvo_decimal_divide does; or
    NVO_INVALID if the integer needs more than 34 digits. */
NVO_STATUS nvo_decimal_divide_integer(NVO_DECIMAL *r, const NVO_DECIMAL *a,
                                      const NVO_DECIMAL *b);

/** Set r to a - b x (the integer part of a / b): the remainder, with a's
    sign and exactly (Decimal's remainder). Returns NVO_OK or NVO_INVALID,
    which is also what a zero b gives. */
NVO_STATUS nvo_decimal_remainder(NVO_DECIMAL *r, const NVO_DECIMAL *a,
                                 const NVO_DECIMAL *b);

/** Set r to 0 - x, which is -x except that the sign of zero stays plus
    (Decimal's minus). r may be x. */
void nvo_decimal_negate(NVO_DECIMAL *r, const NVO_DECIMAL *x);

/** Whether x is zero, of either sign. */
int nvo_decimal_is_zero(const NVO_DECIMAL *x);

/** Returns -1, 0 or 1 as a is less than, equal to or greater than b by
    value: zero of either sign is equal to zero, and 1.0 to 1. */
int nvo_decimal_compare(const NVO_DECIMAL *a, const NVO_DECIMAL *b);

/** Set *v to x as an integer if it is a whole number that fits an INT64.
    Returns NVO_OK; NVO_OVERFLOW if it is whole and does not fit; or
    NVO_INVALID if x has a fractional part. *v is unchanged on an error. */
NVO_STATUS nvo_decimal_to_int(const NVO_DECIMAL *x, INT64 *v);

#endif
