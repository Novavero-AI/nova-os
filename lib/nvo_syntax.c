/* nvo's source text (nvo_syntax.h): docs/spec/shell.md L1 and L2.

   The parser is recursive descent over the bytes, with no separate lexer:
   the two modes of L1 are two sets of functions, and the grammar decides
   which reads a token. A stage's first token picks command mode or
   expression mode (parse_stage); a command's words are runs of
   characters that are not metacharacters (parse_word), and an expression's
   operators are tokens of their own (parse_binary and the functions
   around it).

   Every function that makes a node returns its index, or 0 after an
   error, which it has recorded: the first error is the one reported. The
   one place a function steps back is a look ahead of a few tokens, which
   restores the position it began at. */

#include "nvo_syntax.h"

/* The meaning of a lone byte, for the parser: one place gives the
   metacharacters of L1 rule 4. */
static int is_digit(UINT32 c) { return c >= '0' && c <= '9'; }

static int is_hex(UINT32 c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int is_letter(UINT32 c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_name_start(UINT32 c) { return is_letter(c) || c == '_'; }

static int is_name_char(UINT32 c) { return is_name_start(c) || is_digit(c); }

/* A character that continues a word operator such as `and`, so that
   `android` is not one. */
static int is_word_char(UINT32 c) { return is_name_char(c) || c == '-'; }

/* The two modes of L1 rule 4, for the functions that read a run of
   characters: a comma is a character of a command's word and the end of an
   expression's path, so that a list of paths needs no blank after each. */
typedef enum { READ_COMMAND, READ_EXPRESSION } READ_MODE;

static int is_meta(UINT32 c, READ_MODE mode)
{
    switch (c) {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case '|':
    case ';':
    case '(':
    case ')':
    case '[':
    case ']':
    case '{':
    case '}':
    case '"':
    case '\'':
    case '$':
        return 1;
    case ',':
        return mode == READ_EXPRESSION;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------------
   UTF-8 */

/* The bytes of the sequence that begins at source[i], or 0 if it is not
   UTF-8: RFC 3629's table of well-formed sequences. */
static UINT32 utf8_length(const UINT8 *source, UINT64 length, UINT64 i)
{
    UINT32 lead = source[i], low = 0x80, high = 0xbf, size, k;

    if (lead < 0x80)
        return 1;
    if (lead >= 0xc2 && lead <= 0xdf) {
        size = 2;
    } else if (lead >= 0xe0 && lead <= 0xef) {
        size = 3;
        if (lead == 0xe0)
            low = 0xa0;
        if (lead == 0xed)
            high = 0x9f;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
        size = 4;
        if (lead == 0xf0)
            low = 0x90;
        if (lead == 0xf4)
            high = 0x8f;
    } else {
        return 0;
    }
    if (length - i < size)
        return 0;
    for (k = 1; k < size; k++) {
        UINT32 c = source[i + k];

        if (c < low || c > high)
            return 0;
        low = 0x80;
        high = 0xbf;
    }
    return size;
}

int nvo_utf8_check(const UINT8 *source, UINT64 length, UINT64 *offset)
{
    UINT64 i = 0;

    while (i < length) {
        UINT32 size = utf8_length(source, length, i);

        if (size == 0) {
            *offset = i;
            return 0;
        }
        i += size;
    }
    return 1;
}

/* ------------------------------------------------------------------------
   Number literals */

typedef struct {
    const char *name;
    UINT32 length;
    NVO_NUMBER_KIND kind;
    INT64 factor;
} UNIT;

/* The units of L1 rule 5, in bytes or nanoseconds. */
static const UNIT units[] = {
    {"B", 1, NVO_NUMBER_SIZE, 1LL},
    {"kB", 2, NVO_NUMBER_SIZE, 1000LL},
    {"MB", 2, NVO_NUMBER_SIZE, 1000000LL},
    {"GB", 2, NVO_NUMBER_SIZE, 1000000000LL},
    {"TB", 2, NVO_NUMBER_SIZE, 1000000000000LL},
    {"KiB", 3, NVO_NUMBER_SIZE, 1024LL},
    {"MiB", 3, NVO_NUMBER_SIZE, 1048576LL},
    {"GiB", 3, NVO_NUMBER_SIZE, 1073741824LL},
    {"TiB", 3, NVO_NUMBER_SIZE, 1099511627776LL},
    {"ns", 2, NVO_NUMBER_DURATION, 1LL},
    {"us", 2, NVO_NUMBER_DURATION, 1000LL},
    {"ms", 2, NVO_NUMBER_DURATION, 1000000LL},
    {"s", 1, NVO_NUMBER_DURATION, 1000000000LL},
    {"min", 3, NVO_NUMBER_DURATION, 60000000000LL},
    {"h", 1, NVO_NUMBER_DURATION, 3600000000000LL},
    {"d", 1, NVO_NUMBER_DURATION, 86400000000000LL},
};

#define UNIT_COUNT (sizeof units / sizeof units[0])

/* The greatest decimal digits a size or duration's whole part can have
   (INT64's largest has 19), and the most fraction digits one can have
   once its trailing zeros are dropped: the factors are products of two
   and five, and a fraction that ends in a digit that is not 0 comes to
   whole units only with as many twos or fives in the factor as it has
   digits, and the factor with the most has 2^40. */
#define WHOLE_DIGITS_MAX 19
#define FRACTION_DIGITS_MAX 40

/* A product of a literal and a factor: 59 digits and 14. */
#define PRODUCT_DIGITS 80

/* The end of a run of digits and underscores from i, which is a digit. */
static UINT32 scan_dec(const UINT8 *s, UINT32 n, UINT32 i)
{
    while (i < n && (is_digit(s[i]) || s[i] == '_'))
        i++;
    return i;
}

/* The unit that begins at s[i], the longest that does, or 0. */
static const UNIT *unit_at(const UINT8 *s, UINT32 n, UINT32 i)
{
    const UNIT *best = 0;
    UINT32 u, k;

    for (u = 0; u < UNIT_COUNT; u++) {
        if (i + units[u].length > n)
            continue;
        for (k = 0; k < units[u].length; k++)
            if (s[i + k] != (UINT8)units[u].name[k])
                break;
        if (k == units[u].length && (!best || units[u].length > best->length))
            best = &units[u];
    }
    return best;
}

/* The radix of an integer literal's prefix at s[i], 0 if it has none. */
static UINT32 radix_at(const UINT8 *s, UINT32 n, UINT32 i)
{
    if (i + 2 >= n || s[i] != '0')
        return 0;
    if (s[i + 1] == 'x' && is_hex(s[i + 2]))
        return 16;
    if (s[i + 1] == 'o' && s[i + 2] >= '0' && s[i + 2] <= '7')
        return 8;
    if (s[i + 1] == 'b' && (s[i + 2] == '0' || s[i + 2] == '1'))
        return 2;
    return 0;
}

static UINT32 digit_value(UINT32 c)
{
    if (is_digit(c))
        return c - '0';
    return (c | 0x20u) - 'a' + 10;
}

static int is_radix_digit(UINT32 c, UINT32 radix)
{
    return c == '_' || (is_hex(c) && digit_value(c) < radix);
}

/* The end of the number literal that begins at s[i], a digit, and what
   it is. *unit is the unit it ends in, or 0. */
static UINT32 scan_number(const UINT8 *s, UINT32 n, UINT32 i,
                          NVO_NUMBER_KIND *kind, const UNIT **unit)
{
    UINT32 radix = radix_at(s, n, i), end;

    *unit = 0;
    *kind = NVO_NUMBER_INT;
    if (radix != 0) {
        end = i + 2;
        while (end < n && is_radix_digit(s[end], radix))
            end++;
        return end;
    }
    end = scan_dec(s, n, i);
    if (end + 1 < n && s[end] == '.' && is_digit(s[end + 1])) {
        end = scan_dec(s, n, end + 1);
        *kind = NVO_NUMBER_DECIMAL;
    }
    if (end < n && (s[end] == 'e' || s[end] == 'E')) {
        UINT32 at = end + 1;

        if (at < n && (s[at] == '+' || s[at] == '-'))
            at++;
        if (at < n && is_digit(s[at])) {
            *kind = NVO_NUMBER_DECIMAL;
            return scan_dec(s, n, at);
        }
    }
    *unit = unit_at(s, n, end);
    if (*unit) {
        *kind = (*unit)->kind;
        end += (*unit)->length;
    }
    return end;
}

/* Set *value to the integer literal text holds, which begins at 0, in
   radix. Returns NVO_OVERFLOW if it is over INT64's largest. */
static NVO_STATUS integer_value(const UINT8 *text, UINT32 length, UINT32 radix,
                                INT64 *value)
{
    UINT64 acc = 0, limit = 0x7fffffffffffffffULL;
    UINT32 i = radix == 10 ? 0 : 2;

    for (; i < length; i++) {
        UINT32 d;

        if (text[i] == '_')
            continue;
        d = digit_value(text[i]);
        if (acc > (limit - d) / radix)
            return NVO_OVERFLOW;
        acc = acc * radix + d;
    }
    *value = (INT64)acc;
    return NVO_OK;
}

/* Set *value to whole.fraction x factor, which is a whole number if the
   fraction's digits, with trailing zeros dropped, are few enough, and
   the product of the digits and the factor, exactly, has zeros below the
   point. The digits are in text, from whole to whole + whole_length and
   from fraction to fraction + fraction_length, underscores removed. */
static NVO_STATUS scaled_value(const UINT8 *whole, UINT32 whole_length,
                               const UINT8 *fraction, UINT32 fraction_length,
                               INT64 factor, INT64 *value)
{
    UINT8 digits[WHOLE_DIGITS_MAX + FRACTION_DIGITS_MAX];
    UINT32 columns[PRODUCT_DIGITS];
    UINT8 factor_digits[WHOLE_DIGITS_MAX];
    UINT32 count = 0, factor_count = 0, i, j, carry = 0, below;
    UINT64 acc = 0, rest = (UINT64)factor;

    while (whole_length > 0 && *whole == '0') {
        whole++;
        whole_length--;
    }
    while (fraction_length > 0 && fraction[fraction_length - 1] == '0')
        fraction_length--;
    if (whole_length > WHOLE_DIGITS_MAX)
        return NVO_OVERFLOW;
    if (fraction_length > FRACTION_DIGITS_MAX)
        return NVO_INVALID;
    /* The literal as one integer, least significant digit first. */
    for (i = fraction_length; i > 0; i--)
        digits[count++] = (UINT8)(fraction[i - 1] - '0');
    for (i = whole_length; i > 0; i--)
        digits[count++] = (UINT8)(whole[i - 1] - '0');
    while (rest > 0) {
        factor_digits[factor_count++] = (UINT8)(rest % 10);
        rest /= 10;
    }
    for (i = 0; i < PRODUCT_DIGITS; i++)
        columns[i] = 0;
    for (i = 0; i < count; i++)
        for (j = 0; j < factor_count; j++)
            columns[i + j] += (UINT32)digits[i] * factor_digits[j];
    for (i = 0; i < PRODUCT_DIGITS; i++) {
        UINT32 sum = columns[i] + carry;

        columns[i] = sum % 10;
        carry = sum / 10;
    }
    for (below = 0; below < fraction_length; below++)
        if (columns[below] != 0)
            return NVO_INVALID;
    for (i = PRODUCT_DIGITS; i > fraction_length; i--) {
        if (acc > (0x7fffffffffffffffULL - columns[i - 1]) / 10)
            return NVO_OVERFLOW;
        acc = acc * 10 + columns[i - 1];
    }
    *value = (INT64)acc;
    return NVO_OK;
}

NVO_STATUS nvo_number_parse(const UINT8 *text, UINT64 length,
                            NVO_NUMBER *number)
{
    NVO_NUMBER_KIND kind;
    const UNIT *unit;
    UINT32 n = (UINT32)length, end, radix, number_end, i, clean = 0;
    UINT8 buffer[NVO_NUMBER_LITERAL_MAX];
    UINT32 point = 0, seen_point = 0;

    if (length == 0 || length > NVO_SOURCE_MAX || !is_digit(text[0]))
        return NVO_SYNTAX;
    end = scan_number(text, n, 0, &kind, &unit);
    if (end != n)
        return NVO_SYNTAX;
    number->kind = kind;
    nvo_decimal_zero(&number->decimal);
    number->integer = 0;
    radix = radix_at(text, n, 0);
    if (kind == NVO_NUMBER_INT)
        return integer_value(text, n, radix == 0 ? 10 : radix,
                             &number->integer);
    number_end = n - (unit ? unit->length : 0);
    if (number_end > NVO_NUMBER_LITERAL_MAX)
        return NVO_INVALID;
    for (i = 0; i < number_end; i++) {
        if (text[i] == '_')
            continue;
        if (text[i] == '.') {
            seen_point = 1;
            point = clean;
        }
        buffer[clean++] = text[i];
    }
    if (kind == NVO_NUMBER_DECIMAL)
        return nvo_decimal_from_text(&number->decimal, buffer, clean);
    /* A size or duration: digits, a point and digits. */
    if (seen_point)
        return scaled_value(buffer, point, buffer + point + 1,
                            clean - point - 1, unit->factor, &number->integer);
    return scaled_value(buffer, clean, buffer, 0, unit->factor,
                        &number->integer);
}

/* ------------------------------------------------------------------------
   Parser state and nodes */

typedef struct {
    const UINT8 *s;    /* the source */
    UINT32 n;          /* its length */
    UINT32 pos;        /* where the next token begins, or blanks before it */
    UINT32 end;        /* the end of the last token taken */
    NVO_TREE *tree;    /* the nodes */
    UINT32 depth;      /* the nesting the parser is at */
    int blank_lines;   /* whether a newline is a blank: inside ( [ and a
                          record's { */
    NVO_STATUS status; /* NVO_OK until the first error */
    NVO_ERROR error;   /* what the error was, for NVO_SYNTAX */
    UINT32 error_at;   /* and where */
    /* Where each level of nesting opened: NOT_A_BRACKET for a prefix
       operator, which has no opening to report. */
    UINT32 opened[NVO_DEPTH_MAX];
} PARSER;

/* What a level of nesting that opens no bracket records as where it opened. */
#define NOT_A_BRACKET 0xffffffffu

/* Set *open to where the innermost bracket the parser is inside opened, and
   return 1, or return 0 if it is inside none. */
static int innermost_bracket(const PARSER *p, UINT32 *open)
{
    UINT32 level;

    for (level = p->depth; level > 0; level--)
        if (p->opened[level - 1] != NOT_A_BRACKET) {
            *open = p->opened[level - 1];
            return 1;
        }
    return 0;
}

static UINT32 fail(PARSER *p, NVO_ERROR error, UINT32 at)
{
    UINT32 open;

    if (p->status != NVO_OK)
        return 0;
    /* A source that ends where the grammar needs a token inside a bracket
       is a bracket never closed: it is reported at the opening of the
       innermost one, not at the end, which points at nothing. */
    if (error == NVO_ERROR_UNEXPECTED && at >= p->n &&
        innermost_bracket(p, &open)) {
        error = NVO_ERROR_UNTERMINATED;
        at = open;
    }
    p->status = NVO_SYNTAX;
    p->error = error;
    p->error_at = at;
    return 0;
}

static UINT32 make(PARSER *p, NVO_NODE_KIND kind, UINT32 offset, UINT32 length)
{
    NVO_TREE *tree = p->tree;
    NVO_NODE *node;

    if (p->status != NVO_OK)
        return 0;
    if (tree->count >= tree->capacity) {
        p->status = NVO_MEMORY;
        return 0;
    }
    node = &tree->nodes[tree->count];
    node->kind = (UINT16)kind;
    node->flags = 0;
    node->first = 0;
    node->last = 0;
    node->next = 0;
    node->parent = 0;
    node->offset = offset;
    node->length = length;
    return tree->count++;
}

/* Make a node over [offset, p->end). */
static UINT32 over(PARSER *p, NVO_NODE_KIND kind, UINT32 offset)
{
    return make(p, kind, offset, p->end - offset);
}

static NVO_NODE *at(PARSER *p, UINT32 index) { return &p->tree->nodes[index]; }

/* Make child the last child of parent. */
static void adopt(PARSER *p, UINT32 parent, UINT32 child)
{
    NVO_NODE *nodes = p->tree->nodes;

    nodes[child].parent = parent;
    if (nodes[parent].last != 0)
        nodes[nodes[parent].last].next = child;
    else
        nodes[parent].first = child;
    nodes[parent].last = child;
}

/* Set the node's span to run to the end of the last token taken. */
static UINT32 seal(PARSER *p, UINT32 node)
{
    NVO_NODE *n = at(p, node);

    /* A node that took no token, an empty script, begins after the blanks
       that were skipped and so is past the last token's end: it is empty. */
    n->length = p->end > n->offset ? p->end - n->offset : 0;
    return node;
}

/* Take length bytes as a token. */
static void take(PARSER *p, UINT32 length)
{
    p->pos += length;
    p->end = p->pos;
}

static UINT32 newline_length(const PARSER *p, UINT32 i)
{
    if (i < p->n && p->s[i] == '\n')
        return 1;
    if (i + 1 < p->n && p->s[i] == '\r' && p->s[i + 1] == '\n')
        return 2;
    return 0;
}

/* Skip blanks, comments and, if newlines is set, newlines: whatever lies
   between tokens. A comment ends at its line's end, which it leaves. */
static void skip(PARSER *p, int newlines)
{
    for (;;) {
        UINT32 c, k;

        if (p->pos >= p->n)
            return;
        c = p->s[p->pos];
        if (c == ' ' || c == '\t') {
            p->pos++;
        } else if (c == '#') {
            while (p->pos < p->n && newline_length(p, p->pos) == 0)
                p->pos++;
        } else if (newlines && (k = newline_length(p, p->pos)) != 0) {
            p->pos += k;
        } else {
            return;
        }
    }
}

/* Skip what lies between tokens where this context's newlines are. */
static void skip_here(PARSER *p) { skip(p, p->blank_lines); }

static UINT32 peek(const PARSER *p)
{
    return p->pos < p->n ? p->s[p->pos] : 0x100u;
}

static UINT32 peek_at(const PARSER *p, UINT32 i)
{
    return i < p->n ? p->s[i] : 0x100u;
}

/* Fail where a token is that the grammar has no place for, or the end of
   the source is. */
static UINT32 unexpected(PARSER *p)
{
    return fail(p, NVO_ERROR_UNEXPECTED, p->pos < p->n ? p->pos : p->n);
}

/* What a level of nesting is: a bracket, which the source must close, or a
   prefix operator, which closes nothing. */
typedef enum { LEVEL_BRACKET, LEVEL_PREFIX } LEVEL;

/* Count a level of nesting in, which began at at_offset; the caller counts
   it out with p->depth--. */
static int deepen(PARSER *p, UINT32 at_offset, LEVEL level)
{
    if (p->depth >= NVO_DEPTH_MAX) {
        (void)fail(p, NVO_ERROR_DEPTH, at_offset);
        return 0;
    }
    p->opened[p->depth++] = level == LEVEL_BRACKET ? at_offset : NOT_A_BRACKET;
    return 1;
}

static UINT32 run_end(const PARSER *p, UINT32 i, READ_MODE mode)
{
    while (i < p->n && !is_meta(p->s[i], mode))
        i++;
    return i;
}

static UINT32 name_end(const PARSER *p, UINT32 i)
{
    if (!is_name_start(peek_at(p, i)))
        return i;
    while (is_name_char(peek_at(p, i)))
        i++;
    return i;
}

static UINT32 defname_end(const PARSER *p, UINT32 i)
{
    if (!is_name_start(peek_at(p, i)))
        return i;
    while (is_word_char(peek_at(p, i)))
        i++;
    return i;
}

/* Whether the bytes [start, end) are the ASCII text word. */
static int is_text(const PARSER *p, UINT32 start, UINT32 end, const char *word)
{
    UINT32 i = 0;

    while (word[i] != '\0' && start + i < end &&
           p->s[start + i] == (UINT8)word[i])
        i++;
    return word[i] == '\0' && start + i == end;
}

/* Whether the source ends at or in the bytes from start on, and they are the
   first bytes of word: the word is cut short by the end of the source. */
static int ends_in_start_of(const PARSER *p, UINT32 start, const char *word)
{
    UINT32 i;

    for (i = 0; start + i < p->n; i++)
        if (word[i] == '\0' || p->s[start + i] != (UINT8)word[i])
            return 0;
    return 1;
}

/* Whether a word operator is at the position: the word, and no character
   that would continue it. */
static int word_at(const PARSER *p, const char *word)
{
    UINT32 length = 0;

    while (word[length] != '\0')
        length++;
    if (p->pos + length > p->n || !is_text(p, p->pos, p->pos + length, word))
        return 0;
    return !is_word_char(peek_at(p, p->pos + length));
}

/* Whether the keyword of a statement is at the position: the whole run of
   characters to the next metacharacter is the keyword, as parse_stage and
   parse_statement read a stage's first token, so that `else!` is the name
   of a command and not `else` with `!` after it. */
static int keyword_at(const PARSER *p, const char *word)
{
    return is_text(p, p->pos, run_end(p, p->pos, READ_COMMAND), word);
}

/* ------------------------------------------------------------------------
   Keywords */

static const char *const keywords[] = {
    "let",   "var",   "set",   "def",   "export", "use",   "if",   "for",
    "while", "try",   "break", "continue", "return", "needs", "else", "catch",
    "in",    "as",    "and",   "or",    "not",    "true",  "false", "nothing",
};

static int is_keyword(const PARSER *p, UINT32 start, UINT32 end)
{
    UINT32 k;

    for (k = 0; k < sizeof keywords / sizeof keywords[0]; k++)
        if (is_text(p, start, end, keywords[k]))
            return 1;
    return 0;
}

/* Whether c begins an operator of L2 where an operand is expected. */
static int begins_operator(UINT32 c)
{
    return c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
           c == '<' || c == '>' || c == '=' || c == '!';
}

/* Fail where an operand is expected and none begins. A word that is not a
   keyword and does not begin an operator is a bare word (L1 rule 4); a
   keyword, an operator or a bracket is a token with no place here. */
static UINT32 no_operand(PARSER *p)
{
    UINT32 start = p->pos, end;

    if (start >= p->n)
        return fail(p, NVO_ERROR_UNEXPECTED, p->n);
    end = run_end(p, start, READ_EXPRESSION);
    if (end > start && !begins_operator(p->s[start]) &&
        !is_keyword(p, start, end))
        return fail(p, NVO_ERROR_BARE_WORD, start);
    return fail(p, NVO_ERROR_UNEXPECTED, start);
}

/* Whether the run [start, end) names a path: a path literal or a glob. */
static int is_path_run(const PARSER *p, UINT32 start, UINT32 end)
{
    UINT32 length = end - start;
    const UINT8 *r = p->s + start;

    if (length == 0)
        return 0;
    if (length == 1)
        return r[0] == '/' || r[0] == '.' || r[0] == '~';
    if (length == 2 && r[0] == '.' && r[1] == '.')
        return 1;
    return r[0] == '/' || (r[0] == '.' && r[1] == '/') ||
           (r[0] == '~' && r[1] == '/') ||
           (length >= 3 && r[0] == '.' && r[1] == '.' && r[2] == '/');
}

static NVO_NODE_KIND path_kind(const PARSER *p, UINT32 start, UINT32 end)
{
    UINT32 i;

    for (i = start; i < end; i++)
        if (p->s[i] == '*' || p->s[i] == '?')
            return NVO_NODE_GLOB;
    return NVO_NODE_PATH;
}

/* ------------------------------------------------------------------------
   Forward declarations: the grammar is mutually recursive. */

static UINT32 parse_expr(PARSER *p);
static UINT32 parse_pipeline(PARSER *p);
static UINT32 parse_script(PARSER *p);
static UINT32 parse_block(PARSER *p);
static UINT32 parse_type(PARSER *p);
static UINT32 parse_part(PARSER *p, READ_MODE mode);

/* ------------------------------------------------------------------------
   Strings, interpolation and members */

/* Close a bracket: the position is at ch, or the source ended or holds
   something else. open is where the bracket opened. */
static int expect(PARSER *p, UINT32 ch, UINT32 open)
{
    skip_here(p);
    if (peek(p) == ch) {
        take(p, 1);
        return 1;
    }
    if (p->pos >= p->n)
        (void)fail(p, NVO_ERROR_UNTERMINATED, open);
    else
        (void)unexpected(p);
    return 0;
}

/* The value of a `\u{ }` escape's digits, or 0x110000 if too large. */
#define SCALAR_LIMIT 0x110000u
#define SURROGATE_FIRST 0xd800u
#define SURROGATE_LAST 0xdfffu

/* Check the escape at the backslash at p->pos, and take it. */
static int take_escape(PARSER *p, UINT32 string_start)
{
    UINT32 c = peek_at(p, p->pos + 1);

    if (c == 0x100u) {
        (void)fail(p, NVO_ERROR_UNTERMINATED, string_start);
        return 0;
    }
    if (c == '\\' || c == '"' || c == '$' || c == 'n' || c == 't' ||
        c == 'r') {
        take(p, 2);
        return 1;
    }
    if (c == 'u' && peek_at(p, p->pos + 2) == '{') {
        UINT32 i = p->pos + 3, digits = 0, value = 0;

        while (is_hex(peek_at(p, i))) {
            if (value < SCALAR_LIMIT)
                value = value * 16 + digit_value(peek_at(p, i));
            digits++;
            i++;
        }
        if (peek_at(p, i) == 0x100u) {
            (void)fail(p, NVO_ERROR_UNTERMINATED, string_start);
            return 0;
        }
        if (digits > 0 && peek_at(p, i) == '}' && value < SCALAR_LIMIT &&
            (value < SURROGATE_FIRST || value > SURROGATE_LAST)) {
            take(p, i + 1 - p->pos);
            return 1;
        }
    }
    (void)fail(p, NVO_ERROR_ESCAPE, p->pos);
    return 0;
}

static UINT32 parse_single_quoted(PARSER *p)
{
    UINT32 start = p->pos, i = p->pos + 1;

    for (;;) {
        if (i >= p->n)
            return fail(p, NVO_ERROR_UNTERMINATED, start);
        if (p->s[i] == '\'') {
            if (peek_at(p, i + 1) == '\'') {
                i += 2;
                continue;
            }
            break;
        }
        i++;
    }
    take(p, i + 1 - start);
    return over(p, NVO_NODE_STRING_SQ, start);
}

/* Make a text node of the string's characters from from to p->pos, if
   there are any, a child of string. */
static int flush_text(PARSER *p, UINT32 string, UINT32 from)
{
    UINT32 text;

    if (p->pos == from)
        return 1;
    text = make(p, NVO_NODE_TEXT, from, p->pos - from);
    if (!text)
        return 0;
    adopt(p, string, text);
    return 1;
}

/* Where a variable stands, which sets what may follow its members: a
   double-quoted string takes members that are names or digits and a `?`; a
   word takes those and members that are strings; a spread takes members of
   all three and no `?`. */
typedef enum {
    VARIABLE_IN_STRING,
    VARIABLE_IN_WORD,
    VARIABLE_IN_SPREAD
} VARIABLE_IN;

static UINT32 parse_interpolation(PARSER *p, VARIABLE_IN where);

static UINT32 parse_double_quoted(PARSER *p)
{
    UINT32 start = p->pos, string, from;

    string = make(p, NVO_NODE_STRING_DQ, start, 0);
    if (!string)
        return 0;
    p->pos++;
    from = p->pos;
    for (;;) {
        UINT32 c = peek(p);

        if (c == 0x100u)
            return fail(p, NVO_ERROR_UNTERMINATED, start);
        if (c == '"') {
            if (!flush_text(p, string, from))
                return 0;
            take(p, 1);
            return seal(p, string);
        }
        if (c == '\\') {
            if (!take_escape(p, start))
                return 0;
        } else if (c == '$') {
            UINT32 part;

            if (!flush_text(p, string, from))
                return 0;
            part = parse_interpolation(p, VARIABLE_IN_STRING);
            if (!part)
                return 0;
            adopt(p, string, part);
            from = p->pos;
        } else {
            p->pos++;
        }
    }
}

/* Parse the member after a dot that p->pos is past: a name, digits or,
   if strings is set, a string. Returns the member node. */
static UINT32 parse_member(PARSER *p, int strings)
{
    UINT32 start = p->pos, c = peek(p), member, end;

    if (is_name_start(c)) {
        end = name_end(p, start);
        take(p, end - start);
        return over(p, NVO_NODE_MEMBER, start);
    }
    if (is_digit(c)) {
        end = start;
        while (is_digit(peek_at(p, end)))
            end++;
        take(p, end - start);
        member = over(p, NVO_NODE_MEMBER, start);
        if (member)
            at(p, member)->flags = NVO_MEMBER_INT;
        return member;
    }
    if (strings && (c == '"' || c == '\'')) {
        UINT32 text, node;

        member = make(p, NVO_NODE_MEMBER, start, 0);
        if (!member)
            return 0;
        node = c == '"' ? parse_double_quoted(p) : parse_single_quoted(p);
        text = node;
        if (!text)
            return 0;
        adopt(p, member, text);
        at(p, member)->flags = NVO_MEMBER_STRING;
        return seal(p, member);
    }
    return unexpected(p);
}

/* Whether a member follows at the dot at p->pos, that is, whether the dot
   is a member's and not a character of text. */
static int member_follows(const PARSER *p, int strings)
{
    UINT32 c = peek_at(p, p->pos + 1);

    return peek(p) == '.' && (is_name_start(c) || is_digit(c) ||
                              (strings && (c == '"' || c == '\'')));
}

/* Take the dot at p->pos and the member after it, into parent. */
static int parse_dotted_member(PARSER *p, UINT32 parent, int strings)
{
    UINT32 member;

    take(p, 1);
    member = parse_member(p, strings);
    if (!member)
        return 0;
    adopt(p, parent, member);
    return 1;
}

/* Parse the members of an unbraced variable from the dots at p->pos on. */
static int parse_members(PARSER *p, UINT32 parent, int strings)
{
    while (member_follows(p, strings))
        if (!parse_dotted_member(p, parent, strings))
            return 0;
    return 1;
}

/* A variable, `$name` and its members, or `${name}` and its. A `$` that no
   name follows is an error at the `$`. */
static UINT32 parse_variable(PARSER *p, VARIABLE_IN where)
{
    UINT32 start = p->pos, name_from, name_to, variable;
    int braced = peek_at(p, p->pos + 1) == '{';

    name_from = start + (braced ? 2 : 1);
    name_to = name_end(p, name_from);
    if (name_to == name_from) {
        /* A `${` that the source ends after is a bracket never closed. */
        if (braced && name_from >= p->n)
            return fail(p, NVO_ERROR_UNTERMINATED, start);
        return fail(p, NVO_ERROR_UNEXPECTED, start);
    }
    variable = make(p, NVO_NODE_VARIABLE, name_from, name_to - name_from);
    if (!variable)
        return 0;
    take(p, name_to - start);
    if (braced) {
        /* A member of a braced variable may be a string, which may hold
           a braced variable: the brace is a level of nesting. */
        if (!deepen(p, start, LEVEL_BRACKET))
            return 0;
        at(p, variable)->flags = NVO_FLAG_BRACED;
        /* Only `}` can follow a member inside the braces, so every dot
           there begins one. */
        while (peek(p) == '.')
            if (!parse_dotted_member(p, variable, 1))
                return 0;
        if (peek(p) != '}') {
            if (p->pos >= p->n)
                return fail(p, NVO_ERROR_UNTERMINATED, start);
            return unexpected(p);
        }
        take(p, 1);
        p->depth--;
    } else {
        if (!parse_members(p, variable, where != VARIABLE_IN_STRING))
            return 0;
        if (where != VARIABLE_IN_SPREAD && at(p, variable)->first != 0 &&
            peek(p) == '?') {
            at(p, variable)->flags = NVO_FLAG_OPTIONAL;
            take(p, 1);
        }
    }
    return variable;
}

/* The group's or substitution's pipeline and its close paren: the open
   paren at open is taken, and the pipeline follows. */
static UINT32 parse_parenthesized(PARSER *p, NVO_NODE_KIND kind, UINT32 start)
{
    UINT32 node, pipeline;
    int saved = p->blank_lines;

    if (!deepen(p, start, LEVEL_BRACKET))
        return 0;
    node = make(p, kind, start, 0);
    p->blank_lines = 1;
    pipeline = parse_pipeline(p);
    if (!pipeline || !expect(p, ')', start))
        return 0;
    p->blank_lines = saved;
    p->depth--;
    adopt(p, node, pipeline);
    return seal(p, node);
}

static UINT32 parse_group(PARSER *p)
{
    UINT32 start = p->pos;

    take(p, 1);
    return parse_parenthesized(p, NVO_NODE_GROUP, start);
}

/* A `$` in a word, a double-quoted string or a flag: a substitution,
   `${name}` or `$name`. */
static UINT32 parse_interpolation(PARSER *p, VARIABLE_IN where)
{
    UINT32 start = p->pos;

    if (peek_at(p, start + 1) == '(') {
        take(p, 2);
        return parse_parenthesized(p, NVO_NODE_SUBST, start);
    }
    return parse_variable(p, where);
}

/* ------------------------------------------------------------------------
   Literals */

static NVO_NODE_KIND number_node_kind(NVO_NUMBER_KIND kind)
{
    switch (kind) {
    case NVO_NUMBER_DECIMAL:
        return NVO_NODE_DECIMAL;
    case NVO_NUMBER_SIZE:
        return NVO_NODE_SIZE;
    case NVO_NUMBER_DURATION:
        return NVO_NODE_DURATION;
    default:
        return NVO_NODE_INT;
    }
}

static UINT32 parse_bytes(PARSER *p)
{
    UINT32 start = p->pos, i = start + 3;

    for (;;) {
        while (peek_at(p, i) == ' ')
            i++;
        if (peek_at(p, i) == ']')
            break;
        if (peek_at(p, i) == 0x100u)
            return fail(p, NVO_ERROR_UNTERMINATED, start);
        if (!is_hex(peek_at(p, i)) || !is_hex(peek_at(p, i + 1)))
            return fail(p, NVO_ERROR_LITERAL, i);
        i += 2;
    }
    take(p, i + 1 - start);
    return over(p, NVO_NODE_BYTES, start);
}

static UINT32 parse_number(PARSER *p)
{
    NVO_NUMBER_KIND kind;
    const UNIT *unit;
    NVO_NUMBER value;
    UINT32 start = p->pos, end;

    if (p->s[start] == '0' && peek_at(p, start + 1) == 'x' &&
        peek_at(p, start + 2) == '[')
        return parse_bytes(p);
    end = scan_number(p->s, p->n, start, &kind, &unit);
    if (nvo_number_parse(p->s + start, end - start, &value) != NVO_OK)
        return fail(p, NVO_ERROR_LITERAL, start);
    take(p, end - start);
    return over(p, number_node_kind(kind), start);
}

/* ------------------------------------------------------------------------
   Words and parts */

/* Whether the character ends a word. */
static int ends_word(UINT32 c, READ_MODE mode)
{
    return c == 0x100u || c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '|' || c == ';' || c == ')' || c == ']' || c == '}' ||
           c == '{' || (c == ',' && mode == READ_EXPRESSION);
}

static UINT32 parse_list(PARSER *p);

/* One part of a word: a string, an interpolation, a parenthesized
   pipeline, a list or a run of characters. A `[` in a word begins a list: a
   table is an expression only. */
static UINT32 parse_part(PARSER *p, READ_MODE mode)
{
    UINT32 c = peek(p), start = p->pos, end;

    if (c == '"')
        return parse_double_quoted(p);
    if (c == '\'')
        return parse_single_quoted(p);
    if (c == '$')
        return parse_interpolation(p, VARIABLE_IN_WORD);
    if (c == '(')
        return parse_group(p);
    if (c == '[')
        return parse_list(p);
    end = run_end(p, start, mode);
    if (end == start)
        return fail(p, NVO_ERROR_UNEXPECTED, start);
    take(p, end - start);
    return over(p, NVO_NODE_BARE, start);
}

/* Join first and the parts that touch it into one word. */
static UINT32 more_parts(PARSER *p, UINT32 first, READ_MODE mode)
{
    UINT32 word = 0;

    while (!ends_word(peek(p), mode)) {
        UINT32 part = parse_part(p, mode);

        if (!part)
            return 0;
        if (!word) {
            word = make(p, NVO_NODE_WORD, at(p, first)->offset, 0);
            if (!word)
                return 0;
            adopt(p, word, first);
        }
        adopt(p, word, part);
    }
    return word ? seal(p, word) : first;
}

/* A word, in command mode: its first part may be a path or a glob. */
static UINT32 parse_word(PARSER *p)
{
    UINT32 c = peek(p), start = p->pos, first, end;

    if (c == '"' || c == '\'' || c == '$' || c == '(' || c == '[') {
        first = parse_part(p, READ_COMMAND);
    } else {
        end = run_end(p, start, READ_COMMAND);
        if (end == start)
            return fail(p, NVO_ERROR_UNEXPECTED, start);
        take(p, end - start);
        first = over(p, is_path_run(p, start, end) ? path_kind(p, start, end)
                                                   : NVO_NODE_BARE,
                     start);
    }
    if (!first)
        return 0;
    return more_parts(p, first, READ_COMMAND);
}

/* ------------------------------------------------------------------------
   Expressions */

static UINT32 binary(PARSER *p, NVO_NODE_KIND kind, UINT32 left, UINT32 right)
{
    UINT32 node = make(p, kind, at(p, left)->offset, 0);

    if (!node || !right)
        return 0;
    adopt(p, node, left);
    adopt(p, node, right);
    return seal(p, node);
}

static UINT32 parse_unary(PARSER *p);

/* An operator at this level, after blanks: its kind and its length, or
   NVO_NODE_NONE. */
static NVO_NODE_KIND operator_at(PARSER *p, UINT32 level, UINT32 *length)
{
    UINT32 c, d;

    skip_here(p);
    c = peek(p);
    d = peek_at(p, p->pos + 1);
    *length = 1;
    if (level == 0 && c == '+' && d == '+') {
        *length = 2;
        return NVO_NODE_CONCAT;
    }
    if (level == 1 && c == '+' && d != '+')
        return NVO_NODE_ADD;
    if (level == 1 && c == '-')
        return NVO_NODE_SUB;
    if (level == 2 && c == '*')
        return NVO_NODE_MUL;
    if (level == 2 && c == '/' && d == '/') {
        *length = 2;
        return NVO_NODE_IDIV;
    }
    if (level == 2 && c == '/')
        return NVO_NODE_DIV;
    if (level == 2 && c == '%')
        return NVO_NODE_MOD;
    return NVO_NODE_NONE;
}

#define LEVEL_PRODUCT 2

/* concat, sum and product: left to right, each over the next. */
static UINT32 parse_binary(PARSER *p, UINT32 level)
{
    UINT32 left, length;
    NVO_NODE_KIND kind;

    left = level == LEVEL_PRODUCT ? parse_unary(p) : parse_binary(p, level + 1);
    while (left && (kind = operator_at(p, level, &length)) != NVO_NODE_NONE) {
        UINT32 right;

        take(p, length);
        right = level == LEVEL_PRODUCT ? parse_unary(p)
                                       : parse_binary(p, level + 1);
        left = binary(p, kind, left, right);
    }
    return left;
}

/* The comparison operator at the position, after blanks, and its length. */
static NVO_NODE_KIND comparison_at(PARSER *p, UINT32 *length)
{
    UINT32 c, d;

    skip_here(p);
    c = peek(p);
    d = peek_at(p, p->pos + 1);
    *length = 2;
    if (c == '=' && d == '=')
        return NVO_NODE_EQ;
    if (c == '!' && d == '=')
        return NVO_NODE_NE;
    if (c == '<' && d == '=')
        return NVO_NODE_LE;
    if (c == '>' && d == '=')
        return NVO_NODE_GE;
    *length = 1;
    if (c == '<')
        return NVO_NODE_LT;
    if (c == '>')
        return NVO_NODE_GT;
    if (word_at(p, "in")) {
        *length = 2;
        return NVO_NODE_IN;
    }
    if (word_at(p, "not-in")) {
        *length = 6;
        return NVO_NODE_NOT_IN;
    }
    return NVO_NODE_NONE;
}

static UINT32 parse_compare(PARSER *p)
{
    UINT32 left = parse_binary(p, 0), right, length;
    NVO_NODE_KIND kind;

    if (!left)
        return 0;
    kind = comparison_at(p, &length);
    if (kind == NVO_NODE_NONE)
        return left;
    take(p, length);
    right = parse_binary(p, 0);
    left = binary(p, kind, left, right);
    if (left && comparison_at(p, &length) != NVO_NODE_NONE)
        return fail(p, NVO_ERROR_CHAINED, p->pos);
    return left;
}

static UINT32 parse_not(PARSER *p)
{
    UINT32 start, operand, node;

    skip_here(p);
    start = p->pos;
    if (!word_at(p, "not"))
        return parse_compare(p);
    if (!deepen(p, start, LEVEL_PREFIX))
        return 0;
    take(p, 3);
    operand = parse_not(p);
    p->depth--;
    if (!operand)
        return 0;
    node = make(p, NVO_NODE_NOT, start, 0);
    if (!node)
        return 0;
    adopt(p, node, operand);
    return seal(p, node);
}

/* Parse the operands of `and` or `or` left to right. */
static UINT32 parse_and(PARSER *p)
{
    UINT32 left = parse_not(p);

    while (left) {
        UINT32 right;

        skip_here(p);
        if (!word_at(p, "and"))
            break;
        take(p, 3);
        right = parse_not(p);
        left = binary(p, NVO_NODE_AND, left, right);
    }
    return left;
}

static UINT32 parse_expr(PARSER *p)
{
    UINT32 left = parse_and(p);

    while (left) {
        UINT32 right;

        skip_here(p);
        if (!word_at(p, "or"))
            break;
        take(p, 2);
        right = parse_and(p);
        left = binary(p, NVO_NODE_OR, left, right);
    }
    return left;
}

/* The name of a table's column or a record's key, at the position. */
static UINT32 parse_name(PARSER *p, NVO_NODE_KIND kind)
{
    UINT32 start, end;

    skip_here(p);
    start = p->pos;
    end = kind == NVO_NODE_DEFNAME ? defname_end(p, start) : name_end(p, start);
    if (end == start)
        return unexpected(p);
    take(p, end - start);
    return over(p, kind, start);
}

/* Whether `[[` begins a table, that is, `[ [ name, name ] ;`. */
static int table_ahead(PARSER *p)
{
    UINT32 saved = p->pos;
    int found = 0;

    take(p, 1);
    skip(p, 1);
    if (peek(p) == '[') {
        p->pos++;
        for (;;) {
            UINT32 end;

            skip(p, 1);
            end = name_end(p, p->pos);
            if (end == p->pos)
                break;
            p->pos = end;
            skip(p, 1);
            if (peek(p) == ',') {
                p->pos++;
                continue;
            }
            if (peek(p) == ']') {
                p->pos++;
                skip(p, 1);
                found = peek(p) == ';';
            }
            break;
        }
    }
    p->pos = saved;
    return found;
}

/* A bracketed list of items, each parsed by item, from the open bracket at
   p->pos, ended by close, with a comma before it if trailing allows. It
   holds no items or one or more. The node is kind. */
typedef UINT32 (*ITEM)(PARSER *p);

typedef enum { TRAILING_COMMA_ALLOWED, TRAILING_COMMA_REFUSED } TRAILING_COMMA;

static UINT32 parse_items(PARSER *p, NVO_NODE_KIND kind, UINT32 close,
                          ITEM item, TRAILING_COMMA trailing)
{
    UINT32 start = p->pos, node;
    int saved = p->blank_lines;

    if (!deepen(p, start, LEVEL_BRACKET))
        return 0;
    node = make(p, kind, start, 0);
    if (!node)
        return 0;
    p->blank_lines = 1;
    take(p, 1);
    for (;;) {
        UINT32 child;

        skip_here(p);
        if (peek(p) == close &&
            (trailing == TRAILING_COMMA_ALLOWED || at(p, node)->first == 0))
            break;
        child = item(p);
        if (!child)
            return 0;
        adopt(p, node, child);
        skip_here(p);
        if (peek(p) != ',')
            break;
        take(p, 1);
    }
    if (!expect(p, close, start))
        return 0;
    p->blank_lines = saved;
    p->depth--;
    return seal(p, node);
}

static int spread_at(const PARSER *p)
{
    return peek(p) == '.' && peek_at(p, p->pos + 1) == '.' &&
           peek_at(p, p->pos + 2) == '.';
}

/* A spread's operator, then the node of what it spreads. */
static UINT32 parse_spread_of(PARSER *p, ITEM operand)
{
    UINT32 start = p->pos, spread, inner;

    take(p, 3);
    inner = operand(p);
    if (!inner)
        return 0;
    spread = make(p, NVO_NODE_SPREAD, start, 0);
    if (!spread)
        return 0;
    adopt(p, spread, inner);
    return seal(p, spread);
}

static UINT32 parse_element(PARSER *p)
{
    skip_here(p);
    if (spread_at(p))
        return parse_spread_of(p, parse_expr);
    return parse_expr(p);
}

static UINT32 parse_row(PARSER *p)
{
    skip_here(p);
    if (peek(p) != '[')
        return unexpected(p);
    return parse_items(p, NVO_NODE_ROW, ']', parse_expr,
                       TRAILING_COMMA_REFUSED);
}

static UINT32 parse_column(PARSER *p) { return parse_name(p, NVO_NODE_NAME); }

static UINT32 parse_table(PARSER *p)
{
    UINT32 start = p->pos, table, columns, rows = 0;
    int saved = p->blank_lines;

    if (!deepen(p, start, LEVEL_BRACKET))
        return 0;
    table = make(p, NVO_NODE_TABLE, start, 0);
    if (!table)
        return 0;
    p->blank_lines = 1;
    take(p, 1);
    skip_here(p);
    columns = parse_items(p, NVO_NODE_COLUMNS, ']', parse_column,
                          TRAILING_COMMA_REFUSED);
    if (!columns)
        return 0;
    adopt(p, table, columns);
    skip_here(p);
    take(p, 1); /* the `;` table_ahead found */
    for (;;) {
        UINT32 row;

        skip_here(p);
        if (peek(p) == ']' && rows > 0)
            break;
        row = parse_row(p);
        if (!row)
            return 0;
        adopt(p, table, row);
        rows++;
        skip_here(p);
        if (peek(p) != ',')
            break;
        take(p, 1);
    }
    if (!expect(p, ']', start))
        return 0;
    p->blank_lines = saved;
    p->depth--;
    return seal(p, table);
}

static UINT32 parse_list(PARSER *p)
{
    return parse_items(p, NVO_NODE_LIST, ']', parse_element,
                       TRAILING_COMMA_ALLOWED);
}

static UINT32 parse_list_or_table(PARSER *p)
{
    if (table_ahead(p))
        return parse_table(p);
    return parse_list(p);
}

/* A record's entry: a key, a colon and an expression, or a spread. */
static UINT32 parse_entry(PARSER *p)
{
    UINT32 start, key, entry, value;

    skip_here(p);
    if (spread_at(p))
        return parse_spread_of(p, parse_expr);
    start = p->pos;
    if (peek(p) == '"')
        key = parse_double_quoted(p);
    else if (peek(p) == '\'')
        key = parse_single_quoted(p);
    else
        key = parse_name(p, NVO_NODE_NAME);
    if (!key)
        return 0;
    skip_here(p);
    if (peek(p) != ':')
        return unexpected(p);
    take(p, 1);
    value = parse_expr(p);
    if (!value)
        return 0;
    entry = make(p, NVO_NODE_ENTRY, start, 0);
    if (!entry)
        return 0;
    adopt(p, entry, key);
    adopt(p, entry, value);
    return seal(p, entry);
}

static UINT32 parse_record(PARSER *p)
{
    return parse_items(p, NVO_NODE_RECORD, '}', parse_entry,
                       TRAILING_COMMA_ALLOWED);
}

/* The names of a closure's parameters, one or more with no comma after the
   last, as children of params. */
static int parse_closure_names(PARSER *p, UINT32 params)
{
    for (;;) {
        UINT32 name = parse_name(p, NVO_NODE_NAME);

        if (!name)
            return 0;
        adopt(p, params, name);
        skip(p, 0);
        if (peek(p) != ',')
            return 1;
        take(p, 1);
    }
}

/* A closure's parameter names are between bars: `|a, b|`. */
static UINT32 parse_closure(PARSER *p)
{
    UINT32 start = p->pos, closure, params, script;
    int saved = p->blank_lines;

    if (!deepen(p, start, LEVEL_BRACKET))
        return 0;
    closure = make(p, NVO_NODE_CLOSURE, start, 0);
    params = make(p, NVO_NODE_PARAMS, start, 0);
    if (!closure || !params)
        return 0;
    p->blank_lines = 0;
    take(p, 1);
    skip(p, 0);
    take(p, 1); /* the opening bar */
    skip(p, 0);
    if (peek(p) != '|' && !parse_closure_names(p, params))
        return 0;
    if (peek(p) != '|') {
        if (p->pos >= p->n)
            return fail(p, NVO_ERROR_UNTERMINATED, start);
        return unexpected(p);
    }
    take(p, 1);
    seal(p, params);
    script = parse_script(p);
    if (!script)
        return 0;
    if (peek(p) != '}') {
        if (p->pos >= p->n)
            return fail(p, NVO_ERROR_UNTERMINATED, start);
        return unexpected(p);
    }
    take(p, 1);
    p->blank_lines = saved;
    p->depth--;
    adopt(p, closure, params);
    adopt(p, closure, script);
    return seal(p, closure);
}

/* Whether the `{` at p->pos begins a closure: a bar follows it on its
   line. */
static int closure_ahead(const PARSER *p)
{
    UINT32 i = p->pos + 1;

    while (peek_at(p, i) == ' ' || peek_at(p, i) == '\t')
        i++;
    return peek_at(p, i) == '|';
}

static UINT32 parse_primary(PARSER *p)
{
    UINT32 c, start, end;

    skip_here(p);
    c = peek(p);
    start = p->pos;
    if (c == 0x100u)
        return fail(p, NVO_ERROR_UNEXPECTED, p->n);
    if (c == '"')
        return parse_double_quoted(p);
    if (c == '\'')
        return parse_single_quoted(p);
    if (c == '$') {
        if (peek_at(p, start + 1) == '(')
            return parse_interpolation(p, VARIABLE_IN_WORD);
        if (!is_name_start(peek_at(p, start + 1)))
            return fail(p, NVO_ERROR_UNEXPECTED, start);
        end = name_end(p, start + 1);
        take(p, end - start);
        return over(p, NVO_NODE_VARIABLE, start + 1);
    }
    if (c == '(')
        return parse_group(p);
    if (c == '[')
        return parse_list_or_table(p);
    if (c == '{')
        return closure_ahead(p) ? parse_closure(p) : parse_record(p);
    if (is_digit(c))
        return parse_number(p);
    end = run_end(p, start, READ_EXPRESSION);
    if (is_path_run(p, start, end)) {
        UINT32 path;

        take(p, end - start);
        path = over(p, path_kind(p, start, end), start);
        return path ? more_parts(p, path, READ_EXPRESSION) : 0;
    }
    end = name_end(p, start);
    if (is_text(p, start, end, "true") || is_text(p, start, end, "false") ||
        is_text(p, start, end, "nothing")) {
        take(p, end - start);
        return over(p, p->s[start] == 't'
                           ? NVO_NODE_TRUE
                           : (p->s[start] == 'f' ? NVO_NODE_FALSE
                                                 : NVO_NODE_NOTHING),
                    start);
    }
    return no_operand(p);
}

/* A primary and the members that touch it. */
static UINT32 parse_postfix(PARSER *p)
{
    UINT32 base = parse_primary(p);

    while (base && peek(p) == '.') {
        UINT32 member, access;

        take(p, 1);
        member = parse_member(p, 1);
        if (!member)
            return 0;
        access = make(p, NVO_NODE_ACCESS, at(p, base)->offset, 0);
        if (!access)
            return 0;
        adopt(p, access, base);
        adopt(p, access, member);
        if (peek(p) == '?') {
            at(p, access)->flags = NVO_FLAG_OPTIONAL;
            take(p, 1);
        }
        base = seal(p, access);
    }
    return base;
}

static UINT32 parse_unary(PARSER *p)
{
    UINT32 start, operand, node;

    skip_here(p);
    start = p->pos;
    if (peek(p) != '-')
        return parse_postfix(p);
    if (!deepen(p, start, LEVEL_PREFIX))
        return 0;
    take(p, 1);
    operand = parse_unary(p);
    p->depth--;
    if (!operand)
        return 0;
    node = make(p, NVO_NODE_NEG, start, 0);
    if (!node)
        return 0;
    adopt(p, node, operand);
    return seal(p, node);
}

/* ------------------------------------------------------------------------
   Commands and pipelines */

/* Whether the character ends a stage (or the source). */
static int ends_stage(const PARSER *p)
{
    UINT32 c = peek(p);

    return c == 0x100u || c == '|' || c == ';' || c == ')' || c == '}' ||
           c == ']' || newline_length(p, p->pos) != 0;
}

/* Whether a flag is at the run [start, end): `--`, `--name` with an
   optional `=` and what follows, or `-` and a letter. */
static int is_flag_run(const PARSER *p, UINT32 start, UINT32 end)
{
    UINT32 length = end - start, after;

    if (length < 2 || p->s[start] != '-')
        return 0;
    if (p->s[start + 1] != '-')
        return is_letter(p->s[start + 1]);
    if (length == 2)
        return 1;
    if (!is_name_start(p->s[start + 2]))
        return 0;
    after = defname_end(p, start + 2);
    return after == end || p->s[after] == '=';
}

/* Whether the run [start, end) is `=>` standing alone, the mark of a grant
   (L17): a word of its own, with no part touching it, so that `=>$x` is a
   word of two parts and `=>x` is text. */
static int is_grant_mark(const PARSER *p, UINT32 start, UINT32 end)
{
    return is_text(p, start, end, "=>") &&
           ends_word(peek_at(p, end), READ_COMMAND);
}

static int variable_ahead(const PARSER *p, UINT32 i)
{
    return peek_at(p, i) == '$' && is_name_start(peek_at(p, i + 1));
}

/* The argument after `...`: a variable and its members, or a group. */
static UINT32 parse_spread_operand(PARSER *p)
{
    if (peek(p) == '(')
        return parse_group(p);
    return parse_variable(p, VARIABLE_IN_SPREAD);
}

static UINT32 parse_flag(PARSER *p, UINT32 end)
{
    UINT32 start = p->pos, flag;

    take(p, end - start);
    flag = over(p, NVO_NODE_FLAG, start);
    while (flag && !ends_word(peek(p), READ_COMMAND)) {
        UINT32 part = parse_part(p, READ_COMMAND);

        if (!part)
            return 0;
        adopt(p, flag, part);
    }
    return flag;
}

static UINT32 parse_argument(PARSER *p)
{
    UINT32 c = peek(p), start = p->pos, end = run_end(p, start, READ_COMMAND);

    if (c == '{') {
        if (closure_ahead(p))
            return parse_closure(p);
        return parse_block(p);
    }
    if (is_grant_mark(p, start, end)) {
        UINT32 grant, target;

        take(p, 2);
        skip_here(p);
        if (ends_stage(p))
            return unexpected(p);
        if (spread_at(p) && (peek_at(p, p->pos + 3) == '(' ||
                             variable_ahead(p, p->pos + 3)))
            target = parse_spread_of(p, parse_spread_operand);
        else
            target = parse_word(p);
        if (!target)
            return 0;
        grant = make(p, NVO_NODE_GRANT, start, 0);
        if (!grant)
            return 0;
        adopt(p, grant, target);
        return seal(p, grant);
    }
    if (spread_at(p) && (peek_at(p, start + 3) == '(' ||
                         variable_ahead(p, start + 3)))
        return parse_spread_of(p, parse_spread_operand);
    if (c == '-' && is_flag_run(p, start, end))
        return parse_flag(p, end);
    return parse_word(p);
}

static UINT32 parse_command(PARSER *p)
{
    UINT32 start = p->pos, command, head, run_to, head_from;

    command = make(p, NVO_NODE_COMMAND, start, 0);
    if (!command)
        return 0;
    if (peek(p) == '^') {
        at(p, command)->flags = NVO_FLAG_CARET;
        take(p, 1);
    }
    head_from = p->pos;
    run_to = run_end(p, head_from, READ_COMMAND);
    /* A `#` where the name should begin starts a comment, and a caret
       followed by one has no name. */
    if (run_to == head_from || peek(p) == '#')
        return fail(p, NVO_ERROR_UNEXPECTED, head_from);
    take(p, run_to - head_from);
    head = over(p, NVO_NODE_BARE, head_from);
    if (!head)
        return 0;
    adopt(p, command, head);
    for (;;) {
        UINT32 argument;

        skip_here(p);
        if (ends_stage(p))
            break;
        argument = parse_argument(p);
        if (!argument)
            return 0;
        adopt(p, command, argument);
    }
    return seal(p, command);
}

/* Whether the run [start, end) is one number literal. */
static int is_number_run(const PARSER *p, UINT32 start, UINT32 end)
{
    NVO_NUMBER_KIND kind;
    const UNIT *unit;

    return is_digit(p->s[start]) &&
           scan_number(p->s, p->n, start, &kind, &unit) == end;
}

/* A stage: a command if its first token is a bare word that is not a
   keyword, not wholly a literal and not starting with `-`, or starts
   with `^`; else an expression (L2, Stages). */
static UINT32 parse_stage(PARSER *p)
{
    UINT32 c, start, end;

    skip_here(p);
    if (ends_stage(p))
        return unexpected(p);
    c = peek(p);
    start = p->pos;
    if (c == '^')
        return parse_command(p);
    if (is_meta(c, READ_COMMAND) || c == '-')
        return parse_expr(p);
    end = run_end(p, start, READ_COMMAND);
    /* A mark has no place at the head, and a head is one run of text, so a
       run of `=>` is not one whatever touches it: a program named `=>` is
       run as `^=>`. */
    if (is_text(p, start, end, "=>"))
        return fail(p, NVO_ERROR_UNEXPECTED, start);
    if (is_keyword(p, start, end) || is_path_run(p, start, end) ||
        is_number_run(p, start, end) ||
        (is_text(p, start, end, "0x") && peek_at(p, end) == '['))
        return parse_expr(p);
    return parse_command(p);
}

static UINT32 parse_pipeline(PARSER *p)
{
    UINT32 pipeline = 0, stage = parse_stage(p);

    while (stage) {
        UINT32 next;

        skip_here(p);
        if (peek(p) != '|')
            break;
        take(p, 1);
        skip(p, 0);
        p->pos += newline_length(p, p->pos);
        if (!pipeline) {
            pipeline = make(p, NVO_NODE_PIPELINE, at(p, stage)->offset, 0);
            if (!pipeline)
                return 0;
            adopt(p, pipeline, stage);
        }
        next = parse_stage(p);
        if (!next)
            return 0;
        adopt(p, pipeline, next);
        stage = next;
    }
    if (!stage)
        return 0;
    return pipeline ? seal(p, pipeline) : stage;
}

/* ------------------------------------------------------------------------
   Types and parameters */

static const char *const type_words[] = {
    "any",      "nothing", "bool",  "int",    "decimal", "number", "string",
    "bytes",    "duration", "size",  "time",   "closure", "error",  "glob",
    "path",     "list",    "stream", "record", "table",
};

/* Take the character, or fail. */
static int take_char(PARSER *p, UINT32 ch, UINT32 open)
{
    skip_here(p);
    if (peek(p) == ch) {
        take(p, 1);
        return 1;
    }
    if (open != 0x100u && p->pos >= p->n)
        (void)fail(p, NVO_ERROR_UNTERMINATED, open);
    else
        (void)unexpected(p);
    return 0;
}

static UINT32 parse_field_type(PARSER *p)
{
    UINT32 start, key, field, type;

    skip_here(p);
    start = p->pos;
    if (peek(p) == '"')
        key = parse_double_quoted(p);
    else if (peek(p) == '\'')
        key = parse_single_quoted(p);
    else
        key = parse_name(p, NVO_NODE_NAME);
    if (!key || !take_char(p, ':', 0x100u))
        return 0;
    type = parse_type(p);
    if (!type)
        return 0;
    field = make(p, NVO_NODE_FIELD_TYPE, start, 0);
    if (!field)
        return 0;
    adopt(p, field, key);
    adopt(p, field, type);
    return seal(p, field);
}

/* The angle brackets of a type: `<`, items by item, `>`. */
static int parse_type_arguments(PARSER *p, UINT32 type, ITEM item, int many)
{
    UINT32 open = p->pos;

    if (!deepen(p, open, LEVEL_BRACKET))
        return 0;
    take(p, 1);
    for (;;) {
        UINT32 child = item(p);

        if (!child)
            return 0;
        adopt(p, type, child);
        skip_here(p);
        if (!many || peek(p) != ',')
            break;
        take(p, 1);
    }
    if (!take_char(p, '>', open))
        return 0;
    p->depth--;
    return 1;
}

static UINT32 parse_type(PARSER *p)
{
    UINT32 start, end, k, type, kind = 0;
    UINT32 saved;

    skip_here(p);
    start = p->pos;
    end = start;
    while (is_letter(peek_at(p, end)))
        end++;
    for (k = 0; k < sizeof type_words / sizeof type_words[0]; k++)
        if (is_text(p, start, end, type_words[k]))
            kind = k + 1;
    if (kind == 0)
        return unexpected(p);
    take(p, end - start);
    type = over(p, NVO_NODE_TYPE, start);
    if (!type)
        return 0;
    saved = p->pos;
    skip_here(p);
    if (is_text(p, start, end, "path") && peek(p) == '(') {
        UINT32 open = p->pos, word_end;

        take(p, 1);
        skip_here(p);
        word_end = p->pos;
        while (is_letter(peek_at(p, word_end)))
            word_end++;
        if (is_text(p, p->pos, word_end, "read"))
            at(p, type)->flags = NVO_FLAG_READ;
        else if (is_text(p, p->pos, word_end, "write"))
            at(p, type)->flags = NVO_FLAG_WRITE;
        else if (ends_in_start_of(p, p->pos, "read") ||
                 ends_in_start_of(p, p->pos, "write"))
            return fail(p, NVO_ERROR_UNTERMINATED, open);
        else
            return unexpected(p);
        take(p, word_end - p->pos);
        if (!take_char(p, ')', open))
            return 0;
    } else if (peek(p) == '<' && (is_text(p, start, end, "list") ||
                                  is_text(p, start, end, "stream"))) {
        if (!parse_type_arguments(p, type, parse_type, 0))
            return 0;
    } else if (peek(p) == '<' && (is_text(p, start, end, "record") ||
                                  is_text(p, start, end, "table"))) {
        if (!parse_type_arguments(p, type, parse_field_type, 1))
            return 0;
    } else if (is_text(p, start, end, "stream")) {
        return unexpected(p);
    } else {
        p->pos = saved;
    }
    return type;
}

/* An optional `: type` after a parameter's name. */
static int parse_parameter_type(PARSER *p, UINT32 param)
{
    UINT32 type;

    skip_here(p);
    if (peek(p) != ':')
        return 1;
    take(p, 1);
    type = parse_type(p);
    if (!type)
        return 0;
    adopt(p, param, type);
    return 1;
}

/* An optional `= expr` after a parameter's name and type. */
static int parse_parameter_default(PARSER *p, UINT32 param)
{
    UINT32 value;

    skip_here(p);
    if (peek(p) != '=')
        return 1;
    take(p, 1);
    value = parse_expr(p);
    if (!value)
        return 0;
    adopt(p, param, value);
    return 1;
}

/* What of a flag parameter's short form, `(-x)` with x a letter, the source
   holds from where the parser is: all of it, none of it, or the start of it
   and then the end of the source. */
typedef enum { SHORT_FORM_ABSENT, SHORT_FORM_CUT, SHORT_FORM_WHOLE } SHORT_FORM;

static SHORT_FORM short_form_at(const PARSER *p)
{
    static const char shape[] = "(-x)";
    UINT32 k;

    for (k = 0; k < sizeof shape - 1; k++) {
        UINT32 c = peek_at(p, p->pos + k);

        if (c == 0x100u)
            return k > 0 ? SHORT_FORM_CUT : SHORT_FORM_ABSENT;
        if (k == 2 ? !is_letter(c) : c != (UINT32)shape[k])
            return SHORT_FORM_ABSENT;
    }
    return SHORT_FORM_WHOLE;
}

/* `--name`, a `(-x)` short form, a type and a default, the last three
   optional. */
static UINT32 parse_flag_parameter(PARSER *p, UINT32 start)
{
    UINT32 param = make(p, NVO_NODE_FLAG_PARAM, start, 0), name;

    if (!param)
        return 0;
    take(p, 2);
    name = parse_name(p, NVO_NODE_DEFNAME);
    if (!name)
        return 0;
    adopt(p, param, name);
    switch (short_form_at(p)) {
    case SHORT_FORM_WHOLE: {
        UINT32 letter = p->pos + 2;

        take(p, 4);
        name = make(p, NVO_NODE_SHORT, letter, 1);
        if (!name)
            return 0;
        adopt(p, param, name);
        break;
    }
    case SHORT_FORM_CUT:
        return fail(p, NVO_ERROR_UNTERMINATED, p->pos);
    case SHORT_FORM_ABSENT:
        break;
    }
    if (!parse_parameter_type(p, param) || !parse_parameter_default(p, param))
        return 0;
    return seal(p, param);
}

/* `...name: type`. */
static UINT32 parse_rest_parameter(PARSER *p, UINT32 start)
{
    UINT32 param = make(p, NVO_NODE_REST_PARAM, start, 0), name, type;

    if (!param)
        return 0;
    take(p, 3);
    name = parse_name(p, NVO_NODE_NAME);
    if (!name)
        return 0;
    adopt(p, param, name);
    if (!take_char(p, ':', 0x100u))
        return 0;
    type = parse_type(p);
    if (!type)
        return 0;
    adopt(p, param, type);
    return seal(p, param);
}

/* `name: type` or `name?: type`, and a default. */
static UINT32 parse_positional_parameter(PARSER *p, UINT32 start)
{
    UINT32 param = make(p, NVO_NODE_PARAM, start, 0), name, type;

    if (!param)
        return 0;
    name = parse_name(p, NVO_NODE_NAME);
    if (!name)
        return 0;
    adopt(p, param, name);
    if (peek(p) == '?') {
        at(p, param)->flags = NVO_FLAG_OPTIONAL;
        take(p, 1);
    }
    if (!take_char(p, ':', 0x100u))
        return 0;
    type = parse_type(p);
    if (!type)
        return 0;
    adopt(p, param, type);
    if (!parse_parameter_default(p, param))
        return 0;
    return seal(p, param);
}

static UINT32 parse_parameter(PARSER *p)
{
    UINT32 start;

    skip_here(p);
    start = p->pos;
    if (peek(p) == '-' && peek_at(p, start + 1) == '-')
        return parse_flag_parameter(p, start);
    if (spread_at(p))
        return parse_rest_parameter(p, start);
    return parse_positional_parameter(p, start);
}

static UINT32 parse_parameters(PARSER *p)
{
    UINT32 start = p->pos, params;
    int saved = p->blank_lines;

    if (!deepen(p, start, LEVEL_BRACKET))
        return 0;
    params = make(p, NVO_NODE_PARAMS, start, 0);
    if (!params)
        return 0;
    p->blank_lines = 1;
    take(p, 1);
    for (;;) {
        UINT32 param;

        skip_here(p);
        if (peek(p) == ')')
            break;
        param = parse_parameter(p);
        if (!param)
            return 0;
        adopt(p, params, param);
        skip_here(p);
        if (peek(p) != ',')
            break;
        take(p, 1);
    }
    if (!expect(p, ')', start))
        return 0;
    p->blank_lines = saved;
    p->depth--;
    return seal(p, params);
}

/* ------------------------------------------------------------------------
   Statements */

/* After an else, a catch or a statement: the end of a statement. */
static int at_statement_end(const PARSER *p)
{
    UINT32 c = peek(p);

    return c == 0x100u || c == ';' || c == '}' ||
           newline_length(p, p->pos) != 0;
}

/* Parse a binding or its kin, after the keyword at p->pos of length
   length. */
static UINT32 parse_binding(PARSER *p, NVO_NODE_KIND kind, UINT32 start,
                            UINT32 keyword_length, UINT32 flags)
{
    UINT32 node, name, type, value;

    take(p, keyword_length);
    node = make(p, kind, start, 0);
    name = parse_name(p, NVO_NODE_NAME);
    if (!node || !name)
        return 0;
    at(p, node)->flags = (UINT16)flags;
    adopt(p, node, name);
    skip_here(p);
    if (peek(p) == ':') {
        take(p, 1);
        type = parse_type(p);
        if (!type)
            return 0;
        adopt(p, node, type);
        skip_here(p);
    }
    if (peek(p) != '=' || peek_at(p, p->pos + 1) == '=' ||
        peek_at(p, p->pos + 1) == '>')
        return unexpected(p);
    take(p, 1);
    value = parse_expr(p);
    if (!value)
        return 0;
    adopt(p, node, value);
    return seal(p, node);
}

static UINT32 parse_set(PARSER *p, UINT32 start)
{
    UINT32 node, name, value, op = NVO_SET_ASSIGN, length = 1, c, d;

    take(p, 3);
    node = make(p, NVO_NODE_SET, start, 0);
    name = parse_name(p, NVO_NODE_NAME);
    if (!node || !name)
        return 0;
    adopt(p, node, name);
    while (peek(p) == '.') {
        UINT32 member;

        take(p, 1);
        member = parse_member(p, 1);
        if (!member)
            return 0;
        adopt(p, node, member);
    }
    skip_here(p);
    c = peek(p);
    d = peek_at(p, p->pos + 1);
    if (c == '+' && d == '+' && peek_at(p, p->pos + 2) == '=') {
        op = NVO_SET_CONCAT;
        length = 3;
    } else if (c == '+' && d == '=') {
        op = NVO_SET_ADD;
        length = 2;
    } else if (c == '-' && d == '=') {
        op = NVO_SET_SUBTRACT;
        length = 2;
    } else if (!(c == '=' && d != '=' && d != '>')) {
        return unexpected(p);
    }
    take(p, length);
    at(p, node)->flags = (UINT16)op;
    value = parse_expr(p);
    if (!value)
        return 0;
    adopt(p, node, value);
    return seal(p, node);
}

static UINT32 parse_definition(PARSER *p, UINT32 start, UINT32 flags)
{
    UINT32 def, name, params, type, body;

    take(p, 3);
    def = make(p, NVO_NODE_DEF, start, 0);
    name = parse_name(p, NVO_NODE_DEFNAME);
    if (!def || !name)
        return 0;
    at(p, def)->flags = (UINT16)flags;
    adopt(p, def, name);
    skip_here(p);
    if (peek(p) != '(')
        return unexpected(p);
    params = parse_parameters(p);
    if (!params)
        return 0;
    adopt(p, def, params);
    skip_here(p);
    if (peek(p) == '-' && peek_at(p, p->pos + 1) == '>') {
        take(p, 2);
        type = parse_type(p);
        if (!type)
            return 0;
        adopt(p, def, type);
    }
    body = parse_block(p);
    if (!body)
        return 0;
    adopt(p, def, body);
    return seal(p, def);
}

static UINT32 parse_use(PARSER *p, UINT32 start)
{
    UINT32 use, path, from, to;

    take(p, 3);
    use = make(p, NVO_NODE_USE, start, 0);
    skip_here(p);
    from = p->pos;
    to = run_end(p, from, READ_EXPRESSION);
    if (!use || !is_path_run(p, from, to) ||
        path_kind(p, from, to) != NVO_NODE_PATH)
        return unexpected(p);
    take(p, to - from);
    path = over(p, NVO_NODE_PATH, from);
    if (!path)
        return 0;
    adopt(p, use, path);
    skip_here(p);
    if (word_at(p, "as")) {
        UINT32 name;

        take(p, 2);
        name = parse_name(p, NVO_NODE_NAME);
        if (!name)
            return 0;
        adopt(p, use, name);
    }
    return seal(p, use);
}

/* Parse the block of a statement, adopted by parent. */
static int parse_body(PARSER *p, UINT32 parent)
{
    UINT32 block = parse_block(p);

    if (!block)
        return 0;
    adopt(p, parent, block);
    return 1;
}

/* Whether the keyword follows the block that has just ended, on its line
   or after one newline. If it does it is not taken, and the position is
   at it; if not the position is where it was. */
static int keyword_follows(PARSER *p, const char *keyword)
{
    UINT32 saved = p->pos;

    skip(p, 0);
    p->pos += newline_length(p, p->pos);
    skip(p, 0);
    if (keyword_at(p, keyword))
        return 1;
    p->pos = saved;
    return 0;
}

static UINT32 parse_if(PARSER *p, UINT32 start)
{
    UINT32 node, cond;

    take(p, 2);
    node = make(p, NVO_NODE_IF, start, 0);
    cond = parse_expr(p);
    if (!node || !cond)
        return 0;
    adopt(p, node, cond);
    if (!parse_body(p, node))
        return 0;
    while (keyword_follows(p, "else")) {
        UINT32 arm, from = p->pos;

        take(p, 4);
        skip_here(p);
        if (keyword_at(p, "if")) {
            take(p, 2);
            arm = make(p, NVO_NODE_ELIF, from, 0);
            cond = parse_expr(p);
            if (!arm || !cond)
                return 0;
            adopt(p, arm, cond);
            if (!parse_body(p, arm))
                return 0;
            adopt(p, node, seal(p, arm));
            continue;
        }
        arm = make(p, NVO_NODE_ELSE, from, 0);
        if (!arm || !parse_body(p, arm))
            return 0;
        adopt(p, node, seal(p, arm));
        break;
    }
    return seal(p, node);
}

static UINT32 parse_for(PARSER *p, UINT32 start)
{
    UINT32 node, name, subject;

    take(p, 3);
    node = make(p, NVO_NODE_FOR, start, 0);
    name = parse_name(p, NVO_NODE_NAME);
    if (!node || !name)
        return 0;
    adopt(p, node, name);
    skip_here(p);
    if (!word_at(p, "in"))
        return unexpected(p);
    take(p, 2);
    subject = parse_expr(p);
    if (!subject)
        return 0;
    adopt(p, node, subject);
    if (!parse_body(p, node))
        return 0;
    return seal(p, node);
}

static UINT32 parse_while(PARSER *p, UINT32 start)
{
    UINT32 node, cond;

    take(p, 5);
    node = make(p, NVO_NODE_WHILE, start, 0);
    cond = parse_expr(p);
    if (!node || !cond)
        return 0;
    adopt(p, node, cond);
    if (!parse_body(p, node))
        return 0;
    return seal(p, node);
}

static UINT32 parse_try(PARSER *p, UINT32 start)
{
    UINT32 node;

    take(p, 3);
    node = make(p, NVO_NODE_TRY, start, 0);
    if (!node || !parse_body(p, node))
        return 0;
    if (keyword_follows(p, "catch")) {
        UINT32 arm = make(p, NVO_NODE_CATCH, p->pos, 0);

        take(p, 5);
        skip_here(p);
        if (arm && is_name_start(peek(p))) {
            UINT32 name = parse_name(p, NVO_NODE_NAME);

            if (!name)
                return 0;
            adopt(p, arm, name);
        }
        if (!arm || !parse_body(p, arm))
            return 0;
        adopt(p, node, seal(p, arm));
    }
    return seal(p, node);
}

static UINT32 parse_return(PARSER *p, UINT32 start)
{
    UINT32 node = make(p, NVO_NODE_RETURN, start, 0);

    take(p, 6);
    skip_here(p);
    if (!node)
        return 0;
    if (!at_statement_end(p)) {
        UINT32 value = parse_expr(p);

        if (!value)
            return 0;
        adopt(p, node, value);
    }
    return seal(p, node);
}

/* The leaf statement of one keyword: break or continue. */
static UINT32 parse_keyword_statement(PARSER *p, NVO_NODE_KIND kind,
                                      UINT32 length)
{
    UINT32 start = p->pos;

    take(p, length);
    return over(p, kind, start);
}

static UINT32 parse_statement(PARSER *p)
{
    UINT32 start, end;

    skip_here(p);
    start = p->pos;
    end = run_end(p, start, READ_COMMAND);
    if (is_text(p, start, end, "let"))
        return parse_binding(p, NVO_NODE_LET, start, 3, 0);
    if (is_text(p, start, end, "var"))
        return parse_binding(p, NVO_NODE_VAR, start, 3, 0);
    if (is_text(p, start, end, "set"))
        return parse_set(p, start);
    if (is_text(p, start, end, "def"))
        return parse_definition(p, start, 0);
    if (is_text(p, start, end, "export")) {
        UINT32 word_from, word_to, node;

        take(p, 6);
        skip_here(p);
        word_from = p->pos;
        word_to = run_end(p, word_from, READ_COMMAND);
        if (is_text(p, word_from, word_to, "let"))
            node = parse_binding(p, NVO_NODE_LET, word_from, 3,
                                 NVO_FLAG_EXPORT);
        else if (is_text(p, word_from, word_to, "def"))
            node = parse_definition(p, word_from, NVO_FLAG_EXPORT);
        else
            return unexpected(p);
        if (node)
            at(p, node)->offset = start;
        return node ? seal(p, node) : 0;
    }
    if (is_text(p, start, end, "use"))
        return parse_use(p, start);
    if (is_text(p, start, end, "if"))
        return parse_if(p, start);
    if (is_text(p, start, end, "for"))
        return parse_for(p, start);
    if (is_text(p, start, end, "while"))
        return parse_while(p, start);
    if (is_text(p, start, end, "try"))
        return parse_try(p, start);
    if (is_text(p, start, end, "break"))
        return parse_keyword_statement(p, NVO_NODE_BREAK, 5);
    if (is_text(p, start, end, "continue"))
        return parse_keyword_statement(p, NVO_NODE_CONTINUE, 8);
    if (is_text(p, start, end, "return"))
        return parse_return(p, start);
    return parse_pipeline(p);
}

/* Skip the separators between statements: newlines and semicolons. */
static void skip_separators(PARSER *p)
{
    for (;;) {
        skip(p, 1);
        if (peek(p) != ';')
            return;
        p->pos++;
    }
}

/* Whether a statement may end here: at a separator, a closing brace or
   the end of the source. */
static int statement_ends(const PARSER *p)
{
    return at_statement_end(p);
}

static UINT32 parse_script(PARSER *p)
{
    UINT32 script = make(p, NVO_NODE_SCRIPT, p->pos, 0);

    if (!script)
        return 0;
    for (;;) {
        UINT32 statement;

        skip_separators(p);
        if (p->pos >= p->n || peek(p) == '}')
            break;
        statement = parse_statement(p);
        if (!statement)
            return 0;
        adopt(p, script, statement);
        skip(p, 0);
        if (!statement_ends(p))
            return unexpected(p);
    }
    return seal(p, script);
}

static UINT32 parse_block(PARSER *p)
{
    UINT32 start, block, script;
    int saved = p->blank_lines;

    skip_here(p);
    start = p->pos;
    if (peek(p) != '{')
        return unexpected(p);
    if (!deepen(p, start, LEVEL_BRACKET))
        return 0;
    block = make(p, NVO_NODE_BLOCK, start, 0);
    if (!block)
        return 0;
    p->blank_lines = 0;
    take(p, 1);
    script = parse_script(p);
    if (!script)
        return 0;
    if (peek(p) != '}') {
        if (p->pos >= p->n)
            return fail(p, NVO_ERROR_UNTERMINATED, start);
        return unexpected(p);
    }
    take(p, 1);
    p->blank_lines = saved;
    p->depth--;
    adopt(p, block, script);
    return seal(p, block);
}

/* `needs` and its record, at the top of a file. */
static UINT32 parse_needs(PARSER *p, UINT32 start)
{
    UINT32 node = make(p, NVO_NODE_NEEDS, start, 0), record;

    take(p, 5);
    skip_here(p);
    if (!node)
        return 0;
    if (peek(p) != '{')
        return unexpected(p);
    record = parse_record(p);
    if (!record)
        return 0;
    adopt(p, node, record);
    seal(p, node);
    skip(p, 0);
    if (!statement_ends(p))
        return unexpected(p);
    return node;
}

static UINT32 parse_file(PARSER *p, NVO_PARSE_MODE mode)
{
    UINT32 file = 0, script;

    if (mode == NVO_PARSE_FILE) {
        file = make(p, NVO_NODE_FILE, 0, 0);
        if (!file)
            return 0;
    }
    skip_separators(p);
    if (mode == NVO_PARSE_FILE) {
        UINT32 end = run_end(p, p->pos, READ_COMMAND);

        if (is_text(p, p->pos, end, "needs")) {
            UINT32 needs = parse_needs(p, p->pos);

            if (!needs)
                return 0;
            adopt(p, file, needs);
        }
    }
    script = parse_script(p);
    if (!script)
        return 0;
    if (p->pos < p->n)
        return unexpected(p);
    if (!file)
        return script;
    adopt(p, file, script);
    p->end = p->n;
    return seal(p, file);
}

NVO_STATUS nvo_parse(NVO_TREE *tree, NVO_PARSE_MODE mode, const UINT8 *source,
                     UINT64 length, NVO_NODE *nodes, UINT32 capacity)
{
    PARSER p;
    UINT64 offset;

    tree->nodes = nodes;
    tree->capacity = capacity;
    tree->count = 1;
    tree->root = 0;
    tree->error = NVO_ERROR_NONE;
    tree->error_offset = 0;
    if (capacity < 1 || length > NVO_SOURCE_MAX)
        return NVO_MEMORY;
    nodes[0].kind = NVO_NODE_NONE;
    nodes[0].flags = 0;
    nodes[0].first = 0;
    nodes[0].last = 0;
    nodes[0].next = 0;
    nodes[0].parent = 0;
    nodes[0].offset = 0;
    nodes[0].length = 0;
    if (!nvo_utf8_check(source, length, &offset)) {
        tree->error = NVO_ERROR_UTF8;
        tree->error_offset = offset;
        return NVO_SYNTAX;
    }
    p.s = source;
    p.n = (UINT32)length;
    p.pos = 0;
    p.end = 0;
    p.tree = tree;
    p.depth = 0;
    p.blank_lines = 0;
    p.status = NVO_OK;
    p.error = NVO_ERROR_NONE;
    p.error_at = 0;
    tree->root = parse_file(&p, mode);
    if (p.status == NVO_SYNTAX) {
        tree->error = p.error;
        tree->error_offset = p.error_at;
    }
    return p.status;
}

/* ------------------------------------------------------------------------
   The printer

   It walks the tree without recursing: a left-leaning chain of
   operators is as deep as a source is long, and the parser's depth limit
   does not bound it. Each node has a parent, so the walk needs no stack.
   Every node writes some text as the walk enters it, between its
   children, and as the walk leaves it; a leaf does the first and the
   last. */

typedef struct {
    const NVO_TREE *tree;
    const UINT8 *source;
    UINT8 *out;
    UINT64 capacity;
    UINT64 length; /* the bytes the output holds, counted past capacity */
} PRINTER;

int nvo_node_has_text(const NVO_NODE *node)
{
    switch ((NVO_NODE_KIND)node->kind) {
    case NVO_NODE_NAME:
    case NVO_NODE_DEFNAME:
    case NVO_NODE_BARE:
    case NVO_NODE_PATH:
    case NVO_NODE_GLOB:
    case NVO_NODE_STRING_SQ:
    case NVO_NODE_TEXT:
    case NVO_NODE_VARIABLE:
    case NVO_NODE_INT:
    case NVO_NODE_DECIMAL:
    case NVO_NODE_SIZE:
    case NVO_NODE_DURATION:
    case NVO_NODE_BYTES:
    case NVO_NODE_TRUE:
    case NVO_NODE_FALSE:
    case NVO_NODE_NOTHING:
    case NVO_NODE_TYPE:
    case NVO_NODE_SHORT:
    case NVO_NODE_FLAG:
        return 1;
    case NVO_NODE_MEMBER:
        return node->flags != NVO_MEMBER_STRING;
    default:
        return 0;
    }
}

static void put_byte(PRINTER *r, UINT32 c)
{
    if (r->length < r->capacity)
        r->out[r->length] = (UINT8)c;
    r->length++;
}

static void put(PRINTER *r, const char *text)
{
    while (*text != '\0')
        put_byte(r, (UINT8)*text++);
}

static void put_text(PRINTER *r, const NVO_NODE *node)
{
    UINT32 i;

    for (i = 0; i < node->length; i++)
        put_byte(r, r->source[node->offset + i]);
}

static const NVO_NODE *node_of(const PRINTER *r, UINT32 index)
{
    return &r->tree->nodes[index];
}

/* Whether a script is a file's or the prompt's whole input, not the body of
   a block. */
static int is_top_script(const PRINTER *r, const NVO_NODE *script)
{
    return script->parent == 0 ||
           node_of(r, script->parent)->kind == NVO_NODE_FILE;
}

/* The text a binary operator is written as, with its blanks. */
static const char *operator_text(UINT32 kind)
{
    switch ((NVO_NODE_KIND)kind) {
    case NVO_NODE_OR:
        return " or ";
    case NVO_NODE_AND:
        return " and ";
    case NVO_NODE_EQ:
        return " == ";
    case NVO_NODE_NE:
        return " != ";
    case NVO_NODE_LT:
        return " < ";
    case NVO_NODE_LE:
        return " <= ";
    case NVO_NODE_GT:
        return " > ";
    case NVO_NODE_GE:
        return " >= ";
    case NVO_NODE_IN:
        return " in ";
    case NVO_NODE_NOT_IN:
        return " not-in ";
    case NVO_NODE_CONCAT:
        return " ++ ";
    case NVO_NODE_ADD:
        return " + ";
    case NVO_NODE_SUB:
        return " - ";
    case NVO_NODE_MUL:
        return " * ";
    case NVO_NODE_DIV:
        return " / ";
    case NVO_NODE_IDIV:
        return " // ";
    case NVO_NODE_MOD:
        return " % ";
    default:
        return 0;
    }
}

static const char *set_text(UINT32 flags)
{
    switch (flags) {
    case NVO_SET_ADD:
        return " += ";
    case NVO_SET_SUBTRACT:
        return " -= ";
    case NVO_SET_CONCAT:
        return " ++= ";
    default:
        return " = ";
    }
}

static void enter(PRINTER *r, const NVO_NODE *n)
{
    switch ((NVO_NODE_KIND)n->kind) {
    case NVO_NODE_NEEDS:
        put(r, "needs ");
        break;
    case NVO_NODE_BLOCK:
        put(r, "{ ");
        break;
    case NVO_NODE_CLOSURE:
        put(r, "{ |");
        break;
    case NVO_NODE_PARAMS:
        if (node_of(r, n->parent)->kind == NVO_NODE_DEF)
            put(r, "(");
        break;
    case NVO_NODE_LET:
        put(r, n->flags & NVO_FLAG_EXPORT ? "export let " : "let ");
        break;
    case NVO_NODE_VAR:
        put(r, "var ");
        break;
    case NVO_NODE_SET:
        put(r, "set ");
        break;
    case NVO_NODE_DEF:
        put(r, n->flags & NVO_FLAG_EXPORT ? "export def " : "def ");
        break;
    case NVO_NODE_USE:
        put(r, "use ");
        break;
    case NVO_NODE_IF:
        put(r, "if ");
        break;
    case NVO_NODE_ELIF:
        put(r, "else if ");
        break;
    case NVO_NODE_ELSE:
        put(r, "else ");
        break;
    case NVO_NODE_FOR:
        put(r, "for ");
        break;
    case NVO_NODE_WHILE:
        put(r, "while ");
        break;
    case NVO_NODE_TRY:
        put(r, "try ");
        break;
    case NVO_NODE_CATCH:
        put(r, "catch ");
        break;
    case NVO_NODE_BREAK:
        put(r, "break");
        break;
    case NVO_NODE_CONTINUE:
        put(r, "continue");
        break;
    case NVO_NODE_RETURN:
        put(r, n->first != 0 ? "return " : "return");
        break;
    case NVO_NODE_FLAG_PARAM:
        put(r, "--");
        break;
    case NVO_NODE_REST_PARAM:
        put(r, "...");
        break;
    case NVO_NODE_SHORT:
        put(r, "(-");
        put_text(r, n);
        put(r, ")");
        break;
    case NVO_NODE_TYPE:
        put_text(r, n);
        if (n->flags & NVO_FLAG_READ)
            put(r, "(read)");
        if (n->flags & NVO_FLAG_WRITE)
            put(r, "(write)");
        if (n->first != 0)
            put(r, "<");
        break;
    case NVO_NODE_COMMAND:
        if (n->flags & NVO_FLAG_CARET)
            put(r, "^");
        break;
    case NVO_NODE_GRANT:
        put(r, "=> ");
        break;
    case NVO_NODE_SPREAD:
        put(r, "...");
        break;
    case NVO_NODE_STRING_DQ:
        put(r, "\"");
        break;
    case NVO_NODE_VARIABLE:
        put(r, n->flags & NVO_FLAG_BRACED ? "${" : "$");
        put_text(r, n);
        break;
    case NVO_NODE_MEMBER:
        put(r, ".");
        if (n->flags != NVO_MEMBER_STRING)
            put_text(r, n);
        break;
    case NVO_NODE_SUBST:
        put(r, "$(");
        break;
    case NVO_NODE_GROUP:
        put(r, "(");
        break;
    case NVO_NODE_LIST:
    case NVO_NODE_TABLE:
    case NVO_NODE_COLUMNS:
    case NVO_NODE_ROW:
        put(r, "[");
        break;
    case NVO_NODE_RECORD:
        put(r, "{");
        break;
    case NVO_NODE_NOT:
        put(r, "not ");
        break;
    case NVO_NODE_NEG:
        put(r, "-");
        break;
    default:
        if (nvo_node_has_text(n))
            put_text(r, n);
        break;
    }
}

static void leave(PRINTER *r, const NVO_NODE *n)
{
    switch ((NVO_NODE_KIND)n->kind) {
    case NVO_NODE_BLOCK:
    case NVO_NODE_CLOSURE:
        put(r, " }");
        break;
    case NVO_NODE_PARAMS:
        if (node_of(r, n->parent)->kind == NVO_NODE_DEF)
            put(r, ")");
        break;
    case NVO_NODE_SCRIPT:
        if (is_top_script(r, n) && n->first != 0)
            put(r, "\n");
        break;
    case NVO_NODE_TYPE:
        if (n->first != 0)
            put(r, ">");
        break;
    case NVO_NODE_STRING_DQ:
        put(r, "\"");
        break;
    case NVO_NODE_VARIABLE:
        if (n->flags & NVO_FLAG_BRACED)
            put(r, "}");
        else if (n->flags & NVO_FLAG_OPTIONAL)
            put(r, "?");
        break;
    case NVO_NODE_ACCESS:
        if (n->flags & NVO_FLAG_OPTIONAL)
            put(r, "?");
        break;
    case NVO_NODE_SUBST:
    case NVO_NODE_GROUP:
        put(r, ")");
        break;
    case NVO_NODE_LIST:
    case NVO_NODE_TABLE:
    case NVO_NODE_COLUMNS:
    case NVO_NODE_ROW:
        put(r, "]");
        break;
    case NVO_NODE_RECORD:
        put(r, "}");
        break;
    default:
        break;
    }
}

/* The text between two children of parent. */
static void between(PRINTER *r, const NVO_NODE *parent, const NVO_NODE *prev,
                    const NVO_NODE *next)
{
    const char *op = operator_text(parent->kind);

    if (op) {
        put(r, op);
        return;
    }
    switch ((NVO_NODE_KIND)parent->kind) {
    case NVO_NODE_FILE:
        put(r, "\n");
        break;
    case NVO_NODE_SCRIPT:
        put(r, is_top_script(r, parent) ? "\n" : "; ");
        break;
    case NVO_NODE_CLOSURE:
        put(r, "| ");
        break;
    case NVO_NODE_PARAMS:
    case NVO_NODE_LIST:
    case NVO_NODE_RECORD:
    case NVO_NODE_COLUMNS:
    case NVO_NODE_ROW:
    case NVO_NODE_TYPE:
        put(r, ", ");
        break;
    case NVO_NODE_LET:
    case NVO_NODE_VAR:
        put(r, next->kind == NVO_NODE_TYPE ? ": " : " = ");
        break;
    case NVO_NODE_SET:
        if (next->kind != NVO_NODE_MEMBER)
            put(r, set_text(parent->flags));
        break;
    case NVO_NODE_DEF:
        if (next->kind == NVO_NODE_TYPE)
            put(r, " -> ");
        else if (next->kind == NVO_NODE_BLOCK)
            put(r, " ");
        break;
    case NVO_NODE_PARAM:
        if (next->kind == NVO_NODE_TYPE)
            put(r, parent->flags & NVO_FLAG_OPTIONAL ? "?: " : ": ");
        else
            put(r, " = ");
        break;
    case NVO_NODE_FLAG_PARAM:
        if (next->kind == NVO_NODE_TYPE)
            put(r, ": ");
        else if (next->kind != NVO_NODE_SHORT)
            put(r, " = ");
        break;
    case NVO_NODE_REST_PARAM:
    case NVO_NODE_FIELD_TYPE:
    case NVO_NODE_ENTRY:
        put(r, ": ");
        break;
    case NVO_NODE_USE:
        put(r, " as ");
        break;
    case NVO_NODE_IF:
    case NVO_NODE_ELIF:
    case NVO_NODE_WHILE:
    case NVO_NODE_TRY:
    case NVO_NODE_CATCH:
    case NVO_NODE_COMMAND:
        put(r, " ");
        break;
    case NVO_NODE_FOR:
        put(r, next->kind == NVO_NODE_BLOCK ? " " : " in ");
        break;
    case NVO_NODE_PIPELINE:
        put(r, " | ");
        break;
    case NVO_NODE_TABLE:
        put(r, prev->kind == NVO_NODE_COLUMNS ? "; " : ", ");
        break;
    default:
        break;
    }
}

/* Walk the tree from its root, writing each node's text as the walk
   enters it, goes between its children and leaves it. */
static void print_tree(PRINTER *r)
{
    UINT32 cur = r->tree->root;

    for (;;) {
        const NVO_NODE *node = node_of(r, cur);

        enter(r, node);
        if (node->first != 0) {
            cur = node->first;
            continue;
        }
        leave(r, node);
        for (;;) {
            node = node_of(r, cur);
            if (cur == r->tree->root)
                return;
            if (node->next != 0) {
                between(r, node_of(r, node->parent), node,
                        node_of(r, node->next));
                cur = node->next;
                break;
            }
            cur = node->parent;
            leave(r, node_of(r, cur));
        }
    }
}

NVO_STATUS nvo_print(const NVO_TREE *tree, const UINT8 *source, UINT8 *out,
                     UINT64 capacity, UINT64 *written)
{
    PRINTER r;

    r.tree = tree;
    r.source = source;
    r.out = out;
    r.capacity = capacity;
    r.length = 0;
    *written = 0;
    if (tree->root == 0)
        return NVO_INVALID;
    print_tree(&r);
    *written = r.length;
    return r.length <= capacity ? NVO_OK : NVO_MEMORY;
}
