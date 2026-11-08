/* nvo's values (nvo_value.h): docs/spec/shell.md L3.

   A value is copied member by member, never by struct assignment: the
   guest's compiler turns a struct copy into a call to a copy function the
   guest does not have. */

#include "nvo_value.h"

#define INT64_LARGEST 0x7fffffffffffffffLL
#define INT64_SMALLEST (-0x7fffffffffffffffLL - 1)
#define MAGNITUDE_SMALLEST 0x8000000000000000ULL

/* How an arena rounds what it gives. */
#define ARENA_ALIGNMENT 8u

/* ------------------------------------------------------------------------
   Construction */

void nvo_arena_init(NVO_ARENA *arena, void *memory, UINT64 size)
{
    arena->base = (UINT8 *)memory;
    arena->size = size;
    arena->used = 0;
}

/* The count bytes from the arena, aligned, or 0. */
static void *arena_take(NVO_ARENA *arena, UINT64 count)
{
    UINT64 start = (arena->used + (ARENA_ALIGNMENT - 1)) &
                   ~(UINT64)(ARENA_ALIGNMENT - 1);

    if (start < arena->used || start > arena->size ||
        count > arena->size - start)
        return 0;
    arena->used = start + count;
    return arena->base + start;
}

static void reset(NVO_VALUE *v, NVO_VALUE_KIND kind)
{
    v->kind = kind;
    v->error_kind = 0;
}

void nvo_nothing(NVO_VALUE *v)
{
    reset(v, NVO_VALUE_NOTHING);
    v->u.integer = 0;
}

void nvo_bool(NVO_VALUE *v, int truth)
{
    reset(v, NVO_VALUE_BOOL);
    v->u.boolean = truth ? 1u : 0u;
}

static void set_integer(NVO_VALUE *v, NVO_VALUE_KIND kind, INT64 n)
{
    reset(v, kind);
    v->u.integer = n;
}

void nvo_int(NVO_VALUE *v, INT64 n) { set_integer(v, NVO_VALUE_INT, n); }

void nvo_duration(NVO_VALUE *v, INT64 n)
{
    set_integer(v, NVO_VALUE_DURATION, n);
}

void nvo_size(NVO_VALUE *v, INT64 n) { set_integer(v, NVO_VALUE_SIZE, n); }

void nvo_time(NVO_VALUE *v, INT64 n) { set_integer(v, NVO_VALUE_TIME, n); }

void nvo_decimal_value(NVO_VALUE *v, const NVO_DECIMAL *d)
{
    reset(v, NVO_VALUE_DECIMAL);
    nvo_decimal_copy(&v->u.decimal, d);
}

static void set_text(NVO_VALUE *v, NVO_VALUE_KIND kind, const UINT8 *data,
                     UINT64 length)
{
    reset(v, kind);
    v->u.text.data = data;
    v->u.text.length = length;
}

NVO_STATUS nvo_string(NVO_VALUE *v, const UINT8 *data, UINT64 length)
{
    UINT64 at;

    if (!nvo_utf8_check(data, length, &at))
        return NVO_INVALID;
    set_text(v, NVO_VALUE_STRING, data, length);
    return NVO_OK;
}

void nvo_bytes(NVO_VALUE *v, const UINT8 *data, UINT64 length)
{
    set_text(v, NVO_VALUE_BYTES, data, length);
}

NVO_STATUS nvo_path(NVO_VALUE *v, const UINT8 *data, UINT64 length)
{
    UINT64 at;

    if (length == 0 || !nvo_utf8_check(data, length, &at))
        return NVO_INVALID;
    set_text(v, NVO_VALUE_PATH, data, length);
    return NVO_OK;
}

void nvo_list(NVO_VALUE *v, const NVO_VALUE *items, UINT64 count)
{
    reset(v, NVO_VALUE_LIST);
    v->u.list.items = items;
    v->u.list.count = count;
}

static int same_bytes(const UINT8 *a, UINT64 a_length, const UINT8 *b,
                      UINT64 b_length)
{
    UINT64 i;

    if (a_length != b_length)
        return 0;
    for (i = 0; i < a_length; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

NVO_STATUS nvo_record(NVO_VALUE *v, const NVO_FIELD *fields, UINT64 count)
{
    UINT64 i, j, at;

    for (i = 0; i < count; i++) {
        if (!nvo_utf8_check(fields[i].key, fields[i].key_length, &at))
            return NVO_INVALID;
        for (j = 0; j < i; j++)
            if (same_bytes(fields[i].key, fields[i].key_length, fields[j].key,
                           fields[j].key_length))
                return NVO_INVALID;
    }
    reset(v, NVO_VALUE_RECORD);
    v->u.record.fields = fields;
    v->u.record.count = count;
    return NVO_OK;
}

void nvo_from_number(NVO_VALUE *v, const NVO_NUMBER *number)
{
    switch (number->kind) {
    case NVO_NUMBER_DECIMAL:
        nvo_decimal_value(v, &number->decimal);
        break;
    case NVO_NUMBER_SIZE:
        nvo_size(v, number->integer);
        break;
    case NVO_NUMBER_DURATION:
        nvo_duration(v, number->integer);
        break;
    default:
        nvo_int(v, number->integer);
        break;
    }
}

/* Set to to from, member by member. */
static void copy_value(NVO_VALUE *to, const NVO_VALUE *from)
{
    NVO_VALUE_KIND kind = from->kind;
    UINT32 error_kind = from->error_kind;

    switch (kind) {
    case NVO_VALUE_BOOL:
        nvo_bool(to, from->u.boolean != 0);
        break;
    case NVO_VALUE_DECIMAL:
        nvo_decimal_value(to, &from->u.decimal);
        break;
    case NVO_VALUE_STRING:
    case NVO_VALUE_BYTES:
    case NVO_VALUE_PATH:
    case NVO_VALUE_ERROR:
        set_text(to, kind, from->u.text.data, from->u.text.length);
        break;
    case NVO_VALUE_LIST:
        nvo_list(to, from->u.list.items, from->u.list.count);
        break;
    case NVO_VALUE_RECORD:
        reset(to, kind);
        to->u.record.fields = from->u.record.fields;
        to->u.record.count = from->u.record.count;
        break;
    case NVO_VALUE_CLOSURE:
        reset(to, kind);
        to->u.closure.code = from->u.closure.code;
        to->u.closure.captured = from->u.closure.captured;
        to->u.closure.count = from->u.closure.count;
        break;
    case NVO_VALUE_NOTHING:
        nvo_nothing(to);
        break;
    default:
        set_integer(to, kind, from->u.integer);
        break;
    }
    to->error_kind = error_kind;
}

/* ------------------------------------------------------------------------
   Kinds and tables */

static int same_keys(const NVO_VALUE *a, const NVO_VALUE *b)
{
    UINT64 i;

    if (a->u.record.count != b->u.record.count)
        return 0;
    for (i = 0; i < a->u.record.count; i++)
        if (!same_bytes(a->u.record.fields[i].key,
                        a->u.record.fields[i].key_length,
                        b->u.record.fields[i].key,
                        b->u.record.fields[i].key_length))
            return 0;
    return 1;
}

int nvo_is_table(const NVO_VALUE *v)
{
    UINT64 i;

    if (v->kind != NVO_VALUE_LIST || v->u.list.count == 0)
        return 0;
    for (i = 0; i < v->u.list.count; i++) {
        const NVO_VALUE *row = &v->u.list.items[i];

        if (row->kind != NVO_VALUE_RECORD || row->u.record.count == 0 ||
            !same_keys(&v->u.list.items[0], row))
            return 0;
    }
    return 1;
}

NVO_VALUE_KIND nvo_kind_of(const NVO_VALUE *v)
{
    return nvo_is_table(v) ? NVO_VALUE_TABLE : v->kind;
}

static int is_number(const NVO_VALUE *v)
{
    return v->kind == NVO_VALUE_INT || v->kind == NVO_VALUE_DECIMAL;
}

/* An int is widened to a decimal exactly. */
static void as_decimal(NVO_DECIMAL *d, const NVO_VALUE *v)
{
    if (v->kind == NVO_VALUE_INT)
        nvo_decimal_from_int(d, v->u.integer);
    else
        nvo_decimal_copy(d, &v->u.decimal);
}

/* ------------------------------------------------------------------------
   Equality and order */

static NVO_STATUS equal_at(const NVO_VALUE *a, const NVO_VALUE *b, int *equal,
                           UINT32 depth)
{
    UINT64 i;

    if (depth > NVO_VALUE_DEPTH_MAX)
        return NVO_OVERFLOW;
    *equal = 0;
    if (is_number(a) && is_number(b)) {
        NVO_DECIMAL x, y;

        as_decimal(&x, a);
        as_decimal(&y, b);
        *equal = nvo_decimal_compare(&x, &y) == 0;
        return NVO_OK;
    }
    if (a->kind != b->kind)
        return NVO_OK;
    switch (a->kind) {
    case NVO_VALUE_NOTHING:
        *equal = 1;
        break;
    case NVO_VALUE_BOOL:
        *equal = a->u.boolean == b->u.boolean;
        break;
    case NVO_VALUE_STRING:
    case NVO_VALUE_BYTES:
    case NVO_VALUE_PATH:
        *equal = same_bytes(a->u.text.data, a->u.text.length, b->u.text.data,
                            b->u.text.length);
        break;
    case NVO_VALUE_ERROR:
        *equal = a->error_kind == b->error_kind &&
                 same_bytes(a->u.text.data, a->u.text.length, b->u.text.data,
                            b->u.text.length);
        break;
    case NVO_VALUE_LIST:
    case NVO_VALUE_CLOSURE: {
        UINT64 count = a->kind == NVO_VALUE_LIST ? a->u.list.count
                                                 : a->u.closure.count;
        const NVO_VALUE *left = a->kind == NVO_VALUE_LIST
                                    ? a->u.list.items
                                    : a->u.closure.captured;
        const NVO_VALUE *right = a->kind == NVO_VALUE_LIST
                                     ? b->u.list.items
                                     : b->u.closure.captured;

        if (count != (a->kind == NVO_VALUE_LIST ? b->u.list.count
                                                : b->u.closure.count) ||
            (a->kind == NVO_VALUE_CLOSURE &&
             a->u.closure.code != b->u.closure.code))
            return NVO_OK;
        for (i = 0; i < count; i++) {
            int same;
            NVO_STATUS status = equal_at(&left[i], &right[i], &same, depth + 1);

            if (status != NVO_OK)
                return status;
            if (!same)
                return NVO_OK;
        }
        *equal = 1;
        break;
    }
    case NVO_VALUE_RECORD:
        if (!same_keys(a, b))
            return NVO_OK;
        for (i = 0; i < a->u.record.count; i++) {
            int same;
            NVO_STATUS status =
                equal_at(a->u.record.fields[i].value,
                         b->u.record.fields[i].value, &same, depth + 1);

            if (status != NVO_OK)
                return status;
            if (!same)
                return NVO_OK;
        }
        *equal = 1;
        break;
    case NVO_VALUE_DURATION:
    case NVO_VALUE_SIZE:
    case NVO_VALUE_TIME:
        *equal = a->u.integer == b->u.integer;
        break;
    default:
        /* Numbers were compared above, and a table is a list. */
        break;
    }
    return NVO_OK;
}

NVO_STATUS nvo_equal(const NVO_VALUE *a, const NVO_VALUE *b, int *equal)
{
    return equal_at(a, b, equal, 0);
}

static int sign_of(int order) { return order < 0 ? -1 : (order > 0 ? 1 : 0); }

/* Bytes compared as unsigned, a prefix before what extends it. */
static int compare_bytes(const UINT8 *a, UINT64 a_length, const UINT8 *b,
                         UINT64 b_length)
{
    UINT64 i, n = a_length < b_length ? a_length : b_length;

    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return a_length < b_length ? -1 : (a_length > b_length ? 1 : 0);
}

NVO_STATUS nvo_order(const NVO_VALUE *a, const NVO_VALUE *b, int *order)
{
    if (a->kind == NVO_VALUE_INT && b->kind == NVO_VALUE_INT) {
        *order = a->u.integer < b->u.integer
                     ? -1
                     : (a->u.integer > b->u.integer ? 1 : 0);
        return NVO_OK;
    }
    if (is_number(a) && is_number(b)) {
        NVO_DECIMAL x, y;

        as_decimal(&x, a);
        as_decimal(&y, b);
        *order = sign_of(nvo_decimal_compare(&x, &y));
        return NVO_OK;
    }
    if (a->kind != b->kind)
        return NVO_TYPE;
    switch (a->kind) {
    case NVO_VALUE_STRING:
    case NVO_VALUE_PATH:
        *order = compare_bytes(a->u.text.data, a->u.text.length, b->u.text.data,
                               b->u.text.length);
        return NVO_OK;
    case NVO_VALUE_SIZE:
    case NVO_VALUE_DURATION:
    case NVO_VALUE_TIME:
        *order = a->u.integer < b->u.integer
                     ? -1
                     : (a->u.integer > b->u.integer ? 1 : 0);
        return NVO_OK;
    default:
        return NVO_TYPE;
    }
}

NVO_STATUS nvo_condition(const NVO_VALUE *v, int *truth)
{
    if (v->kind != NVO_VALUE_BOOL)
        return NVO_TYPE;
    *truth = v->u.boolean != 0;
    return NVO_OK;
}

/* ------------------------------------------------------------------------
   Arithmetic */

static NVO_STATUS add_checked(INT64 a, INT64 b, INT64 *r)
{
    if ((b > 0 && a > INT64_LARGEST - b) || (b < 0 && a < INT64_SMALLEST - b))
        return NVO_OVERFLOW;
    *r = a + b;
    return NVO_OK;
}

static NVO_STATUS subtract_checked(INT64 a, INT64 b, INT64 *r)
{
    if ((b < 0 && a > INT64_LARGEST + b) || (b > 0 && a < INT64_SMALLEST + b))
        return NVO_OVERFLOW;
    *r = a - b;
    return NVO_OK;
}

static NVO_STATUS multiply_checked(INT64 a, INT64 b, INT64 *r)
{
    UINT64 ma = a < 0 ? (UINT64)0 - (UINT64)a : (UINT64)a;
    UINT64 mb = b < 0 ? (UINT64)0 - (UINT64)b : (UINT64)b;
    int negative = (a < 0) != (b < 0);
    UINT64 limit = negative ? MAGNITUDE_SMALLEST : (UINT64)INT64_LARGEST;

    if (ma != 0 && mb > limit / ma)
        return NVO_OVERFLOW;
    *r = negative ? (INT64)((UINT64)0 - ma * mb) : (INT64)(ma * mb);
    return NVO_OK;
}

/* Set r to the result of a decimal operation. */
static NVO_STATUS decimal_to(NVO_VALUE *r, NVO_STATUS status,
                             const NVO_DECIMAL *d)
{
    if (status == NVO_OK)
        nvo_decimal_value(r, d);
    return status;
}

typedef NVO_STATUS (*DECIMAL_OP)(NVO_DECIMAL *, const NVO_DECIMAL *,
                                 const NVO_DECIMAL *);
typedef NVO_STATUS (*INT_OP)(INT64, INT64, INT64 *);

/* An operation on two numbers: two ints by integer_op, else as decimals. */
static NVO_STATUS number_operation(NVO_VALUE *r, const NVO_VALUE *a,
                                   const NVO_VALUE *b, INT_OP integer_op,
                                   DECIMAL_OP decimal_op)
{
    NVO_DECIMAL x, y, z;
    INT64 n;
    NVO_STATUS status;

    if (a->kind == NVO_VALUE_INT && b->kind == NVO_VALUE_INT) {
        status = integer_op(a->u.integer, b->u.integer, &n);
        if (status == NVO_OK)
            nvo_int(r, n);
        return status;
    }
    as_decimal(&x, a);
    as_decimal(&y, b);
    status = decimal_op(&z, &x, &y);
    return decimal_to(r, status, &z);
}

/* An operation on two values of one unit kind (size or duration). */
static NVO_STATUS unit_operation(NVO_VALUE *r, NVO_VALUE_KIND kind, INT64 a,
                                 INT64 b, INT_OP integer_op)
{
    INT64 n;
    NVO_STATUS status = integer_op(a, b, &n);

    if (status == NVO_OK)
        set_integer(r, kind, n);
    return status;
}

NVO_STATUS nvo_add(NVO_VALUE *r, const NVO_VALUE *a, const NVO_VALUE *b)
{
    NVO_VALUE out;
    NVO_STATUS status;

    if (is_number(a) && is_number(b))
        status = number_operation(&out, a, b, add_checked, nvo_decimal_add);
    else if (a->kind == b->kind && (a->kind == NVO_VALUE_SIZE ||
                                    a->kind == NVO_VALUE_DURATION))
        status = unit_operation(&out, a->kind, a->u.integer, b->u.integer,
                                add_checked);
    else if ((a->kind == NVO_VALUE_TIME && b->kind == NVO_VALUE_DURATION) ||
             (a->kind == NVO_VALUE_DURATION && b->kind == NVO_VALUE_TIME))
        status = unit_operation(&out, NVO_VALUE_TIME, a->u.integer,
                                b->u.integer, add_checked);
    else
        return NVO_TYPE;
    if (status == NVO_OK)
        copy_value(r, &out);
    return status;
}

NVO_STATUS nvo_subtract(NVO_VALUE *r, const NVO_VALUE *a, const NVO_VALUE *b)
{
    NVO_VALUE out;
    NVO_STATUS status;

    if (is_number(a) && is_number(b))
        status = number_operation(&out, a, b, subtract_checked,
                                  nvo_decimal_subtract);
    else if (a->kind == b->kind && (a->kind == NVO_VALUE_SIZE ||
                                    a->kind == NVO_VALUE_DURATION))
        status = unit_operation(&out, a->kind, a->u.integer, b->u.integer,
                                subtract_checked);
    else if (a->kind == NVO_VALUE_TIME && b->kind == NVO_VALUE_DURATION)
        status = unit_operation(&out, NVO_VALUE_TIME, a->u.integer,
                                b->u.integer, subtract_checked);
    else if (a->kind == NVO_VALUE_TIME && b->kind == NVO_VALUE_TIME)
        status = unit_operation(&out, NVO_VALUE_DURATION, a->u.integer,
                                b->u.integer, subtract_checked);
    else
        return NVO_TYPE;
    if (status == NVO_OK)
        copy_value(r, &out);
    return status;
}

static int is_unit(const NVO_VALUE *v)
{
    return v->kind == NVO_VALUE_SIZE || v->kind == NVO_VALUE_DURATION;
}

NVO_STATUS nvo_multiply(NVO_VALUE *r, const NVO_VALUE *a, const NVO_VALUE *b)
{
    NVO_VALUE out;
    NVO_STATUS status;

    if (is_number(a) && is_number(b))
        status = number_operation(&out, a, b, multiply_checked,
                                  nvo_decimal_multiply);
    else if (is_unit(a) && b->kind == NVO_VALUE_INT)
        status = unit_operation(&out, a->kind, a->u.integer, b->u.integer,
                                multiply_checked);
    else if (a->kind == NVO_VALUE_INT && is_unit(b))
        status = unit_operation(&out, b->kind, a->u.integer, b->u.integer,
                                multiply_checked);
    else
        return NVO_TYPE;
    if (status == NVO_OK)
        copy_value(r, &out);
    return status;
}

NVO_STATUS nvo_negate(NVO_VALUE *r, const NVO_VALUE *a)
{
    NVO_VALUE out;

    if (a->kind == NVO_VALUE_DECIMAL) {
        NVO_DECIMAL d;

        nvo_decimal_negate(&d, &a->u.decimal);
        nvo_decimal_value(&out, &d);
    } else if (a->kind == NVO_VALUE_INT || is_unit(a)) {
        if (a->u.integer == INT64_SMALLEST)
            return NVO_OVERFLOW;
        set_integer(&out, a->kind, -a->u.integer);
    } else {
        return NVO_TYPE;
    }
    copy_value(r, &out);
    return NVO_OK;
}

static int is_zero(const NVO_VALUE *v)
{
    return v->kind == NVO_VALUE_INT ? v->u.integer == 0
                                    : nvo_decimal_is_zero(&v->u.decimal);
}

NVO_STATUS nvo_divide(NVO_VALUE *r, const NVO_VALUE *a, const NVO_VALUE *b)
{
    NVO_DECIMAL x, y, z;
    NVO_STATUS status;

    if (!is_number(a) || !is_number(b))
        return NVO_TYPE;
    if (is_zero(b))
        return NVO_DIVIDE_BY_ZERO;
    as_decimal(&x, a);
    as_decimal(&y, b);
    status = nvo_decimal_divide(&z, &x, &y);
    return decimal_to(r, status, &z);
}

/* The floor of a / b, as ints; b is not zero. */
static NVO_STATUS floor_divide(INT64 a, INT64 b, INT64 *q)
{
    INT64 truncated, rest;

    if (a == INT64_SMALLEST && b == -1)
        return NVO_OVERFLOW;
    truncated = a / b;
    rest = a % b;
    *q = (rest != 0 && ((rest < 0) != (b < 0))) ? truncated - 1 : truncated;
    return NVO_OK;
}

/* The remainder of a floored division of ints; b is not zero. Dividing
   by -1 has no remainder, and is not computed, as INT64's smallest over -1
   overflows in the machine. */
static INT64 floor_remainder(INT64 a, INT64 b)
{
    INT64 r;

    if (b == -1)
        return 0;
    r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? r + b : r;
}

NVO_STATUS nvo_divide_floor(NVO_VALUE *r, const NVO_VALUE *a,
                            const NVO_VALUE *b)
{
    NVO_DECIMAL x, y, q, rest, one;
    INT64 n;
    NVO_STATUS status;

    if (!is_number(a) || !is_number(b))
        return NVO_TYPE;
    if (is_zero(b))
        return NVO_DIVIDE_BY_ZERO;
    if (a->kind == NVO_VALUE_INT && b->kind == NVO_VALUE_INT) {
        status = floor_divide(a->u.integer, b->u.integer, &n);
        if (status == NVO_OK)
            nvo_int(r, n);
        return status;
    }
    as_decimal(&x, a);
    as_decimal(&y, b);
    /* An integer part past 34 digits is past an int as well. */
    if (nvo_decimal_divide_integer(&q, &x, &y) != NVO_OK ||
        nvo_decimal_remainder(&rest, &x, &y) != NVO_OK)
        return NVO_OVERFLOW;
    if (!nvo_decimal_is_zero(&rest) && rest.negative != y.negative) {
        nvo_decimal_from_int(&one, 1);
        if (nvo_decimal_subtract(&q, &q, &one) != NVO_OK)
            return NVO_OVERFLOW;
    }
    status = nvo_decimal_to_int(&q, &n);
    if (status != NVO_OK)
        return NVO_OVERFLOW;
    nvo_int(r, n);
    return NVO_OK;
}

NVO_STATUS nvo_remainder(NVO_VALUE *r, const NVO_VALUE *a, const NVO_VALUE *b)
{
    NVO_DECIMAL x, y, rest;
    int inexact = 0;

    if (!is_number(a) || !is_number(b))
        return NVO_TYPE;
    if (is_zero(b))
        return NVO_DIVIDE_BY_ZERO;
    if (a->kind == NVO_VALUE_INT && b->kind == NVO_VALUE_INT) {
        nvo_int(r, floor_remainder(a->u.integer, b->u.integer));
        return NVO_OK;
    }
    as_decimal(&x, a);
    as_decimal(&y, b);
    if (nvo_decimal_remainder(&rest, &x, &y) != NVO_OK)
        return NVO_OVERFLOW;
    /* The floored remainder is the truncated one plus the divisor when
       their signs differ. The sum needs more than 34 digits when a's
       exponent is below b's, and then no decimal is the remainder: the
       rounded sum can equal the divisor, so it is an overflow. */
    if (!nvo_decimal_is_zero(&rest) && rest.negative != y.negative &&
        (nvo_decimal_add_inexact(&rest, &rest, &y, &inexact) != NVO_OK ||
         inexact))
        return NVO_OVERFLOW;
    nvo_decimal_value(r, &rest);
    return NVO_OK;
}

/* ------------------------------------------------------------------------
   Concatenation and joining */

static void copy_bytes(UINT8 *to, const UINT8 *from, UINT64 count)
{
    UINT64 i;

    for (i = 0; i < count; i++)
        to[i] = from[i];
}

NVO_STATUS nvo_concat(NVO_ARENA *arena, NVO_VALUE *r, const NVO_VALUE *a,
                      const NVO_VALUE *b)
{
    UINT64 i;

    if (a->kind != b->kind)
        return NVO_TYPE;
    if (a->kind == NVO_VALUE_STRING || a->kind == NVO_VALUE_BYTES) {
        UINT64 length = a->u.text.length + b->u.text.length;
        UINT8 *data = 0;

        if (length < a->u.text.length)
            return NVO_MEMORY;
        if (length > 0) {
            data = (UINT8 *)arena_take(arena, length);
            if (!data)
                return NVO_MEMORY;
            copy_bytes(data, a->u.text.data, a->u.text.length);
            copy_bytes(data + a->u.text.length, b->u.text.data,
                       b->u.text.length);
        }
        set_text(r, a->kind, data, length);
        return NVO_OK;
    }
    if (a->kind == NVO_VALUE_LIST) {
        UINT64 count = a->u.list.count + b->u.list.count;
        NVO_VALUE *items = 0;

        if (count < a->u.list.count ||
            count > arena->size / sizeof(NVO_VALUE))
            return NVO_MEMORY;
        if (count > 0) {
            items = (NVO_VALUE *)arena_take(arena, count * sizeof(NVO_VALUE));
            if (!items)
                return NVO_MEMORY;
            for (i = 0; i < a->u.list.count; i++)
                copy_value(&items[i], &a->u.list.items[i]);
            for (i = 0; i < b->u.list.count; i++)
                copy_value(&items[a->u.list.count + i], &b->u.list.items[i]);
        }
        nvo_list(r, items, count);
        return NVO_OK;
    }
    return NVO_TYPE;
}

NVO_STATUS nvo_join(NVO_ARENA *arena, NVO_VALUE *r, const NVO_VALUE *a,
                    const NVO_VALUE *b)
{
    UINT64 slash, length;
    UINT8 *data;

    if (a->kind != NVO_VALUE_PATH ||
        (b->kind != NVO_VALUE_STRING && b->kind != NVO_VALUE_PATH))
        return NVO_TYPE;
    if (b->u.text.length > 0 && b->u.text.data[0] == '/')
        return NVO_TYPE;
    slash = a->u.text.length == 0 || a->u.text.data[a->u.text.length - 1] == '/'
                ? 0u
                : 1u;
    length = a->u.text.length + slash + b->u.text.length;
    if (length < a->u.text.length)
        return NVO_MEMORY;
    data = (UINT8 *)arena_take(arena, length);
    if (!data)
        return NVO_MEMORY;
    copy_bytes(data, a->u.text.data, a->u.text.length);
    if (slash)
        data[a->u.text.length] = '/';
    copy_bytes(data + a->u.text.length + slash, b->u.text.data,
               b->u.text.length);
    set_text(r, NVO_VALUE_PATH, data, length);
    return NVO_OK;
}
