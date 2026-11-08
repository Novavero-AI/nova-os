/** @file
    nvo's values and the rules that govern them (docs/spec/shell.md L3):
    the types, equality and order, conditions, and the arithmetic and
    concatenation of the operators.

    A value holds no memory of its own that it owns: strings, bytes, lists
    and records point at memory the caller keeps, and an operation that
    makes more of it, `++`, takes it from an NVO_ARENA the caller gives.
    Nothing here makes a system call, so the guest and every host give the
    same results. The statuses are NVO_STATUS's, and each is the error L11
    names: NVO_TYPE is `type`, NVO_OVERFLOW `overflow`,
    NVO_DIVIDE_BY_ZERO `division-by-zero`. */

#ifndef NOVA_NVO_VALUE_H
#define NOVA_NVO_VALUE_H

#include "nvo_syntax.h"

/** The deepest nesting of lists, records and closures the comparison of two
    values follows; deeper raises NVO_OVERFLOW, so that no value, however it
    was built, uses more than a bounded stack. */
#define NVO_VALUE_DEPTH_MAX 64

/** The types of values, as L3 lists them. */
typedef enum {
    NVO_VALUE_NOTHING,  /**< no value */
    NVO_VALUE_BOOL,     /**< `true` or `false` */
    NVO_VALUE_INT,      /**< a 64-bit two's-complement integer */
    NVO_VALUE_DECIMAL,  /**< a 34-digit decimal */
    NVO_VALUE_STRING,   /**< text, UTF-8 */
    NVO_VALUE_BYTES,    /**< octets in no stated encoding */
    NVO_VALUE_PATH,     /**< a name in the namespace, as written */
    NVO_VALUE_DURATION, /**< a signed count of nanoseconds */
    NVO_VALUE_SIZE,     /**< a signed count of bytes */
    NVO_VALUE_TIME,     /**< nanoseconds since 1970-01-01T00:00:00 UTC */
    NVO_VALUE_LIST,     /**< an ordered sequence of values */
    NVO_VALUE_RECORD,   /**< ordered, unique string keys with values */
    NVO_VALUE_TABLE,    /**< a list that is a table: never a value's own
                             kind, only what nvo_kind_of reports */
    NVO_VALUE_CLOSURE,  /**< code with the values it captured */
    NVO_VALUE_ERROR     /**< a raised failure as a value */
} NVO_VALUE_KIND;

struct NVO_VALUE;

/** A record's field: a key and its value. */
typedef struct NVO_FIELD {
    const UINT8 *key;              /**< the key's UTF-8 bytes */
    UINT64 key_length;             /**< their count */
    const struct NVO_VALUE *value; /**< its value */
} NVO_FIELD;

/** A value. Which member of the union holds it is its kind's. */
typedef struct NVO_VALUE {
    NVO_VALUE_KIND kind; /**< what the value is; never NVO_VALUE_TABLE */
    union {
        UINT32 boolean;      /**< bool: 0 or 1 */
        INT64 integer;       /**< int, duration, size and time */
        NVO_DECIMAL decimal; /**< decimal */
        struct {
            const UINT8 *data; /**< the bytes */
            UINT64 length;     /**< their count */
        } text;                /**< string, bytes, path, an error's message */
        struct {
            const struct NVO_VALUE *items; /**< the elements */
            UINT64 count;                  /**< their count */
        } list;                            /**< list */
        struct {
            const NVO_FIELD *fields; /**< the fields in order */
            UINT64 count;            /**< their count */
        } record;                    /**< record */
        struct {
            const void *code;                 /**< the code, which this library
                                                   does not read */
            const struct NVO_VALUE *captured; /**< the values it captured */
            UINT64 count;                     /**< their count */
        } closure;                            /**< closure */
    } u;                                      /**< the value's own members */
    UINT32 error_kind; /**< error: which of L11's kinds, as the caller numbers
                            them */
} NVO_VALUE;

/** Memory the caller gives for what an operation makes: a block it filled
    from the front. */
typedef struct {
    UINT8 *base;  /**< the block */
    UINT64 size;  /**< its bytes */
    UINT64 used;  /**< the bytes taken from the front */
} NVO_ARENA;

/** Set arena to hold the size bytes at memory, empty. memory is aligned to
    8 bytes, which the arena keeps for what it gives. */
void nvo_arena_init(NVO_ARENA *arena, void *memory, UINT64 size);

/** Set v to `nothing`. */
void nvo_nothing(NVO_VALUE *v);

/** Set v to the bool truth. */
void nvo_bool(NVO_VALUE *v, int truth);

/** Set v to the int n. */
void nvo_int(NVO_VALUE *v, INT64 n);

/** Set v to the decimal d. */
void nvo_decimal_value(NVO_VALUE *v, const NVO_DECIMAL *d);

/** Set v to the duration of n nanoseconds. */
void nvo_duration(NVO_VALUE *v, INT64 n);

/** Set v to the size of n bytes. */
void nvo_size(NVO_VALUE *v, INT64 n);

/** Set v to the time n nanoseconds after 1970-01-01T00:00:00 UTC. */
void nvo_time(NVO_VALUE *v, INT64 n);

/** Set v to the string of the length bytes at data, which stay with the
    caller. Returns NVO_OK, or NVO_INVALID if they are not UTF-8. */
NVO_STATUS nvo_string(NVO_VALUE *v, const UINT8 *data, UINT64 length);

/** Set v to the bytes of the length bytes at data. */
void nvo_bytes(NVO_VALUE *v, const UINT8 *data, UINT64 length);

/** Set v to the path written as the length bytes at data, which are UTF-8.
    The text is kept as given; resolving `.` and `..` against the working
    directory is the shell's step when a path reaches a program (L15), as it
    needs that directory. Returns NVO_OK, or NVO_INVALID if the text is empty
    or not UTF-8. */
NVO_STATUS nvo_path(NVO_VALUE *v, const UINT8 *data, UINT64 length);

/** Set v to the list of the count values at items. */
void nvo_list(NVO_VALUE *v, const NVO_VALUE *items, UINT64 count);

/** Set v to the record of the count fields at fields. Returns NVO_OK, or
    NVO_INVALID if a key is not UTF-8 or two keys are equal. */
NVO_STATUS nvo_record(NVO_VALUE *v, const NVO_FIELD *fields, UINT64 count);

/** Set v to the value of a number literal nvo_number_parse read. */
void nvo_from_number(NVO_VALUE *v, const NVO_NUMBER *number);

/** The type of v as a user sees it: NVO_VALUE_TABLE for a list that is a
    table, else v's kind. */
NVO_VALUE_KIND nvo_kind_of(const NVO_VALUE *v);

/** Whether v is a table: a list of one or more records, each with one or
    more fields, all with the same keys in the same order (L3 rule 5). */
int nvo_is_table(const NVO_VALUE *v);

/** Set *equal to whether a and b are equal (L3 rule 2): structurally,
    values of different types unequal except an int and a decimal, which
    compare by value; records equal when their keys, in order, and their
    values are. Returns NVO_OK or NVO_OVERFLOW for nesting past
    NVO_VALUE_DEPTH_MAX. */
NVO_STATUS nvo_equal(const NVO_VALUE *a, const NVO_VALUE *b, int *equal);

/** Set *order to -1, 0 or 1 as a is before, equal to or after b (L3 rule
    2). Defined within numbers, strings and paths (bytewise), sizes,
    durations and times; across any other pair of types, NVO_TYPE. */
NVO_STATUS nvo_order(const NVO_VALUE *a, const NVO_VALUE *b, int *order);

/** Set *truth to v as a condition (L3 rule 3): v must be a bool, and NVO_TYPE
    if it is not. */
NVO_STATUS nvo_condition(const NVO_VALUE *v, int *truth);

/** Set r to a + b: numbers (an int meeting a decimal is widened exactly),
    sizes and durations, and a duration and a time. NVO_TYPE for other
    operands, NVO_OVERFLOW if the result does not fit. r may be a or b. */
NVO_STATUS nvo_add(NVO_VALUE *r, const NVO_VALUE *a, const NVO_VALUE *b);

/** Set r to a - b, as nvo_add's operands go: numbers, sizes, durations, a
    time less a duration (a time), and a time less a time (a duration). */
NVO_STATUS nvo_subtract(NVO_VALUE *r, const NVO_VALUE *a, const NVO_VALUE *b);

/** Set r to a x b: numbers, and a size or a duration by an int, either
    way round. */
NVO_STATUS nvo_multiply(NVO_VALUE *r, const NVO_VALUE *a, const NVO_VALUE *b);

/** Set r to -a: an int, a decimal, a size or a duration. */
NVO_STATUS nvo_negate(NVO_VALUE *r, const NVO_VALUE *a);

/** Set r to a / b: a decimal, for numbers (two ints included), with
    NVO_DIVIDE_BY_ZERO if b is zero. A path joined to a string or to a relative
    path is nvo_join's. */
NVO_STATUS nvo_divide(NVO_VALUE *r, const NVO_VALUE *a, const NVO_VALUE *b);

/** Set r to a // b: the floor of the quotient, as an int, for numbers.
    NVO_DIVIDE_BY_ZERO if b is zero; NVO_OVERFLOW if the result is not an
    int. */
NVO_STATUS nvo_divide_floor(NVO_VALUE *r, const NVO_VALUE *a,
                            const NVO_VALUE *b);

/** Set r to a % b, the remainder that makes
    a == (a // b) * b + a % b: an int for two ints, else a decimal, which
    has the sign of b unless it is zero, and a zero keeps the sign of a.
    NVO_DIVIDE_BY_ZERO if b is zero; NVO_OVERFLOW if the integer
    quotient of decimals has more than 34 digits, or the remainder, which is
    exact or none, needs more. */
NVO_STATUS nvo_remainder(NVO_VALUE *r, const NVO_VALUE *a, const NVO_VALUE *b);

/** Set r to a ++ b: two strings, two byte sequences or two lists, in memory
    from arena. NVO_TYPE for other operands, NVO_MEMORY if arena is
    full. */
NVO_STATUS nvo_concat(NVO_ARENA *arena, NVO_VALUE *r, const NVO_VALUE *a,
                      const NVO_VALUE *b);

/** Set r to the path a joined to b with one `/`, in memory from arena: a
    path on the left and a string or a relative path on the right. NVO_TYPE
    for other operands, and for a right operand that begins with `/`;
    NVO_MEMORY if arena is full. */
NVO_STATUS nvo_join(NVO_ARENA *arena, NVO_VALUE *r, const NVO_VALUE *a,
                    const NVO_VALUE *b);

#endif
