/* Checks nvo's decimal (lib/nvo_decimal.c, docs/spec/shell.md L3) on the
   host: the General Decimal Arithmetic test cases for each operation L3
   uses, at 34 digits, and the laws the arithmetic keeps over generated
   numbers. Prints each failure and a count, and exits nonzero if any
   check fails.

   The test cases are the decQuad files of the decTest set
   (docs/spec/README.md pins the archive), unpacked into the directory
   given as the argument:

   cc -std=c99 -Wall -Wextra -pedantic -I include -I lib -o check-decimal \
     tools/check-decimal.c lib/nvo_decimal.c &&
     ./check-decimal DIRECTORY */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvo_decimal.h"

/* The longest line a test file holds, and the most tokens on one. */
#define LINE_MAX_BYTES 16384
#define TOKENS_MAX 24

/* The cases generated for each law. */
#define LAW_CASES 30000

/* A generated number's digits at most, the exponents it takes
   (NUMBER_EXPONENT_SPAN either side of 0, and now and then one of the
   extremes of the format), and the one case in EXTREME_ONE_IN that does. */
#define NUMBER_DIGITS_MAX 34
#define NUMBER_EXPONENT_SPAN 40
#define EXTREME_ONE_IN 16

/* The digits and exponents of the numbers the integer-valued laws take, so
   that every product and sum in them is exact in 34 digits. */
#define SMALL_DIGITS_MAX 12
#define SMALL_EXPONENT_SPAN 5

/* The generator's multiplier (xorshift64*, Vigna 2016). */
#define RNG_MULTIPLIER 0x2545f4914f6cdd1dULL
#define RNG_SEED 0x9e3779b97f4a7c15ULL

static unsigned failures;

static void check(int passed, const char *what)
{
    if (passed)
        return;
    printf("failed: %s\n", what);
    failures++;
}

/* ------------------------------------------------------------------------
   Test files */

/* The decQuad files L3's operations are checked against, and the cases
   each holds that the check runs: a different count means the runner
   skipped a case it should run, or ran one it should not. */
typedef struct {
    const char *name;
    unsigned cases;
} TEST_FILE;

static const TEST_FILE files[] = {
    {"dqAdd.decTest", 690},       {"dqSubtract.decTest", 332},
    {"dqMultiply.decTest", 248},  {"dqDivide.decTest", 440},
    {"dqDivideInt.decTest", 288}, {"dqRemainder.decTest", 421},
    {"dqMinus.decTest", 29},      {"dqCompare.decTest", 566},
    {"dqBase.decTest", 639},
};

typedef NVO_STATUS (*BINARY)(NVO_DECIMAL *, const NVO_DECIMAL *,
                             const NVO_DECIMAL *);

typedef enum {
    OP_ADD,
    OP_SUBTRACT,
    OP_MULTIPLY,
    OP_DIVIDE,
    OP_DIVIDE_INTEGER,
    OP_REMAINDER,
    OP_MINUS,
    OP_COMPARE,
    OP_APPLY,
    OP_TO_SCI,
    OP_UNUSED /* an operation L3 does not use */
} OPERATION;

typedef struct {
    const char *name;
    OPERATION operation;
    unsigned operands;
    BINARY binary;
} OPERATION_NAME;

static const OPERATION_NAME operation_names[] = {
    {"add", OP_ADD, 2, nvo_decimal_add},
    {"subtract", OP_SUBTRACT, 2, nvo_decimal_subtract},
    {"multiply", OP_MULTIPLY, 2, nvo_decimal_multiply},
    {"divide", OP_DIVIDE, 2, nvo_decimal_divide},
    {"divideint", OP_DIVIDE_INTEGER, 2, nvo_decimal_divide_integer},
    {"remainder", OP_REMAINDER, 2, nvo_decimal_remainder},
    {"minus", OP_MINUS, 1, 0},
    {"compare", OP_COMPARE, 2, 0},
    {"apply", OP_APPLY, 1, 0},
    {"tosci", OP_TO_SCI, 1, 0},
    {"toeng", OP_UNUSED, 1, 0},
};

/* The context a file sets: the cases are for one, and in any other they
   are skipped. */
typedef struct {
    int rounds_half_even;
    int is_default; /* precision, exponent range, extended and clamp as
                       L3's decimal has them */
} CONTEXT;

/* What became of a case: it ran, or why it did not. */
typedef enum {
    RAN,
    SKIP_ROUNDING,
    SKIP_CONTEXT,
    SKIP_SPECIAL,
    SKIP_UNUSED,
    OUTCOME_COUNT
} OUTCOME;

static const char *const skip_names[OUTCOME_COUNT] = {
    "", "other rounding", "other context", "infinity or NaN or encoding",
    "operation unused"};

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static int same_word(const char *a, const char *b)
{
    while (*a != '\0' && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int starts_word(const char *s, const char *word)
{
    while (*word != '\0' && lower(*s) == *word) {
        s++;
        word++;
    }
    return *word == '\0';
}

/* Whether a token is an infinity, a NaN, a hexadecimal encoding or the
   test set's unknown result: values this decimal does not have. */
static int is_special(const char *s)
{
    if (*s == '+' || *s == '-')
        s++;
    return starts_word(s, "inf") || starts_word(s, "nan") ||
           starts_word(s, "snan") || *s == '#' || *s == '?';
}

/* Split a line into tokens in place, unquoting quoted ones ('' and ""
   inside a quote stand for the quote), and stop at a comment. Returns the
   count. */
static unsigned tokenize(char *line, char *tokens[])
{
    unsigned n = 0;
    char *in = line;

    for (;;) {
        char *out;

        while (*in == ' ' || *in == '\t' || *in == '\n' || *in == '\r')
            in++;
        if (*in == '\0' || (in[0] == '-' && in[1] == '-') || n == TOKENS_MAX)
            return n;
        out = in;
        tokens[n++] = out;
        if (*in == '\'' || *in == '"') {
            char quote = *in++;

            for (;;) {
                if (*in == '\0')
                    break;
                if (*in == quote && in[1] == quote) {
                    *out++ = quote;
                    in += 2;
                } else if (*in == quote) {
                    in++;
                    break;
                } else {
                    *out++ = *in++;
                }
            }
            if (*in != '\0')
                in++;
            *out = '\0';
        } else {
            while (*in != '\0' && *in != ' ' && *in != '\t' && *in != '\n' &&
                   *in != '\r')
                in++;
            if (*in != '\0')
                *in++ = '\0';
        }
    }
}

/* The status the conditions a case lists say its operation ends in:
   an error condition, else NVO_OK. */
static NVO_STATUS expected_status(char *const conditions[], unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        if (same_word(conditions[i], "Division_by_zero"))
            return NVO_DIVIDE_BY_ZERO;
        if (same_word(conditions[i], "Division_undefined") ||
            same_word(conditions[i], "Division_impossible") ||
            same_word(conditions[i], "Invalid_operation"))
            return NVO_INVALID;
        if (same_word(conditions[i], "Overflow"))
            return NVO_OVERFLOW;
        if (same_word(conditions[i], "Conversion_syntax"))
            return NVO_SYNTAX;
    }
    return NVO_OK;
}

/* Whether the conditions a case lists include name. */
static int lists_condition(char *const conditions[], unsigned n,
                           const char *name)
{
    unsigned i;

    for (i = 0; i < n; i++)
        if (same_word(conditions[i], name))
            return 1;
    return 0;
}

static unsigned text_of(const NVO_DECIMAL *x, char *out)
{
    UINT8 bytes[NVO_DECIMAL_TEXT_MAX];
    UINT32 n = nvo_decimal_to_text(x, bytes), i;

    for (i = 0; i < n; i++)
        out[i] = (char)bytes[i];
    out[n] = '\0';
    return n;
}

static NVO_STATUS number_of(NVO_DECIMAL *x, const char *text)
{
    return nvo_decimal_from_text(x, (const UINT8 *)text, strlen(text));
}

static const char *compare_text(int order)
{
    return order < 0 ? "-1" : order > 0 ? "1" : "0";
}

/* Whether nvo_decimal_add_inexact gives the sum a + b that is in sum, and
   the Inexact condition a case lists. Sets got to what it gave if not. */
static int inexact_agrees(const NVO_DECIMAL *a, const NVO_DECIMAL *b,
                          const NVO_DECIMAL *sum, int inexact, char *got)
{
    NVO_DECIMAL again;
    char text[NVO_DECIMAL_TEXT_MAX + 1], expected[NVO_DECIMAL_TEXT_MAX + 1];
    int flagged = 0;

    if (nvo_decimal_add_inexact(&again, a, b, &flagged) != NVO_OK) {
        (void)strcpy(got, "an error");
        return 0;
    }
    (void)text_of(&again, text);
    (void)text_of(sum, expected);
    if (strcmp(text, expected) != 0) {
        (void)strcpy(got, text);
        return 0;
    }
    if (flagged != inexact)
        (void)strcpy(got, flagged ? "Inexact" : "exact");
    return flagged == inexact;
}

/* Run one case, whose operands are the n tokens from first, and whose
   expected result is want, ending in the conditions: inexact is whether
   they list Inexact. Returns 1 if it agrees. */
static int run_case(OPERATION op, BINARY binary, char *const operands[],
                    unsigned n, const char *want, NVO_STATUS expected,
                    int inexact, char *got)
{
    NVO_DECIMAL a, b, r;
    NVO_STATUS status = NVO_OK;

    got[0] = '\0';
    nvo_decimal_zero(&r);
    if (op == OP_APPLY || op == OP_TO_SCI) {
        status = number_of(&r, operands[0]);
    } else {
        NVO_STATUS first = number_of(&a, operands[0]);

        if (first != NVO_OK)
            return 0;
        if (n == 2 && number_of(&b, operands[1]) != NVO_OK)
            return 0;
        if (op == OP_MINUS)
            nvo_decimal_negate(&r, &a);
        else if (op == OP_COMPARE) {
            (void)strcpy(got, compare_text(nvo_decimal_compare(&a, &b)));
            return expected == NVO_OK && strcmp(got, want) == 0;
        } else {
            status = binary(&r, &a, &b);
            if (op == OP_ADD && status == NVO_OK &&
                !inexact_agrees(&a, &b, &r, inexact, got))
                return 0;
        }
    }
    if (status != expected)
        return 0;
    if (status != NVO_OK)
        return 1;
    (void)text_of(&r, got);
    return strcmp(got, want) == 0;
}

/* Apply a directive line to the context. Returns 0 if the directive is
   not one this runner knows. */
static int apply_directive(CONTEXT *context, const char *key,
                           const char *value)
{
    if (starts_word(key, "version"))
        return 1;
    if (starts_word(key, "rounding")) {
        context->rounds_half_even = same_word(value, "half_even");
        return 1;
    }
    if (starts_word(key, "extended") || starts_word(key, "clamp"))
        context->is_default = context->is_default && value[0] == '1';
    else if (starts_word(key, "precision"))
        context->is_default = context->is_default && strcmp(value, "34") == 0;
    else if (starts_word(key, "maxexponent"))
        context->is_default =
            context->is_default && strcmp(value, "6144") == 0;
    else if (starts_word(key, "minexponent"))
        context->is_default =
            context->is_default && strcmp(value, "-6143") == 0;
    else
        return 0;
    return 1;
}

static const OPERATION_NAME *find_operation(const char *name)
{
    unsigned i;

    for (i = 0; i < sizeof operation_names / sizeof operation_names[0]; i++)
        if (same_word(name, operation_names[i].name))
            return &operation_names[i];
    return 0;
}

/* Run the case the n tokens spell: an identifier, an operation, its
   operands, "->", the result and the conditions. *failed is set if it
   disagrees. */
static OUTCOME run_line(const CONTEXT *context, char *const tokens[],
                        unsigned n, const char *file, unsigned number,
                        unsigned *failed)
{
    unsigned arrow = 0, i, special = 0;
    const OPERATION_NAME *name;
    NVO_STATUS expected;
    char got[NVO_DECIMAL_TEXT_MAX + 8];
    const char *want;

    for (i = 0; i < n; i++)
        if (strcmp(tokens[i], "->") == 0) {
            arrow = i;
            break;
        }
    name = n > 1 ? find_operation(tokens[1]) : 0;
    if (arrow < 3 || arrow + 1 >= n || !name ||
        arrow - 2 != name->operands) {
        printf("failed: %s:%u unreadable case\n", file, number);
        *failed = 1;
        return RAN;
    }
    if (name->operation == OP_UNUSED)
        return SKIP_UNUSED;
    if (!context->rounds_half_even)
        return SKIP_ROUNDING;
    if (!context->is_default)
        return SKIP_CONTEXT;
    want = tokens[arrow + 1];
    expected = expected_status(tokens + arrow + 2, n - arrow - 2);
    for (i = 2; i < arrow; i++)
        special |= (unsigned)is_special(tokens[i]);
    if (special || (is_special(want) && expected == NVO_OK))
        return SKIP_SPECIAL;
    if (!run_case(name->operation, name->binary, tokens + 2, arrow - 2, want,
                  expected,
                  lists_condition(tokens + arrow + 2, n - arrow - 2, "Inexact"),
                  got)) {
        printf("failed: %s:%u %s: got %s, want %s\n", file, number, tokens[0],
               got, want);
        *failed = 1;
    }
    return RAN;
}

static void run_file(const char *directory, const TEST_FILE *file)
{
    char path[1024];
    char line[LINE_MAX_BYTES];
    char *tokens[TOKENS_MAX];
    unsigned count[OUTCOME_COUNT] = {0}, number = 0, i;
    CONTEXT context = {1, 1};
    FILE *in;

    (void)snprintf(path, sizeof path, "%s/%s", directory, file->name);
    in = fopen(path, "r");
    if (!in) {
        printf("failed: cannot open %s\n", path);
        failures++;
        return;
    }
    while (fgets(line, sizeof line, in)) {
        size_t length = strlen(line);
        unsigned n, failed = 0;

        number++;
        if (length + 1 == sizeof line && line[length - 1] != '\n') {
            printf("failed: %s:%u line too long\n", file->name, number);
            failures++;
            break;
        }
        n = tokenize(line, tokens);
        if (n == 0)
            continue;
        length = strlen(tokens[0]);
        if (length > 0 && tokens[0][length - 1] == ':') {
            if (!apply_directive(&context, tokens[0], n > 1 ? tokens[1] : "")) {
                printf("failed: %s:%u unknown directive %s\n", file->name,
                       number, tokens[0]);
                failures++;
            }
            continue;
        }
        count[run_line(&context, tokens, n, file->name, number, &failed)]++;
        failures += failed;
    }
    fclose(in);
    printf("%s: %u run, skipped", file->name, count[RAN]);
    for (i = RAN + 1; i < OUTCOME_COUNT; i++)
        printf(" %s %u%s", skip_names[i], count[i],
               i + 1 < OUTCOME_COUNT ? "," : "");
    printf("\n");
    if (count[RAN] != file->cases) {
        printf("failed: %s ran %u cases, expected %u\n", file->name,
               count[RAN], file->cases);
        failures++;
    }
}

/* ------------------------------------------------------------------------
   Laws over generated numbers */

static UINT64 rng_state = RNG_SEED;

static UINT64 next_random(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * RNG_MULTIPLIER;
}

static UINT32 below(UINT32 limit) { return (UINT32)(next_random() % limit); }

/* Make the number with up to digits digits, an exponent within span of
   0 (or, in every EXTREME_ONE_IN, one at an extreme of the format when
   extremes is set), and a random sign. */
static void generate(NVO_DECIMAL *x, UINT32 digits, INT32 span, int extremes)
{
    char text[NUMBER_DIGITS_MAX + 16];
    UINT32 n = 1 + below(digits), at = 0, i;
    INT32 exponent = (INT32)below((UINT32)(2 * span + 1)) - span;

    if (extremes && below(EXTREME_ONE_IN) == 0) {
        INT32 extreme[] = {NVO_DECIMAL_ETINY, NVO_DECIMAL_EXPONENT_MAX,
                           NVO_DECIMAL_ETINY + 5, NVO_DECIMAL_EXPONENT_MAX - 5};

        exponent = extreme[below(4)];
    }
    if (below(2))
        text[at++] = '-';
    text[at++] = (char)('0' + below(10));
    for (i = 1; i < n; i++)
        text[at++] = (char)('0' + below(10));
    (void)snprintf(text + at, sizeof text - at, "E%d", (int)exponent);
    (void)number_of(x, text);
}

static void generate_any(NVO_DECIMAL *x)
{
    generate(x, NUMBER_DIGITS_MAX, NUMBER_EXPONENT_SPAN, 1);
}

static void generate_small(NVO_DECIMAL *x)
{
    generate(x, SMALL_DIGITS_MAX, SMALL_EXPONENT_SPAN, 0);
}

static int same_text(const NVO_DECIMAL *a, const NVO_DECIMAL *b)
{
    char x[NVO_DECIMAL_TEXT_MAX + 1], y[NVO_DECIMAL_TEXT_MAX + 1];

    (void)text_of(a, x);
    (void)text_of(b, y);
    return strcmp(x, y) == 0;
}

static int sign_of(int order) { return order < 0 ? -1 : order > 0; }

static INT64 random_int(UINT32 bits)
{
    UINT64 v = next_random() >> (64 - bits);

    return below(2) ? -(INT64)v : (INT64)v;
}

/* Numbers read as ints: the status, and the int when it is NVO_OK. */
typedef struct {
    const char *text;
    NVO_STATUS status;
    INT64 value;
} INT_CASE;

static const INT_CASE int_cases[] = {
    {"0", NVO_OK, 0},
    {"-0", NVO_OK, 0},
    {"0E+5", NVO_OK, 0},
    {"7", NVO_OK, 7},
    {"1.0", NVO_OK, 1},
    {"100E-2", NVO_OK, 1},
    {"-12.50E+1", NVO_OK, -125},
    {"1E+3", NVO_OK, 1000},
    {"9223372036854775807", NVO_OK, 0x7fffffffffffffffLL},
    {"-9223372036854775808", NVO_OK, -0x7fffffffffffffffLL - 1},
    {"9223372036854775808", NVO_OVERFLOW, 0},
    {"-9223372036854775809", NVO_OVERFLOW, 0},
    {"1E+19", NVO_OVERFLOW, 0},
    {"123456789012345678901234567890", NVO_OVERFLOW, 0},
    {"1E+6000", NVO_OVERFLOW, 0},
    {"0.5", NVO_INVALID, 0},
    {"-0.001", NVO_INVALID, 0},
    {"1.000000000000000000000000000000001", NVO_INVALID, 0},
    {"1.0000000000000000000000000000000001", NVO_OK, 1},
    {"1E-6000", NVO_INVALID, 0},
};

/* Texts that are not numbers; the last is an Arabic-Indic digit one, in
   UTF-8. */
static const char *const not_numbers[] = {
    "",     "+",    "-",     ".",     "e5",   "1e",   "1e+",   "1.2.3",
    "1 2",  " 1",   "1 ",    "0x10",  "1_000", "Inf", "NaN",   "1e5e5",
    "--1",  "+-1",  "1,5",   "\331\241",
};

static void conversions(void)
{
    unsigned i;
    NVO_DECIMAL x;
    INT64 got = 0;
    char text[NVO_DECIMAL_TEXT_MAX + 1];

    for (i = 0; i < sizeof int_cases / sizeof int_cases[0]; i++) {
        NVO_STATUS status;

        check(number_of(&x, int_cases[i].text) == NVO_OK, int_cases[i].text);
        got = -1;
        status = nvo_decimal_to_int(&x, &got);
        check(status == int_cases[i].status, int_cases[i].text);
        check(status != NVO_OK || got == int_cases[i].value,
              int_cases[i].text);
        check(status == NVO_OK || got == -1, "an int conversion that fails "
                                             "leaves its result");
    }
    for (i = 0; i < sizeof not_numbers / sizeof not_numbers[0]; i++) {
        nvo_decimal_from_int(&x, 5);
        check(number_of(&x, not_numbers[i]) == NVO_SYNTAX, not_numbers[i]);
        (void)text_of(&x, text);
        check(strcmp(text, "5") == 0, "a number that does not read leaves "
                                      "its result");
    }
}

static void laws(void)
{
    UINT32 i;
    NVO_DECIMAL a, b, c, x, y, z;
    char text[NVO_DECIMAL_TEXT_MAX + 1];

    for (i = 0; i < LAW_CASES; i++) {
        generate_any(&a);
        generate_any(&b);
        generate_any(&c);

        /* Text: a number prints as a number that reads back as itself. */
        (void)text_of(&a, text);
        check(number_of(&x, text) == NVO_OK && same_text(&a, &x),
              "text round trip");

        /* Addition and multiplication commute, to the digit. */
        check((nvo_decimal_add(&x, &a, &b) == NVO_OK) ==
                  (nvo_decimal_add(&y, &b, &a) == NVO_OK),
              "addition commutes in status");
        if (nvo_decimal_add(&x, &a, &b) == NVO_OK &&
            nvo_decimal_add(&y, &b, &a) == NVO_OK)
            check(same_text(&x, &y), "addition commutes");
        if (nvo_decimal_multiply(&x, &a, &b) == NVO_OK &&
            nvo_decimal_multiply(&y, &b, &a) == NVO_OK)
            check(same_text(&x, &y), "multiplication commutes");

        /* The sum that reports whether it was rounded is the same sum. */
        if (nvo_decimal_add(&x, &a, &b) == NVO_OK) {
            int flagged = 0;

            check(nvo_decimal_add_inexact(&y, &a, &b, &flagged) == NVO_OK &&
                      same_text(&x, &y),
                  "addition that flags rounding gives the same sum");
        }

        /* Subtraction is addition of the negation, and its sign is the
           order. */
        nvo_decimal_negate(&z, &b);
        if (nvo_decimal_subtract(&x, &a, &b) == NVO_OK &&
            nvo_decimal_add(&y, &a, &z) == NVO_OK) {
            check(nvo_decimal_compare(&x, &y) == 0, "a - b is a + (-b)");
            nvo_decimal_zero(&z);
            check(sign_of(nvo_decimal_compare(&x, &z)) ==
                      sign_of(nvo_decimal_compare(&a, &b)),
                  "the sign of a - b is the order of a and b");
        }

        /* Negation twice is the identity, but for the sign of zero. */
        nvo_decimal_negate(&x, &a);
        nvo_decimal_negate(&y, &x);
        check(nvo_decimal_is_zero(&a) ? nvo_decimal_is_zero(&y)
                                      : same_text(&a, &y),
              "negation is its own inverse");

        /* The order is antisymmetric, reflexive and transitive. */
        check(sign_of(nvo_decimal_compare(&a, &b)) ==
                  -sign_of(nvo_decimal_compare(&b, &a)),
              "order is antisymmetric");
        check(nvo_decimal_compare(&a, &a) == 0, "order is reflexive");
        if (nvo_decimal_compare(&a, &b) <= 0 &&
            nvo_decimal_compare(&b, &c) <= 0)
            check(nvo_decimal_compare(&a, &c) <= 0, "order is transitive");
    }
}

static void integer_laws(void)
{
    UINT32 i;
    NVO_DECIMAL a, b, x, y, q, r, p;
    INT64 got;
    static const INT64 edges[] = {0,
                                  1,
                                  -1,
                                  9,
                                  10,
                                  -10,
                                  999999999,
                                  1000000000,
                                  -1000000000,
                                  0x7fffffffffffffffLL,
                                  -0x7fffffffffffffffLL - 1};

    /* An int converts to a decimal and back, and the order of decimals is
       the order of ints. */
    for (i = 0; i < sizeof edges / sizeof edges[0] * 4; i++) {
        INT64 v = edges[i % (sizeof edges / sizeof edges[0])];
        INT64 w = edges[(i / 4) % (sizeof edges / sizeof edges[0])];

        nvo_decimal_from_int(&x, v);
        nvo_decimal_from_int(&y, w);
        check(nvo_decimal_to_int(&x, &got) == NVO_OK && got == v,
              "int edge round trip");
        check(nvo_decimal_compare(&x, &y) == (v < w ? -1 : v > w),
              "int edge order");
    }
    for (i = 0; i < LAW_CASES; i++) {
        INT64 v = random_int(1 + below(63)), w = random_int(1 + below(63));
        INT64 s = random_int(31), t = random_int(31);

        nvo_decimal_from_int(&x, v);
        nvo_decimal_from_int(&y, w);
        check(nvo_decimal_to_int(&x, &got) == NVO_OK && got == v,
              "int round trip");
        check(nvo_decimal_compare(&x, &y) == (v < w ? -1 : v > w),
              "int order");

        /* Arithmetic on small ints agrees with the machine's. */
        nvo_decimal_from_int(&x, s);
        nvo_decimal_from_int(&y, t);
        nvo_decimal_from_int(&p, s + t);
        check(nvo_decimal_add(&r, &x, &y) == NVO_OK && same_text(&r, &p),
              "int sum");
        nvo_decimal_from_int(&p, s - t);
        check(nvo_decimal_subtract(&r, &x, &y) == NVO_OK && same_text(&r, &p),
              "int difference");
        nvo_decimal_from_int(&p, s * t);
        check(nvo_decimal_multiply(&r, &x, &y) == NVO_OK && same_text(&r, &p),
              "int product");
        if (t != 0) {
            check(nvo_decimal_divide_integer(&q, &x, &y) == NVO_OK &&
                      nvo_decimal_to_int(&q, &got) == NVO_OK && got == s / t,
                  "int integer quotient truncates");
            check(nvo_decimal_remainder(&r, &x, &y) == NVO_OK &&
                      nvo_decimal_to_int(&r, &got) == NVO_OK && got == s % t,
                  "int remainder takes the dividend's sign");
        }
    }

    /* The quotient and remainder of numbers with digits after the point
       give the dividend back, and a product divided by a factor gives the
       other factor, in the exponent it has. */
    for (i = 0; i < LAW_CASES; i++) {
        generate_small(&a);
        generate_small(&b);
        if (nvo_decimal_is_zero(&b))
            continue;
        if (nvo_decimal_divide_integer(&q, &a, &b) == NVO_OK &&
            nvo_decimal_remainder(&r, &a, &b) == NVO_OK) {
            (void)nvo_decimal_multiply(&p, &q, &b);
            (void)nvo_decimal_add(&x, &p, &r);
            check(nvo_decimal_compare(&x, &a) == 0,
                  "a is (a // b) * b + a % b");
        }
        if (nvo_decimal_multiply(&p, &a, &b) == NVO_OK &&
            nvo_decimal_divide(&x, &p, &b) == NVO_OK)
            check(nvo_decimal_compare(&x, &a) == 0 &&
                      (nvo_decimal_is_zero(&a) || same_text(&x, &a)),
                  "(a * b) / b is a, exactly");
    }
}

int main(int argc, char **argv)
{
    unsigned i;

    if (argc != 2) {
        printf("usage: check-decimal DIRECTORY\n");
        return 2;
    }
    for (i = 0; i < sizeof files / sizeof files[0]; i++)
        run_file(argv[1], &files[i]);
    conversions();
    laws();
    integer_laws();
    printf("%u failures\n", failures);
    return failures != 0;
}
