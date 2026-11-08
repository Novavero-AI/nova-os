/* mkinterface: reads one interface description (interfaces/NAME.desc) and
   writes the C header it describes (include/NAME_protocol.h).

   Usage: mkinterface DESCRIPTION HEADER

   Implements docs/spec/interface.md: the lines and words of P2, the
   grammar of P3, constants of P4, imports of P5, layout of P6, channels
   and messages of P7, tails of P8, handles of P9, the checks of P10,
   settings of P12, descriptions of P13 and the header of P14, with the
   command line of P15. A description is checked whole against every rule
   before anything is written; the header is built in memory and then
   written in one piece, so a refused description leaves no file.

   Exit status: 0 on success; 1 with one line, "DESCRIPTION:LINE: what is
   wrong", on standard error; 2 with the usage for the wrong arguments.

   The header is a function of the description's bytes and its file name
   alone. Numbers are formatted here and every comparison is made in
   64-bit unsigned arithmetic, so no host's printf, locale or integer
   width shows in the output. The program reads and writes through
   <stdio.h> only, in binary mode. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long U64;
typedef long long I64;

/* ------------------------------------------------------------------------
   Limits. The language's own limits are named for the rule that gives
   them; the others are this tool's tables, and a description that fills
   one is refused with the table's name. */

/* P15: the most bytes of a description. */
#define DESCRIPTION_MAX 1048576u
/* The most bytes of one line. */
#define LINE_BYTES_MAX 4096u
/* The most words of one line; an outline's contour is the longest. */
#define LINE_WORDS_MAX 1024u
/* P2: the longest name and the longest C name, in characters. */
#define NAME_CHARS 31u
#define CNAME_CHARS 63u
/* P14: the longest name the generator makes, which is a C name's. */
#define GENERATED_CHARS 63u
/* The longest text of a Value as the header spells it. */
#define VALUE_TEXT_CHARS 80u
/* The longest path, as its components joined by dots. */
#define PATH_CHARS 160u

#define SYMBOLS_MAX 4096u
#define STRUCTS_MAX 256u
#define FIELDS_MAX 2048u
#define FIELDS_PER_STRUCT_MAX 96u
#define IMPORTS_MAX 16u
#define CHANNELS_MAX 32u
#define MESSAGES_MAX 256u
#define TAILS_PER_MESSAGE_MAX 8u
#define HANDLE_LINES_PER_MESSAGE_MAX 16u
#define FIELD_LINES_PER_MESSAGE_MAX 16u
#define IN_VALUES_MAX 16u
#define SETTINGS_MAX 128u
#define RIGHTS_PER_SPEC_MAX 8u
#define ALLOWED_WORDS_MAX 32u
#define CONTOURS_MAX 16u
#define POINTS_MAX 512u
/* Every local name the generated functions declare, so that a tail or
   field named for one is refused (P3, rule 2). */
#define LOCALS_MAX 32u

/* P4, rule 5, and ipc.md I1: the channel's limits the generator knows. */
#define CHANNEL_MAX_BYTES_VALUE 65536u
#define CHANNEL_MAX_HANDLES_VALUE 64u
#define CHANNEL_MAX_CAPACITY_VALUE 1024u

/* P14: a dispatcher's `which` for a message too short to hold its
   selector, and the one selector value no message takes (P7, rule 2). */
#define UNNAMED_VALUE 0xFFFFFFFFu

/* P4, rule 2: the largest value a C int holds. A level of an expression
   whose operands and results all stay within it is what C's int arithmetic
   computes as the generator does; a level that passes it starts with
   WIDEN_PREFIX, so C computes it in unsigned 64-bit arithmetic as the
   generator does. */
#define C_INT_MAX_VALUE 2147483647u
#define WIDEN_PREFIX "1ULL * "

/* P12: an outline's coordinates lie within 2^24 of 0. */
#define OUTLINE_COORDINATE_LIMIT 16777216

/* P12: a material's amount is a percent. */
#define PERCENT_MAX 100u

/* P6: a header's assertion message for every layout. */
#define LAYOUT_MESSAGE "one layout on every target"

/* A column the generated declarations wrap their lines before. */
#define WRAP_COLUMN 76u

/* The generated code's indentation, P14 rule 7. */
#define INDENT 4u

/* The refusal of a C name the generated functions take from native_abi.h
   (P3, rule 2). */
#define KERNEL_NAME_REFUSED \
    "a name the generated functions take from native_abi.h"

/* ------------------------------------------------------------------------
   Reporting. Every refusal ends the program with one line naming the
   line of the description it is about. */

static const char *description_path = "";

/* Print "DESCRIPTION:LINE: what" and exit 1; line 0 names no line. */
static void fail(unsigned line, const char *what)
{
    fprintf(stderr, "%s:%u: %s\n", description_path, line, what);
    exit(1);
}

/* fail with a name appended, as "what: name". */
static void fail_name(unsigned line, const char *what, const char *name)
{
    fprintf(stderr, "%s:%u: %s: %s\n", description_path, line, what, name);
    exit(1);
}

/* Allocate bytes, or refuse the description for want of memory. */
static void *xalloc(size_t bytes)
{
    void *p = malloc(bytes ? bytes : 1u);

    if (!p)
        fail(0, "out of memory");
    return p;
}

/* A table of count entries of size bytes, with its capacity named in the
   refusal when it is full. */
#define TABLE_FULL(line, what) fail((line), "too many " what)

/* ------------------------------------------------------------------------
   Text buffers, which hold the header as it is built. */

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} Buf;

/* Make room in b for extra more bytes and the NUL that ends them. */
static void buf_reserve(Buf *b, size_t extra)
{
    size_t need = b->length + extra + 1u;

    if (need > b->capacity) {
        size_t capacity = b->capacity ? b->capacity : 256u;
        char *grown;

        while (capacity < need)
            capacity *= 2u;
        grown = (char *)xalloc(capacity);
        if (b->length)
            memcpy(grown, b->data, b->length);
        free(b->data);
        b->data = grown;
        b->capacity = capacity;
    }
}

/* Append length bytes of text to b. */
static void buf_putn(Buf *b, const char *text, size_t length)
{
    buf_reserve(b, length);
    memcpy(b->data + b->length, text, length);
    b->length += length;
    b->data[b->length] = '\0';
}

/* Append the NUL-terminated text to b. */
static void buf_put(Buf *b, const char *text)
{
    buf_putn(b, text, strlen(text));
}

/* Append the character c to b. */
static void buf_ch(Buf *b, char c)
{
    buf_putn(b, &c, 1u);
}

/* Append value in decimal. */
static void buf_u64(Buf *b, U64 value)
{
    char digits[24];
    unsigned n = 0;

    do {
        digits[n++] = (char)('0' + (unsigned)(value % 10u));
        value /= 10u;
    } while (value);
    while (n)
        buf_ch(b, digits[--n]);
}

/* Append value in decimal, with a minus sign if it is negative. */
static void buf_i64(Buf *b, I64 value)
{
    if (value < 0) {
        buf_ch(b, '-');
        buf_u64(b, (U64)0 - (U64)value);
    } else {
        buf_u64(b, (U64)value);
    }
}

/* Append count spaces to b. */
static void buf_spaces(Buf *b, unsigned count)
{
    while (count--)
        buf_ch(b, ' ');
}

/* The column the next character lands in: the characters since the last
   LF. */
static size_t buf_column(const Buf *b)
{
    size_t n = 0;

    while (n < b->length && b->data[b->length - 1u - n] != '\n')
        n++;
    return n;
}

/* ------------------------------------------------------------------------
   Characters and words. The description is ASCII, so the classes are
   written out and no locale's is used (P2). */

static int is_lower(int c)
{
    return c >= 'a' && c <= 'z';
}

/* Whether c is an ASCII upper-case letter. */
static int is_upper(int c)
{
    return c >= 'A' && c <= 'Z';
}

/* Whether c is an ASCII decimal digit. */
static int is_digit(int c)
{
    return c >= '0' && c <= '9';
}

/* Whether c is an ASCII hexadecimal digit. */
static int is_hex_digit(int c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* One word of a line: a span of the description's bytes. */
typedef struct {
    const char *text;
    unsigned length;
} Word;

/* One line of the description. */
typedef struct {
    const char *text;
    unsigned length;
    unsigned first_word;
    unsigned word_count;
    int has_comment;
    const char *comment;
    unsigned comment_length;
} Line;

static char *description_text;
static Line *lines;
static unsigned line_count;
static Word *words;
static unsigned word_total;

/* Whether w is exactly the text literal. */
static int word_is(const Word *w, const char *literal)
{
    size_t n = strlen(literal);

    return w->length == n && memcmp(w->text, literal, n) == 0;
}

/* Copy a word into a NUL-terminated array of capacity bytes. */
static void word_copy(const Word *w, char *to, unsigned capacity)
{
    if (w->length + 1u > capacity)
        fail(0, "a word is longer than a table holds");
    memcpy(to, w->text, w->length);
    to[w->length] = '\0';
}

/* P2, rule 5: whether w is a Name, `[a-z][a-z0-9_]*` of at most 31
   characters. */
static int is_name(const Word *w)
{
    unsigned i;

    if (w->length == 0 || w->length > NAME_CHARS || !is_lower(w->text[0]))
        return 0;
    for (i = 1; i < w->length; i++)
        if (!is_lower(w->text[i]) && !is_digit(w->text[i]) &&
            w->text[i] != '_')
            return 0;
    return 1;
}

/* A C name, `[A-Z][A-Z0-9_]*` of at most 63 characters. */
static int is_cname(const Word *w)
{
    unsigned i;

    if (w->length == 0 || w->length > CNAME_CHARS || !is_upper(w->text[0]))
        return 0;
    for (i = 1; i < w->length; i++)
        if (!is_upper(w->text[i]) && !is_digit(w->text[i]) &&
            w->text[i] != '_')
            return 0;
    return 1;
}

/* A path: Names joined by dots. */
static int is_path(const Word *w)
{
    unsigned start = 0, i;

    if (w->length == 0 || w->length > PATH_CHARS)
        return 0;
    for (i = 0; i <= w->length; i++)
        if (i == w->length || w->text[i] == '.') {
            Word part;

            part.text = w->text + start;
            part.length = i - start;
            if (!is_name(&part))
                return 0;
            start = i + 1u;
        }
    return 1;
}

/* A setting name: words of `[a-z][a-z0-9-]*` joined by dots. */
static int is_setting_name(const Word *w)
{
    unsigned i, start = 0, j;

    if (w->length == 0 || w->length > PATH_CHARS)
        return 0;
    for (i = 0; i <= w->length; i++)
        if (i == w->length || w->text[i] == '.') {
            if (i == start || !is_lower(w->text[start]))
                return 0;
            for (j = start; j < i; j++)
                if (!is_lower(w->text[j]) && !is_digit(w->text[j]) &&
                    w->text[j] != '-')
                    return 0;
            start = i + 1u;
        }
    return 1;
}

/* A setting's word, `[a-z][a-z0-9-]*`, as `allowed` and an enum's
   default take. */
static int is_setting_word(const Word *w)
{
    unsigned i;

    if (w->length == 0 || w->length > NAME_CHARS || !is_lower(w->text[0]))
        return 0;
    for (i = 1; i < w->length; i++)
        if (!is_lower(w->text[i]) && !is_digit(w->text[i]) &&
            w->text[i] != '-')
            return 0;
    return 1;
}

/* P2, rule 5: a Number, `0`, `[1-9][0-9]*` or `0x[0-9A-Fa-f]+`, at most
   2^64 - 1, and a decimal at most 2^63 - 1. Returns 1 and the value in
   *value, or 0. */
static int word_number(const Word *w, U64 *value)
{
    U64 v = 0;
    unsigned i;

    if (w->length == 0)
        return 0;
    if (w->length > 2u && w->text[0] == '0' && w->text[1] == 'x') {
        for (i = 2; i < w->length; i++) {
            unsigned digit;
            char c = w->text[i];

            if (!is_hex_digit(c))
                return 0;
            if (v >> 60)
                return 0;
            digit = is_digit(c) ? (unsigned)(c - '0')
                                : (unsigned)((c | 0x20) - 'a') + 10u;
            v = v << 4 | digit;
        }
        *value = v;
        return 1;
    }
    if (w->text[0] == '0' && w->length > 1u)
        return 0;
    for (i = 0; i < w->length; i++) {
        unsigned digit;

        if (!is_digit(w->text[i]))
            return 0;
        digit = (unsigned)(w->text[i] - '0');
        if (v > (0x7FFFFFFFFFFFFFFFull - digit) / 10u)
            return 0;
        v = v * 10u + digit;
    }
    *value = v;
    return 1;
}

/* A Signed number: `-` and a decimal number. */
static int word_signed(const Word *w, I64 *value)
{
    Word rest;
    U64 magnitude;

    if (w->length < 2u || w->text[0] != '-')
        return 0;
    rest.text = w->text + 1;
    rest.length = w->length - 1u;
    if (rest.length > 1u && rest.text[0] == '0' && rest.text[1] == 'x')
        return 0;
    if (!word_number(&rest, &magnitude))
        return 0;
    *value = -(I64)magnitude;
    return 1;
}

/* A Number or a Signed number. */
static int word_integer(const Word *w, I64 *value)
{
    U64 v;

    if (word_signed(w, value))
        return 1;
    if (!word_number(w, &v) || v > 0x7FFFFFFFFFFFFFFFull)
        return 0;
    *value = (I64)v;
    return 1;
}

/* A Point: two decimal numbers, each with or without a leading `-`,
   joined by a comma. */
static int word_point(const Word *w, I64 *x, I64 *y)
{
    unsigned comma = 0, i;
    Word a, b;

    for (i = 0; i < w->length; i++)
        if (w->text[i] == ',') {
            if (comma)
                return 0;
            comma = i;
        }
    if (comma == 0)
        return 0;
    a.text = w->text;
    a.length = comma;
    b.text = w->text + comma + 1u;
    b.length = w->length - comma - 1u;
    if (a.length > 1u && a.text[0] == '0' && a.text[1] == 'x')
        return 0;
    if (b.length > 1u && b.text[0] == '0' && b.text[1] == 'x')
        return 0;
    return word_integer(&a, x) && word_integer(&b, y);
}

/* Whether w is a string: two quotes and what they enclose. */
static int word_is_string(const Word *w)
{
    return w->length >= 2u && w->text[0] == '"' &&
           w->text[w->length - 1u] == '"';
}

/* ------------------------------------------------------------------------
   Reading the description (P2): whole, ASCII, LF-ended, split into
   lines and words. */

/* Read path whole, opened "rb", at most DESCRIPTION_MAX bytes, into
   description_text. Returns its length. */
static size_t read_description(const char *path)
{
    FILE *f = fopen(path, "rb");
    size_t length = 0, got;

    if (!f) {
        fprintf(stderr, "mkinterface: cannot read %s\n", path);
        exit(1);
    }
    description_text = (char *)xalloc(DESCRIPTION_MAX + 1u);
    while ((got = fread(description_text + length, 1u,
                        DESCRIPTION_MAX + 1u - length, f)) > 0)
        length += got;
    if (ferror(f)) {
        fprintf(stderr, "mkinterface: cannot read %s\n", path);
        exit(1);
    }
    fclose(f);
    if (length > DESCRIPTION_MAX)
        fail(0, "the description is longer than DESCRIPTION_MAX bytes");
    description_text[length] = '\0';
    return length;
}

/* Split the description into its lines and each line into words and a
   comment. P2: the bytes 20h to 7Eh and an LF ending every line; `#`
   starts a comment unless inside a string; `(` and `)` are words of
   their own; a string is `"`, printable ASCII other than `"` and `\`,
   `"`. */
static void split_lines(size_t length)
{
    size_t at = 0, i;
    unsigned number = 0;

    for (i = 0; i < length; i++) {
        unsigned char c = (unsigned char)description_text[i];

        if (c == '\n')
            line_count++;
        else if (c < 0x20u || c > 0x7Eu) {
            unsigned n = 1;
            size_t j;

            for (j = 0; j < i; j++)
                if (description_text[j] == '\n')
                    n++;
            fail(n, "a byte that is not printable ASCII or LF");
        }
    }
    if (length && description_text[length - 1u] != '\n')
        fail(line_count + 1u, "the last line does not end in LF");
    lines = (Line *)xalloc(sizeof(Line) * (line_count + 1u));
    words = (Word *)xalloc(sizeof(Word) * (length + 1u));
    while (at < length) {
        size_t end = at;
        Line *line = &lines[number];
        size_t p;

        memset(line, 0, sizeof *line);
        while (description_text[end] != '\n')
            end++;
        number++;
        if (end - at > LINE_BYTES_MAX)
            fail(number, "a line is longer than LINE_BYTES_MAX bytes");
        line->text = description_text + at;
        line->length = (unsigned)(end - at);
        line->first_word = word_total;
        p = at;
        while (p < end) {
            char c = description_text[p];

            if (c == ' ') {
                p++;
            } else if (c == '#') {
                line->has_comment = 1;
                line->comment = description_text + p + 1u;
                line->comment_length = (unsigned)(end - p - 1u);
                if (line->comment_length && line->comment[0] == ' ') {
                    line->comment++;
                    line->comment_length--;
                }
                break;
            } else if (c == '(' || c == ')') {
                words[word_total].text = description_text + p;
                words[word_total].length = 1;
                word_total++;
                p++;
            } else if (c == '"') {
                size_t q = p + 1u;

                while (q < end && description_text[q] != '"') {
                    if (description_text[q] == '\\')
                        fail(number, "a string holds a backslash");
                    q++;
                }
                if (q >= end)
                    fail(number, "a string is not closed");
                words[word_total].text = description_text + p;
                words[word_total].length = (unsigned)(q - p + 1u);
                word_total++;
                p = q + 1u;
            } else {
                size_t q = p;

                while (q < end && description_text[q] != ' ' &&
                       description_text[q] != '(' &&
                       description_text[q] != ')' &&
                       description_text[q] != '#')
                    q++;
                words[word_total].text = description_text + p;
                words[word_total].length = (unsigned)(q - p);
                word_total++;
                p = q;
            }
            if (word_total - line->first_word > LINE_WORDS_MAX)
                fail(number, "a line has more words than LINE_WORDS_MAX");
        }
        line->word_count = word_total - line->first_word;
        at = end + 1u;
    }
}

/* A line is 1-based in messages and 0-based here. */
static const Word *line_words(unsigned line)
{
    return &words[lines[line].first_word];
}

/* Whether line, counted from 0, holds a comment and nothing else. */
static int line_comment_only(unsigned line)
{
    return lines[line].word_count == 0 && lines[line].has_comment;
}

/* ------------------------------------------------------------------------
   Descriptions (P2, rule 6, and P13). */

/* The description of a declaration: the comment lines directly above it
   (a run of lines, [first, first + count)), or the comment on its own
   line. */
typedef struct {
    unsigned first;
    unsigned count;
    const char *trail;
    unsigned trail_length;
} Desc;

/* The description of a generated declaration that has none of its own. */
static const Desc NO_DESC = {0, 0, 0, 0};

/* Whether d holds a description. */
static int desc_present(const Desc *d)
{
    return d->count > 0 || d->trail_length > 0;
}

/* The trigraph for a backslash, which at the end of a comment line a
   compiler that reads trigraphs takes for a line continuation (P2, rule
   6). */
#define BACKSLASH_TRIGRAPH "?\?/"
#define BACKSLASH_TRIGRAPH_CHARS 3u
#define COMMENT_BROKEN                                                     \
    "a description holds a comment delimiter or ends in the trigraph ?\?/"

/* Whether text holds a comment delimiter, which would end or start a
   comment in the header, or ends in the trigraph for a backslash. */
static int breaks_comment(const char *text, unsigned length)
{
    unsigned i;

    for (i = 0; i + 1u < length; i++)
        if ((text[i] == '*' && text[i + 1u] == '/') ||
            (text[i] == '/' && text[i + 1u] == '*'))
            return 1;
    return length >= BACKSLASH_TRIGRAPH_CHARS &&
           memcmp(text + length - BACKSLASH_TRIGRAPH_CHARS,
                  BACKSLASH_TRIGRAPH, BACKSLASH_TRIGRAPH_CHARS) == 0;
}

/* The description of the statement on line: the comment lines directly
   above it with no blank line between, or its own trailing comment; a
   statement with both, or whose description holds a comment delimiter,
   is refused. */
static Desc take_desc(unsigned line)
{
    Desc d;
    unsigned above = 0, i;

    d.first = line;
    d.count = 0;
    d.trail = 0;
    d.trail_length = 0;
    while (above < line && line_comment_only(line - above - 1u))
        above++;
    d.first = line - above;
    d.count = above;
    if (lines[line].word_count && lines[line].has_comment &&
        lines[line].comment_length) {
        d.trail = lines[line].comment;
        d.trail_length = lines[line].comment_length;
    }
    if (d.count && d.trail_length)
        fail(line + 1u, "a declaration with a description above it and one "
                        "on its own line");
    for (i = 0; i < d.count; i++)
        if (breaks_comment(lines[d.first + i].comment,
                           lines[d.first + i].comment_length))
            fail(line + 1u, COMMENT_BROKEN);
    if (breaks_comment(d.trail, d.trail_length))
        fail(line + 1u, COMMENT_BROKEN);
    return d;
}

/* A described declaration's description, refused when it has none
   (P13, rule 1). */
static Desc need_desc(unsigned line)
{
    Desc d = take_desc(line);

    if (!desc_present(&d))
        fail(line + 1u, "a declaration without a description");
    return d;
}

/* ------------------------------------------------------------------------
   Scalars, values and symbols. */

typedef enum { SC_U8, SC_U16, SC_U32, SC_U64, SC_I32, SC_I64 } Scalar;

/* P6, rule 1: the scalar types and the types.h names they stand for. */
static const struct {
    const char *name;
    const char *ctype;
    unsigned size;
    int is_signed;
} SCALARS[] = {{"u8", "UINT8", 1, 0},  {"u16", "UINT16", 2, 0},
               {"u32", "UINT32", 4, 0}, {"u64", "UINT64", 8, 0},
               {"i32", "INT32", 4, 1},  {"i64", "INT64", 8, 1}};
#define SCALAR_COUNT 6u

/* C's keywords, which no field or tail is named for (P3, rule 2). */
static const char *const C_KEYWORDS[] = {
    "auto",     "break",  "case",   "char",   "const",    "continue",
    "default",  "do",     "double", "else",   "enum",     "extern",
    "float",    "for",    "goto",   "if",     "inline",   "int",
    "long",     "register", "restrict", "return", "short", "signed",
    "sizeof",   "static", "struct", "switch", "typedef",  "union",
    "unsigned", "void",   "volatile", "while"};

/* The scalar a word names, or -1. */
static int scalar_of(const Word *w)
{
    unsigned i;

    for (i = 0; i < SCALAR_COUNT; i++)
        if (word_is(w, SCALARS[i].name))
            return (int)i;
    return -1;
}

/* The largest value a scalar holds: unsigned bits, or for a signed type
   the largest positive. */
static U64 scalar_max(Scalar s)
{
    unsigned bits = SCALARS[s].size * 8u;
    U64 all = bits == 64u ? ~(U64)0 : ((U64)1 << bits) - 1u;

    return SCALARS[s].is_signed ? all >> 1 : all;
}

/* A value as a description writes it: its number, and the text the
   header spells it with (P14, rule 7). */
typedef struct {
    U64 number;
    char text[VALUE_TEXT_CHARS];
} Value;

typedef enum {
    SYM_NUMBER,
    SYM_STRING,
    SYM_RIGHTS,
    SYM_STRUCT
} SymbolKind;

/* A C name: one namespace for the whole description (P3, rule 2). */
typedef struct {
    char name[CNAME_CHARS + 1u];
    SymbolKind kind;
    U64 value;
    unsigned index;
    unsigned line;
} Symbol;

static Symbol symbols[SYMBOLS_MAX];
static unsigned symbol_count;

/* The C name declared as name, or 0. */
static Symbol *find_symbol(const char *name)
{
    unsigned i;

    for (i = 0; i < symbol_count; i++)
        if (strcmp(symbols[i].name, name) == 0)
            return &symbols[i];
    return 0;
}

/* Record a C name, refused if it is not one, is already declared or a
   table is full. */
static Symbol *put_symbol(unsigned line, const Word *w, SymbolKind kind)
{
    Symbol *s;

    if (!is_cname(w))
        fail(line + 1u, "a C name is [A-Z][A-Z0-9_]* of at most 63 "
                        "characters");
    if (symbol_count >= SYMBOLS_MAX)
        TABLE_FULL(line + 1u, "C names");
    s = &symbols[symbol_count];
    word_copy(w, s->name, sizeof s->name);
    if (find_symbol(s->name))
        fail_name(line + 1u, "a C name declared twice", s->name);
    s->kind = kind;
    s->value = 0;
    s->index = 0;
    s->line = line + 1u;
    symbol_count++;
    return s;
}

static Symbol *add_symbol(unsigned line, const Word *w, SymbolKind kind);

/* ------------------------------------------------------------------------
   Structures and their fields (P6). */

typedef enum {
    CON_NONE,
    CON_EQ,
    CON_IN,
    CON_MASK,
    CON_RANGE
} ConstraintKind;

typedef struct {
    ConstraintKind kind;
    Value a;
    int has_min;
    int has_max;
    Value min;
    Value max;
    Value in[IN_VALUES_MAX];
    unsigned in_count;
} Constraint;

typedef struct {
    char name[NAME_CHARS + 1u];
    unsigned line;
    Desc desc;
    int is_struct;
    Scalar scalar;
    unsigned struct_index;
    int is_array;
    Value array_size;
    int has_count;
    unsigned count_field;
    int is_text;
    Constraint constraint;
    U64 offset;
    U64 size;
    U64 element_size;
} Field;

typedef struct {
    char name[CNAME_CHARS + 1u];
    unsigned line;
    Desc desc;
    int imported;
    unsigned first_field;
    unsigned field_count;
    U64 size;
    unsigned align;
    int align_known;
} Struct;

static Struct structs[STRUCTS_MAX];
static unsigned struct_count;
static Field fields[FIELDS_MAX];
static unsigned field_total;

/* The constants, rights and settings the header declares, with the
   order the description declares them in (P14, rule 1). */
typedef enum { D_CONST, D_RIGHTS, D_STRUCT, D_CHANNEL, D_SETTING } DeclKind;

typedef struct {
    DeclKind kind;
    unsigned index;
} Decl;

#define DECLS_MAX 2048u
static Decl decls[DECLS_MAX];
static unsigned decl_count;

/* Record a declaration the header makes, in the order it is declared. */
static void add_decl(unsigned line, DeclKind kind, unsigned index)
{
    if (decl_count >= DECLS_MAX)
        TABLE_FULL(line + 1u, "declarations");
    decls[decl_count].kind = kind;
    decls[decl_count].index = index;
    decl_count++;
}

typedef struct {
    unsigned symbol;
    Desc desc;
    char *text;
    int is_string;
} ConstDecl;

#define CONSTS_MAX 1024u
static ConstDecl consts[CONSTS_MAX];
static unsigned const_count;

/* The interface's own name and description. */
static char interface_name[NAME_CHARS + 1u];
static Desc interface_desc;
static unsigned interface_line;

/* ------------------------------------------------------------------------
   Expressions (P4). */

static int word_is_operator(const Word *w)
{
    return w->length == 1u &&
           (w->text[0] == '+' || w->text[0] == '-' || w->text[0] == '*' ||
            w->text[0] == '/' || w->text[0] == '|');
}

/* The structure declared or imported as name, or 0. */
static Struct *find_struct(const char *name)
{
    Symbol *s = find_symbol(name);

    return s && s->kind == SYM_STRUCT ? &structs[s->index] : 0;
}

/* Parse the value at w[*i]: a Number, a C name of a number declared
   before, or `sizeof` and a structure declared before (P3, `value`);
   advance *i past it. */
static void parse_value(unsigned line, const Word *w, unsigned n, unsigned *i,
                        Value *v)
{
    char name[CNAME_CHARS + 1u];
    Buf text;

    if (*i >= n)
        fail(line + 1u, "a value is missing");
    memset(&text, 0, sizeof text);
    if (word_number(&w[*i], &v->number)) {
        if (w[*i].length >= VALUE_TEXT_CHARS)
            fail(line + 1u, "a number is too long");
        word_copy(&w[*i], v->text, sizeof v->text);
        (*i)++;
        return;
    }
    if (word_is(&w[*i], "sizeof")) {
        Struct *s;

        if (*i + 1u >= n || !is_cname(&w[*i + 1u]))
            fail(line + 1u, "sizeof takes the C name of a structure");
        word_copy(&w[*i + 1u], name, sizeof name);
        s = find_struct(name);
        if (!s)
            fail_name(line + 1u, "sizeof of a name that is no structure "
                                 "declared before",
                      name);
        v->number = s->size;
        buf_u64(&text, s->size);
        memcpy(v->text, text.data, text.length + 1u);
        free(text.data);
        *i += 2u;
        return;
    }
    if (is_cname(&w[*i])) {
        Symbol *s;

        word_copy(&w[*i], name, sizeof name);
        s = find_symbol(name);
        if (!s)
            fail_name(line + 1u, "a name used before it is declared", name);
        if (s->kind == SYM_RIGHTS)
            fail_name(line + 1u, "a rights constant is no operand", name);
        if (s->kind != SYM_NUMBER)
            fail_name(line + 1u, "a name that is not a number", name);
        v->number = s->value;
        memcpy(v->text, name, strlen(name) + 1u);
        (*i)++;
        return;
    }
    fail(line + 1u, "a value is a number, a C name or sizeof and a "
                    "structure");
}

static U64 parse_sequence(unsigned line, const Word *w, unsigned n,
                          unsigned *i, Buf *text, int *has_operator);

/* A term: a value, or an expression in parentheses. */
static U64 parse_term(unsigned line, const Word *w, unsigned n, unsigned *i,
                      Buf *text)
{
    Value v;

    if (*i < n && word_is(&w[*i], "(")) {
        U64 inner;
        int ignored;

        (*i)++;
        buf_ch(text, '(');
        inner = parse_sequence(line, w, n, i, text, &ignored);
        if (*i >= n || !word_is(&w[*i], ")"))
            fail(line + 1u, "a parenthesis is not closed");
        (*i)++;
        buf_ch(text, ')');
        return inner;
    }
    parse_value(line, w, n, i, &v);
    buf_put(text, v.text);
    return v.number;
}

/* Parse terms and one kind of operator, applied left to right (P4,
   rule 1), and append the level's text to text, which starts with
   WIDEN_PREFIX where an operand or a result passes what a C int holds;
   *has_operator tells whether it holds an operator. */
static U64 parse_sequence(unsigned line, const Word *w, unsigned n,
                          unsigned *i, Buf *text, int *has_operator)
{
    Buf level;
    U64 result;
    char op = 0;
    int wide;

    memset(&level, 0, sizeof level);
    result = parse_term(line, w, n, i, &level);
    wide = result > C_INT_MAX_VALUE;
    while (*i < n && word_is_operator(&w[*i])) {
        char c = w[*i].text[0];
        U64 term;

        if (op && op != c)
            fail(line + 1u, "an expression mixes operators at one level "
                            "of parentheses");
        op = c;
        (*i)++;
        buf_put(&level, " ");
        buf_ch(&level, c);
        buf_put(&level, " ");
        term = parse_term(line, w, n, i, &level);
        wide = wide || term > C_INT_MAX_VALUE;
        switch (c) {
        case '+':
            if (result + term < result)
                fail(line + 1u, "a result past 2^64 - 1");
            result += term;
            break;
        case '-':
            if (result < term)
                fail(line + 1u, "a result below 0");
            result -= term;
            break;
        case '*':
            if (result && term > ~(U64)0 / result)
                fail(line + 1u, "a result past 2^64 - 1");
            result *= term;
            break;
        case '/':
            if (term == 0)
                fail(line + 1u, "a division by 0");
            result /= term;
            break;
        default:
            result |= term;
            break;
        }
        wide = wide || result > C_INT_MAX_VALUE;
    }
    if (op && wide)
        buf_put(text, WIDEN_PREFIX);
    buf_putn(text, level.data, level.length);
    free(level.data);
    *has_operator = op != 0;
    return result;
}

/* ------------------------------------------------------------------------
   Constraints (P6, rule 3). */

/* The values of a constraint on a field of scalar type s: each fits the
   type, a range's bounds are in order, and none restates what the type
   guarantees. */
static void check_constraint(unsigned line, Scalar s, const Constraint *c)
{
    U64 largest = scalar_max(s);
    unsigned i;

    switch (c->kind) {
    case CON_NONE:
        return;
    case CON_EQ:
        if (c->a.number > largest)
            fail(line + 1u, "a value past what the field's type holds");
        return;
    case CON_IN:
        for (i = 0; i < c->in_count; i++)
            if (c->in[i].number > largest)
                fail(line + 1u, "a value past what the field's type holds");
        return;
    case CON_MASK:
        if (c->a.number > largest)
            fail(line + 1u, "a mask past what the field's type holds");
        if (!SCALARS[s].is_signed && c->a.number == largest)
            fail(line + 1u, "a mask the field's type already guarantees");
        return;
    case CON_RANGE:
        if (c->has_min && c->min.number > largest)
            fail(line + 1u, "a bound past what the field's type holds");
        if (c->has_max && c->max.number > largest)
            fail(line + 1u, "a bound past what the field's type holds");
        if (c->has_min && !SCALARS[s].is_signed && c->min.number == 0)
            fail(line + 1u, "min 0 on an unsigned field, which its type "
                            "already guarantees");
        if (c->has_max && c->max.number == largest)
            fail(line + 1u, "a max at its type's largest, which the type "
                            "already guarantees");
        if (c->has_min && c->has_max && c->min.number > c->max.number)
            fail(line + 1u, "a min above its max");
        return;
    }
}

/* Parse a constraint at w[*i]: `= V`, `in V W ...`, `mask V`, `min V`
   with an optional `max W`, or `max V`; for a field of scalar s. */
static void parse_constraint(unsigned line, const Word *w, unsigned n,
                             unsigned *i, Scalar s, Constraint *c)
{
    memset(c, 0, sizeof *c);
    if (word_is(&w[*i], "=")) {
        (*i)++;
        c->kind = CON_EQ;
        parse_value(line, w, n, i, &c->a);
    } else if (word_is(&w[*i], "in")) {
        (*i)++;
        c->kind = CON_IN;
        while (*i < n && (word_number(&w[*i], &c->a.number) ||
                          is_cname(&w[*i]) || word_is(&w[*i], "sizeof"))) {
            if (c->in_count >= IN_VALUES_MAX)
                TABLE_FULL(line + 1u, "values of an `in` constraint");
            parse_value(line, w, n, i, &c->in[c->in_count]);
            c->in_count++;
        }
        if (c->in_count < 2u)
            fail(line + 1u, "`in` takes two or more values");
    } else if (word_is(&w[*i], "mask")) {
        (*i)++;
        c->kind = CON_MASK;
        parse_value(line, w, n, i, &c->a);
    } else if (word_is(&w[*i], "min") || word_is(&w[*i], "max")) {
        c->kind = CON_RANGE;
        if (word_is(&w[*i], "min")) {
            (*i)++;
            c->has_min = 1;
            parse_value(line, w, n, i, &c->min);
        }
        if (*i < n && word_is(&w[*i], "max")) {
            (*i)++;
            c->has_max = 1;
            parse_value(line, w, n, i, &c->max);
        }
    } else {
        fail(line + 1u, "an unknown constraint");
    }
    check_constraint(line, s, c);
}

/* Whether w is the word that opens a constraint. */
static int word_starts_constraint(const Word *w)
{
    return word_is(w, "=") || word_is(w, "in") || word_is(w, "mask") ||
           word_is(w, "min") || word_is(w, "max");
}

/* ------------------------------------------------------------------------
   Parsing the blocks that make types: import, const, rights, struct. */

/* The statement at line, whose words are w[0..n). */
#define STATEMENT(line, w, n)                                              \
    const Word *w = line_words(line);                                      \
    unsigned n = lines[line].word_count

/* The next line at or after from that is a statement (not blank and not
   only a comment), or line_count if there is none. */
static unsigned next_statement(unsigned from)
{
    while (from < line_count && lines[from].word_count == 0)
        from++;
    return from;
}

/* Whether name is a keyword of C. */
static int is_c_keyword(const char *name)
{
    unsigned i;

    for (i = 0; i < sizeof C_KEYWORDS / sizeof C_KEYWORDS[0]; i++)
        if (strcmp(name, C_KEYWORDS[i]) == 0)
            return 1;
    return 0;
}

/* Whether structure s, or one it embeds, holds an array. */
static int struct_has_array(const Struct *s)
{
    unsigned i;

    for (i = 0; i < s->field_count; i++) {
        const Field *f = &fields[s->first_field + i];

        if (f->is_array || (f->is_struct &&
                            struct_has_array(&structs[f->struct_index])))
            return 1;
    }
    return 0;
}

/* Add an empty field to the end of struct s, refused if a table is full.
   Returns it. */
static Field *new_field(unsigned line, Struct *s)
{
    Field *f;

    if (field_total >= FIELDS_MAX)
        TABLE_FULL(line + 1u, "fields");
    if (s->field_count >= FIELDS_PER_STRUCT_MAX)
        TABLE_FULL(line + 1u, "fields in a structure");
    f = &fields[field_total];
    memset(f, 0, sizeof *f);
    f->line = line + 1u;
    field_total++;
    s->field_count++;
    return f;
}

/* Lay f out after the fields of s before it (P6, rule 2): at the sum of
   their sizes, a scalar at a multiple of its size and an embedded
   structure at a multiple of its largest scalar's size, align where
   align_known. */
static void place_field(unsigned line, Struct *s, Field *f, unsigned align,
                        int align_known)
{
    f->offset = s->size;
    if (align_known && f->offset % align != 0)
        fail(line + 1u, "a field at an offset that is not a multiple of its "
                        "type's size");
    if (f->is_array && f->array_size.number > ~(U64)0 / f->element_size)
        fail(line + 1u, "an array of more than 2^64 - 1 bytes");
    f->size = f->is_array ? f->element_size * f->array_size.number
                          : f->element_size;
    if (s->size + f->size < s->size)
        fail(line + 1u, "a structure of more than 2^64 - 1 bytes");
    s->size += f->size;
    if (align_known) {
        if (align > s->align)
            s->align = align;
        s->align_known = 1;
    }
}

/* Parse one field line of struct s (P3, `field`). `imported` is set
   inside an import, where a field has no constraint and no description.
 */
static void parse_field(unsigned line, Struct *s, int imported)
{
    STATEMENT(line, w, n);
    unsigned i = 0, k, index;
    int scalar = scalar_of(&w[0]);
    Field *f;
    unsigned align = 1;
    int align_known = 0;
    Desc none;

    memset(&none, 0, sizeof none);
    f = new_field(line, s);
    f->desc = imported ? none : need_desc(line);
    if (n < 2u || !is_name(&w[1]))
        fail(line + 1u, "a field is a type and a name");
    word_copy(&w[1], f->name, sizeof f->name);
    if (is_c_keyword(f->name))
        fail_name(line + 1u, "a field named for a C keyword", f->name);
    for (k = s->first_field; k + 1u < field_total; k++)
        if (strcmp(fields[k].name, f->name) == 0)
            fail_name(line + 1u, "a field declared twice in a structure",
                      f->name);
    index = field_total - 1u;
    i = 2;
    if (scalar >= 0) {
        f->scalar = (Scalar)scalar;
        f->element_size = SCALARS[scalar].size;
        align = SCALARS[scalar].size;
        align_known = 1;
    } else {
        Struct *e;
        char name[CNAME_CHARS + 1u];

        if (!is_cname(&w[0]))
            fail(line + 1u, "a field's type is a scalar or the C name of a "
                            "structure");
        word_copy(&w[0], name, sizeof name);
        e = find_struct(name);
        if (!e)
            fail_name(line + 1u, "a structure used before it is declared",
                      name);
        f->is_struct = 1;
        f->struct_index = (unsigned)(e - structs);
        f->element_size = e->size;
        align = e->align;
        align_known = e->align_known;
    }
    if (i < n && word_is(&w[i], "array")) {
        i++;
        f->is_array = 1;
        if (f->is_struct && struct_has_array(&structs[f->struct_index]))
            fail(line + 1u, "an array of structures that hold an array");
        parse_value(line, w, n, &i, &f->array_size);
        if (f->array_size.number == 0)
            fail(line + 1u, "an array of no elements");
        if (i < n && word_is(&w[i], "count")) {
            Field *g = 0;
            unsigned j;

            i++;
            if (i >= n || !is_name(&w[i]))
                fail(line + 1u, "`count` takes the name of a field");
            for (j = s->first_field; j < index; j++)
                if (strlen(fields[j].name) == w[i].length &&
                    memcmp(fields[j].name, w[i].text, w[i].length) == 0) {
                    g = &fields[j];
                    f->count_field = j;
                }
            if (!g)
                fail(line + 1u, "an array's count is a field before it in "
                                "the same structure");
            if (g->is_struct || g->is_array || SCALARS[g->scalar].is_signed)
                fail(line + 1u, "an array's count is an unsigned scalar");
            f->has_count = 1;
            i++;
        }
        if (i < n && word_is(&w[i], "text")) {
            i++;
            f->is_text = 1;
            if (!f->has_count || f->is_struct || f->scalar != SC_U8)
                fail(line + 1u, "`text` needs `count` and u8 elements");
        }
    }
    if (i < n && word_starts_constraint(&w[i])) {
        if (imported)
            fail(line + 1u, "a field of an imported structure has no "
                            "constraint");
        if (f->is_struct)
            fail(line + 1u, "a structure field has no constraint");
        if (f->is_array)
            fail(line + 1u, "a constraint on an array: constrain the "
                            "fields of a structure its elements are");
        parse_constraint(line, w, n, &i, f->scalar, &f->constraint);
    }
    if (i != n)
        fail(line + 1u, "an unexpected word at the end of a field");
    place_field(line, s, f, align, align_known);
}

/* Close structure s: its size is a multiple of its largest scalar's
   (P6, rule 2) and it holds a field. */
static void finish_struct(unsigned line, Struct *s)
{
    if (s->field_count == 0)
        fail(line + 1u, "a structure with no field");
    if (s->align_known && s->size % s->align != 0)
        fail(line + 1u, "a structure whose size is not a multiple of its "
                        "largest scalar's size");
}

/* Parse `struct NAME` and its fields from line (P3, `struct`), to its
   `end`. Returns the line of the `end`. */
static unsigned parse_struct(unsigned line)
{
    STATEMENT(line, w, n);
    Symbol *sym;
    Struct *s;
    unsigned at, end;

    if (n != 2u)
        fail(line + 1u, "`struct` takes a C name");
    if (struct_count >= STRUCTS_MAX)
        TABLE_FULL(line + 1u, "structures");
    s = &structs[struct_count];
    memset(s, 0, sizeof *s);
    s->desc = need_desc(line);
    sym = add_symbol(line, &w[1], SYM_STRUCT);
    sym->index = struct_count;
    word_copy(&w[1], s->name, sizeof s->name);
    s->line = line + 1u;
    s->first_field = field_total;
    struct_count++;
    at = line + 1u;
    for (;;) {
        at = next_statement(at);
        if (at >= line_count)
            fail(line + 1u, "a block is not closed by `end`");
        if (word_is(line_words(at), "end") && lines[at].word_count == 1u)
            break;
        parse_field(at, s, 0);
        at++;
    }
    end = at;
    finish_struct(line, s);
    add_decl(line, D_STRUCT, struct_count - 1u);
    return end;
}

/* ------------------------------------------------------------------------
   Imports (P5). */

typedef struct {
    char header[NAME_CHARS + 16u];
    unsigned line;
    unsigned items;
} Import;

typedef struct {
    int is_struct;
    unsigned symbol;
    char text[VALUE_TEXT_CHARS];
} ImportItem;

static Import imports[IMPORTS_MAX];
static unsigned import_count;
#define IMPORT_ITEMS_MAX 256u
static ImportItem import_items[IMPORT_ITEMS_MAX];
static unsigned import_item_count;

/* Whether a word names a header in include/: letters, digits, `_`, `-`
   and `.`, ending in `.h`. */
static int is_header_name(const Word *w)
{
    unsigned i;

    if (w->length < 3u || w->length >= NAME_CHARS + 16u ||
        w->text[w->length - 2u] != '.' || w->text[w->length - 1u] != 'h')
        return 0;
    for (i = 0; i < w->length; i++)
        if (!is_lower(w->text[i]) && !is_upper(w->text[i]) &&
            !is_digit(w->text[i]) && w->text[i] != '_' &&
            w->text[i] != '-' && w->text[i] != '.')
            return 0;
    return 1;
}

/* Parse an `import` block from line (P3, `import`), to its `end`.
   Returns the line of the `end`. */
static unsigned parse_import(unsigned line)
{
    STATEMENT(line, w, n);
    Import *im;
    unsigned at;

    if (n != 2u || !is_header_name(&w[1]))
        fail(line + 1u, "`import` takes the name of a header in include/");
    if (import_count >= IMPORTS_MAX)
        TABLE_FULL(line + 1u, "imports");
    im = &imports[import_count];
    word_copy(&w[1], im->header, sizeof im->header);
    for (at = 0; at < import_count; at++)
        if (strcmp(imports[at].header, im->header) == 0)
            fail_name(line + 1u, "a header imported twice", im->header);
    im->line = line + 1u;
    im->items = import_item_count;
    import_count++;
    at = line + 1u;
    for (;;) {
        unsigned cur = next_statement(at);

        if (cur >= line_count)
            fail(line + 1u, "a block is not closed by `end`");
        {
            const Word *iw = line_words(cur);
            unsigned in = lines[cur].word_count;

            if (word_is(&iw[0], "end") && in == 1u)
                return cur;
            if (import_item_count >= IMPORT_ITEMS_MAX)
                TABLE_FULL(cur + 1u, "names imported");
            if (word_is(&iw[0], "const") && in == 3u) {
                ImportItem *item = &import_items[import_item_count];
                Symbol *sym;
                U64 value;

                if (!word_number(&iw[2], &value))
                    fail(cur + 1u, "an imported constant's value is a "
                                   "number");
                sym = put_symbol(cur, &iw[1], SYM_NUMBER);
                sym->value = value;
                item->is_struct = 0;
                item->symbol = (unsigned)(sym - symbols);
                word_copy(&iw[2], item->text, sizeof item->text);
                import_item_count++;
                at = cur + 1u;
            } else if (word_is(&iw[0], "struct") && in == 3u) {
                ImportItem *item = &import_items[import_item_count];
                Symbol *sym;
                Struct *s;
                U64 size;
                unsigned next;

                if (!word_number(&iw[2], &size) || size == 0)
                    fail(cur + 1u, "an imported structure's size is a "
                                   "number above 0");
                if (struct_count >= STRUCTS_MAX)
                    TABLE_FULL(cur + 1u, "structures");
                s = &structs[struct_count];
                memset(s, 0, sizeof *s);
                sym = put_symbol(cur, &iw[1], SYM_STRUCT);
                sym->index = struct_count;
                word_copy(&iw[1], s->name, sizeof s->name);
                s->line = cur + 1u;
                s->imported = 1;
                s->first_field = field_total;
                struct_count++;
                item->is_struct = 1;
                item->symbol = (unsigned)(sym - symbols);
                item->text[0] = '\0';
                import_item_count++;
                next = next_statement(cur + 1u);
                if (next < line_count) {
                    const Word *nw = line_words(next);

                    if (scalar_of(&nw[0]) >= 0 || is_cname(&nw[0])) {
                        unsigned fl = next;

                        for (;;) {
                            fl = next_statement(fl);
                            if (fl >= line_count)
                                fail(cur + 1u, "a block is not closed by "
                                               "`end`");
                            if (word_is(line_words(fl), "end") &&
                                lines[fl].word_count == 1u)
                                break;
                            parse_field(fl, s, 1);
                            fl++;
                        }
                        if (s->size != size)
                            fail(cur + 1u, "the fields of an imported "
                                           "structure do not fill its size");
                        at = fl + 1u;
                    } else {
                        at = cur + 1u;
                    }
                } else {
                    at = cur + 1u;
                }
                s->size = size;
                s->align_known = s->field_count > 0 ? s->align_known : 0;
            } else {
                fail(cur + 1u, "an import holds `const NAME number` and "
                               "`struct NAME size` lines");
            }
        }
    }
}

/* ------------------------------------------------------------------------
   Constants and rights (P4). */

/* P9: the object types, their macros and their own rights. */
typedef struct {
    const char *name;
    const char *macro;
    const char *own[5];
} TypeInfo;

static const TypeInfo TYPES[] = {
    {"process", "TYPE_PROCESS", {"map", "create_thread", "terminate",
                                 "inspect", 0}},
    {"thread", "TYPE_THREAD", {"start", "terminate", 0, 0, 0}},
    {"section", "TYPE_SECTION", {"read", "write", "execute", "pin", 0}},
    {"channel", "TYPE_CHANNEL", {"read", "write", 0, 0, 0}},
    {"port", "TYPE_PORT", {"queue", "read", 0, 0, 0}},
    {"job", "TYPE_JOB", {"create_process", "create_job", "terminate",
                         "write_execute", 0}},
    {"resource", "TYPE_RESOURCE", {"use", 0, 0, 0, 0}},
    {"interrupt", "TYPE_INTERRUPT", {"bind", "ack", 0, 0, 0}},
    {"initiator", "TYPE_INITIATOR", {"pin", "release", 0, 0, 0}},
    {"pin", "TYPE_PIN", {0, 0, 0, 0, 0}}};
#define TYPE_COUNT 10u

/* P9: the rights every type has. */
static const char *const COMMON_RIGHTS[] = {"duplicate", "transfer", "wait",
                                            "signal"};
#define COMMON_RIGHT_COUNT 4u
#define OWN_RIGHTS_MAX 5u

/* The index in TYPES of the object type w names, or -1. */
static int type_of(const Word *w)
{
    unsigned i;

    for (i = 0; i < TYPE_COUNT; i++)
        if (word_is(w, TYPES[i].name))
            return (int)i;
    return -1;
}

/* The rights of one object type, as the generator tells them apart:
   right id i below OWN_RIGHTS_MAX is the type's own right i, and id
   OWN_RIGHTS_MAX + j is the common right j. A list keeps the order a
   description wrote them in, which the header's expressions keep. */
#define RIGHT_IDS_MAX (OWN_RIGHTS_MAX + COMMON_RIGHT_COUNT)

typedef struct {
    unsigned type;
    unsigned ids[RIGHT_IDS_MAX];
    unsigned count;
    unsigned mask;
} RightsList;

/* The id of the right named by w for type t, or -1 if t has no such
   right. */
static int right_id(unsigned t, const Word *w)
{
    unsigned i;

    for (i = 0; i < OWN_RIGHTS_MAX; i++)
        if (TYPES[t].own[i] && word_is(w, TYPES[t].own[i]))
            return (int)i;
    for (i = 0; i < COMMON_RIGHT_COUNT; i++)
        if (word_is(w, COMMON_RIGHTS[i]))
            return (int)(OWN_RIGHTS_MAX + i);
    return -1;
}

/* Add the right named by w to list l; refused if the type has no such
   right or the list has it already. */
static void rights_add(unsigned line, RightsList *l, const Word *w)
{
    int id = right_id(l->type, w);

    if (id < 0)
        fail(line + 1u, "a right the type does not have");
    if (l->mask & (1u << id))
        fail(line + 1u, "a right named twice");
    l->mask |= 1u << id;
    l->ids[l->count++] = (unsigned)id;
}

/* Append the macros of l's rights, joined by `|`, in the order written:
   `T_RIGHT_R` for a type's own right and `RIGHT_R` for a common one. */
static void put_rights(Buf *b, const RightsList *l)
{
    unsigned i, k;

    for (i = 0; i < l->count; i++) {
        unsigned id = l->ids[i];
        const char *name;

        if (i)
            buf_put(b, " | ");
        if (id < OWN_RIGHTS_MAX) {
            name = TYPES[l->type].own[id];
            for (k = 0; TYPES[l->type].name[k]; k++)
                buf_ch(b, (char)(TYPES[l->type].name[k] - 'a' + 'A'));
            buf_put(b, "_RIGHT_");
        } else {
            name = COMMON_RIGHTS[id - OWN_RIGHTS_MAX];
            buf_put(b, "RIGHT_");
        }
        for (k = 0; name[k]; k++)
            buf_ch(b, (char)(name[k] - 'a' + 'A'));
    }
}

/* The names the generated functions take from native_abi.h and types.h,
   which no C name of a description is (P3, rule 2). */
static const char *const KERNEL_NAMES[] = {
    "OK",          "ERR_INVALID_ARGS", "ERR_NOT_SUPPORTED", "RIGHTS_SAME",
    "RECEIVED_HANDLE", "HANDLE_TRANSFER", "UINT8",           "UINT16",
    "UINT32",      "UINT64",           "INT32",             "INT64"};

/* Whether name is one the generated functions take from native_abi.h and
   types.h: a KERNEL_NAMES entry, an object type's macro or a right's. */
static int is_kernel_name(const char *name)
{
    unsigned t, id, k;
    int found = 0;
    Buf made;

    memset(&made, 0, sizeof made);
    for (k = 0; k < sizeof KERNEL_NAMES / sizeof KERNEL_NAMES[0]; k++)
        found = found || strcmp(name, KERNEL_NAMES[k]) == 0;
    for (t = 0; t < TYPE_COUNT && !found; t++) {
        found = strcmp(name, TYPES[t].macro) == 0;
        for (id = 0; id < RIGHT_IDS_MAX && !found; id++) {
            RightsList one;

            if (id < OWN_RIGHTS_MAX && !TYPES[t].own[id])
                continue;
            memset(&one, 0, sizeof one);
            one.type = t;
            one.ids[0] = id;
            one.count = 1;
            made.length = 0;
            put_rights(&made, &one);
            found = strcmp(made.data, name) == 0;
        }
    }
    free(made.data);
    return found;
}

/* Declare a C name of the description: put_symbol's refusals, and a name
   the generated functions take from native_abi.h. */
static Symbol *add_symbol(unsigned line, const Word *w, SymbolKind kind)
{
    char name[CNAME_CHARS + 1u];

    if (is_cname(w)) {
        word_copy(w, name, sizeof name);
        if (is_kernel_name(name))
            fail_name(line + 1u, KERNEL_NAME_REFUSED, name);
    }
    return put_symbol(line, w, kind);
}

/* A `rights` constant (P4, rule 4). */
typedef struct {
    unsigned symbol;
    Desc desc;
    RightsList list;
} RightsDecl;

#define RIGHTS_MAX 64u
static RightsDecl rights_decls[RIGHTS_MAX];
static unsigned rights_count;

/* Parse `const NAME EXPRESSION` or `const NAME "text"` (P4, rules 1 to
   3). */
static void parse_const(unsigned line)
{
    STATEMENT(line, w, n);
    ConstDecl *c;
    Symbol *sym;
    Desc d;
    Buf text;
    unsigned i;

    if (n < 3u)
        fail(line + 1u, "`const` takes a name and a value");
    if (const_count >= CONSTS_MAX)
        TABLE_FULL(line + 1u, "constants");
    d = need_desc(line);
    memset(&text, 0, sizeof text);
    c = &consts[const_count];
    c->desc = d;
    if (n == 3u && word_is_string(&w[2])) {
        sym = add_symbol(line, &w[1], SYM_STRING);
        buf_putn(&text, w[2].text, w[2].length);
        c->is_string = 1;
    } else {
        U64 value;
        int has_operator;
        Buf inner;

        memset(&inner, 0, sizeof inner);
        i = 2;
        value = parse_sequence(line, w, n, &i, &inner, &has_operator);
        if (i != n)
            fail(line + 1u, "an unexpected word at the end of an "
                            "expression");
        if (has_operator)
            buf_ch(&text, '(');
        buf_putn(&text, inner.data, inner.length);
        if (has_operator)
            buf_ch(&text, ')');
        free(inner.data);
        sym = add_symbol(line, &w[1], SYM_NUMBER);
        sym->value = value;
        c->is_string = 0;
    }
    c->symbol = (unsigned)(sym - symbols);
    c->text = text.data;
    add_decl(line, D_CONST, const_count);
    const_count++;
}

/* Parse `rights NAME TYPE RIGHT...` (P4, rule 4). */
static void parse_rights(unsigned line)
{
    STATEMENT(line, w, n);
    RightsDecl *r;
    Symbol *sym;
    int t;
    unsigned i;

    if (n < 4u)
        fail(line + 1u, "`rights` takes a name, a type and rights");
    if (rights_count >= RIGHTS_MAX)
        TABLE_FULL(line + 1u, "rights constants");
    r = &rights_decls[rights_count];
    memset(r, 0, sizeof *r);
    r->desc = need_desc(line);
    t = type_of(&w[2]);
    if (t < 0)
        fail(line + 1u, "an unknown object type");
    r->list.type = (unsigned)t;
    for (i = 3; i < n; i++)
        rights_add(line, &r->list, &w[i]);
    sym = add_symbol(line, &w[1], SYM_RIGHTS);
    sym->index = rights_count;
    r->symbol = (unsigned)(sym - symbols);
    add_decl(line, D_RIGHTS, rights_count);
    rights_count++;
}

/* ------------------------------------------------------------------------
   Channels and messages (P7 to P10). */

typedef enum { M_REQUEST, M_EVENT, M_REPLY } MessageKind;
typedef enum { BODY_STRUCT, BODY_TRANSACTION, BODY_NONE } BodyKind;
typedef enum { TAIL_STRUCT, TAIL_COUNTED, TAIL_REST } TailShape;
typedef enum { ELEMENT_BYTES, ELEMENT_TEXT, ELEMENT_STRUCT } TailElement;
typedef enum { RS_SAME, RS_NONE, RS_LIST, RS_CONST } RightsSpecKind;
typedef enum { POLICY_NONE, POLICY_CLOSE, POLICY_DROP, POLICY_EXIT,
               POLICY_ANSWER } PolicyKind;

/* Where a path of a message ends: in the body, or in a structure tail
   (tail is its index, else -1), at offset bytes from the start of the
   body or the tail, as a scalar. member is the C member path inside the
   structure, "setup.kind". */
typedef struct {
    int tail;
    U64 offset;
    Scalar scalar;
    char member[PATH_CHARS + 1u];
    int constrained;
} Resolved;

typedef struct {
    char name[NAME_CHARS + 1u];
    unsigned line;
    Desc desc;
    TailShape shape;
    TailElement element;
    unsigned struct_index;
    U64 element_size;
    Resolved count;
    Value max;
} Tail;

typedef struct {
    unsigned line;
    Resolved at;
    int is_tails;
    Constraint constraint;
} FieldRule;

typedef struct {
    RightsSpecKind kind;
    RightsList list;
    char const_name[CNAME_CHARS + 1u];
} RightsSpec;

typedef struct {
    unsigned line;
    Desc desc;
    unsigned type;
    U64 min_count;
    U64 max_count;
    Value min_text;
    Value max_text;
    int has_min;
    int has_max;
    RightsSpec travels;
    RightsSpec needs;
    int has_index;
    char index_name[CNAME_CHARS + 1u];
    U64 position;
} HandleLine;

typedef struct {
    MessageKind kind;
    char name[NAME_CHARS + 1u];
    unsigned line;
    Desc desc;
    int has_cname;
    char cname[CNAME_CHARS + 1u];
    U64 value;
    char value_text[VALUE_TEXT_CHARS];
    BodyKind body;
    unsigned body_struct;
    U64 body_size;
    int first;
    int retired;
    int has_status;
    Resolved status;
    int has_reply;
    char reply_name[NAME_CHARS + 1u];
    int reply_on;
    char reply_channel[NAME_CHARS + 1u];
    int reply_message;
    int has_command;
    char command[NAME_CHARS + 1u];
    unsigned rule_count;
    FieldRule rules[FIELD_LINES_PER_MESSAGE_MAX];
    unsigned tail_count;
    Tail tails[TAILS_PER_MESSAGE_MAX];
    unsigned handle_count;
    HandleLine handles[HANDLE_LINES_PER_MESSAGE_MAX];
    int has_handles_macro;
    char handles_name[CNAME_CHARS + 1u];
    Desc handles_desc;
    int has_transaction;
    Resolved transaction;
    int has_selector;
    Resolved selector;
    int named_by_request;
    U64 bytes_max;
    U64 handles_min;
    U64 handles_max;
    unsigned channel;
} Message;

typedef struct {
    PolicyKind kind;
    char reply[NAME_CHARS + 1u];
    int has_status;
    char status[CNAME_CHARS + 1u];
    unsigned line;
} Policy;

typedef struct {
    char name[NAME_CHARS + 1u];
    unsigned line;
    Desc desc;
    int has_capacity;
    Value capacity;
    unsigned capacity_line;
    int has_select;
    char select[NAME_CHARS + 1u];
    unsigned select_line;
    Policy server;
    Policy client;
    unsigned first_message;
    unsigned message_count;
    int has_requests;
    int has_replies;
    int has_events;
    U64 server_bytes_max;
    U64 server_handles_max;
    U64 client_bytes_max;
    U64 client_handles_max;
    U64 select_offset;
    Scalar select_scalar;
} Channel;

static Channel channels[CHANNELS_MAX];
static unsigned channel_count;
static Message *messages[MESSAGES_MAX];
static unsigned message_total;

/* ------------------------------------------------------------------------
   Paths. */

/* The field of struct s named by the span, or 0. */
static Field *struct_field(const Struct *s, const char *name, unsigned length)
{
    unsigned i;

    for (i = 0; i < s->field_count; i++) {
        Field *f = &fields[s->first_field + i];

        if (strlen(f->name) == length && memcmp(f->name, name, length) == 0)
            return f;
    }
    return 0;
}

/* Append the component name to the member path, joined by a dot. */
static void member_append(unsigned line, char *member, const char *name,
                          unsigned length)
{
    size_t have = strlen(member);

    if (have + length + 2u > PATH_CHARS)
        fail(line + 1u, "a path is too long");
    if (have)
        member[have++] = '.';
    memcpy(member + have, name, length);
    member[have + length] = '\0';
}

/* Resolve the path in word w against the structure start, whose first
   field lies at offset base: each component but the last names an
   embedded structure and the last a scalar. Returns 1, with *r filled in
   (r->tail and r->constrained left to the caller), or 0 if the path
   leads nowhere. */
static int walk_path(unsigned line, const Word *w, const Struct *start,
                     unsigned from, U64 base, Resolved *r)
{
    const Struct *cur = start;
    U64 offset = base;
    unsigned i = from;

    r->member[0] = '\0';
    for (;;) {
        unsigned end = i;
        Field *f;

        while (end < w->length && w->text[end] != '.')
            end++;
        f = struct_field(cur, w->text + i, end - i);
        if (!f)
            return 0;
        offset += f->offset;
        member_append(line, r->member, w->text + i, end - i);
        if (end == w->length) {
            if (f->is_struct || f->is_array)
                return 0;
            r->offset = offset;
            r->scalar = f->scalar;
            r->constrained = f->constraint.kind != CON_NONE;
            return 1;
        }
        if (!f->is_struct || f->is_array)
            return 0;
        cur = &structs[f->struct_index];
        i = end + 1u;
    }
}

/* Resolve a message's path word (P3, `path`): a field of its body, or a
   field of an earlier structure tail, which the first component names.
   Refused where it leads nowhere. */
static void resolve_path(unsigned line, const Message *m, const Word *w,
                         Resolved *r)
{
    unsigned first_end = 0, t;
    int found = 0;

    memset(r, 0, sizeof *r);
    r->tail = -1;
    if (!is_path(w))
        fail(line + 1u, "a path is names joined by dots");
    while (first_end < w->length && w->text[first_end] != '.')
        first_end++;
    if (m->body == BODY_TRANSACTION) {
        if (word_is(w, "transaction")) {
            r->offset = 0;
            r->scalar = SC_U32;
            word_copy(w, r->member, sizeof r->member);
            return;
        }
    } else if (m->body == BODY_STRUCT) {
        found = walk_path(line, w, &structs[m->body_struct], 0, 0, r);
        if (found)
            return;
    }
    for (t = 0; t < m->tail_count; t++) {
        const Tail *tail = &m->tails[t];

        if (tail->shape != TAIL_STRUCT || strlen(tail->name) != first_end ||
            memcmp(tail->name, w->text, first_end) != 0)
            continue;
        if (first_end == w->length)
            break;
        if (walk_path(line, w, &structs[tail->struct_index], first_end + 1u,
                      0, r)) {
            r->tail = (int)t;
            return;
        }
    }
    fail(line + 1u, "a path that leads to no field of the body or of an "
                    "earlier structure tail");
}

/* ------------------------------------------------------------------------
   Parsing a channel. */

static Message *new_message(unsigned line)
{
    Message *m;

    if (message_total >= MESSAGES_MAX)
        TABLE_FULL(line + 1u, "messages");
    m = (Message *)xalloc(sizeof *m);
    memset(m, 0, sizeof *m);
    messages[message_total++] = m;
    return m;
}

/* Parse the rights of `travels` or `needs` at w[*i] for a handle of an
   object type, advancing *i; `same` is for `travels` alone. */
static RightsSpec parse_rights_spec(unsigned line, const Word *w, unsigned n,
                                    unsigned *i, unsigned type, int travels)
{
    RightsSpec spec;

    memset(&spec, 0, sizeof spec);
    spec.list.type = type;
    if (*i >= n)
        fail(line + 1u, "a rights list is missing");
    if (word_is(&w[*i], "same")) {
        if (!travels)
            fail(line + 1u, "`needs` cannot be `same`");
        spec.kind = RS_SAME;
        (*i)++;
    } else if (word_is(&w[*i], "none")) {
        spec.kind = RS_NONE;
        (*i)++;
    } else if (is_cname(&w[*i])) {
        char name[CNAME_CHARS + 1u];
        Symbol *s;

        word_copy(&w[*i], name, sizeof name);
        s = find_symbol(name);
        if (!s || s->kind != SYM_RIGHTS)
            fail_name(line + 1u, "a name that is no rights constant", name);
        if (rights_decls[s->index].list.type != type)
            fail_name(line + 1u, "a rights constant of another type", name);
        spec.kind = RS_CONST;
        spec.list = rights_decls[s->index].list;
        memcpy(spec.const_name, name, strlen(name) + 1u);
        (*i)++;
    } else {
        spec.kind = RS_LIST;
        while (*i < n && !word_is(&w[*i], "needs") &&
               !word_is(&w[*i], "index") && !word_is(&w[*i], "travels"))
            rights_add(line, &spec.list, &w[(*i)++]);
        if (spec.list.count == 0)
            fail(line + 1u, "a rights list is missing");
    }
    return spec;
}

/* Parse a `handle` line (P3, `handle`; P9). */
static void parse_handle(unsigned line, Message *m)
{
    STATEMENT(line, w, n);
    HandleLine *h;
    unsigned i = 2;
    int t = 0;

    if (m->handle_count >= HANDLE_LINES_PER_MESSAGE_MAX)
        TABLE_FULL(line + 1u, "handle lines in a message");
    h = &m->handles[m->handle_count];
    memset(h, 0, sizeof *h);
    h->desc = need_desc(line);
    h->line = line + 1u;
    if (n < 2u || (t = type_of(&w[1])) < 0)
        fail(line + 1u, "`handle` takes an object type");
    h->type = (unsigned)t;
    h->min_count = 1;
    h->max_count = 1;
    if (i < n && word_is(&w[i], "min")) {
        i++;
        parse_value(line, w, n, &i, &h->min_text);
        h->has_min = 1;
        h->min_count = h->min_text.number;
    }
    if (i < n && word_is(&w[i], "max")) {
        i++;
        parse_value(line, w, n, &i, &h->max_text);
        h->has_max = 1;
        h->max_count = h->max_text.number;
    }
    if (h->has_min != h->has_max)
        fail(line + 1u, "`min` and `max` of a handle run come together");
    if (h->has_min && h->min_count > h->max_count)
        fail(line + 1u, "a handle run's min is above its max");
    if (h->max_count == 0)
        fail(line + 1u, "a handle run of no handles");
    if (i >= n || !word_is(&w[i], "travels"))
        fail(line + 1u, "`handle` needs `travels`");
    i++;
    h->travels = parse_rights_spec(line, w, n, &i, h->type, 1);
    if (i >= n || !word_is(&w[i], "needs"))
        fail(line + 1u, "`handle` needs `needs`");
    i++;
    h->needs = parse_rights_spec(line, w, n, &i, h->type, 0);
    if (h->travels.kind != RS_SAME &&
        (h->needs.list.mask & ~h->travels.list.mask))
        fail(line + 1u, "a right the receiver needs that the handle does "
                        "not travel with");
    if (i < n && word_is(&w[i], "index")) {
        Symbol *sym;

        i++;
        if (i >= n)
            fail(line + 1u, "`index` takes a C name");
        sym = add_symbol(line, &w[i], SYM_NUMBER);
        h->has_index = 1;
        memcpy(h->index_name, sym->name, strlen(sym->name) + 1u);
        h->position = 0;
        i++;
    }
    if (i != n)
        fail(line + 1u, "an unexpected word at the end of a handle line");
    m->handle_count++;
}

/* Refuse a structure tail whose structure holds an array, which a check
   of the tail does not walk, or whose start is not a multiple of the
   structure's alignment wherever the tails before it end, so that the
   pointer its accessor returns is aligned (P8, rule 9). The message's bytes
   are aligned to 8 bytes, the largest alignment of any scalar. */
static void check_struct_tail(unsigned line, const Message *m, const Struct *s)
{
    U64 start;
    unsigned j;

    if (struct_has_array(s))
        fail(line + 1u, "a structure tail whose structure holds an array");
    if (!s->align_known)
        fail(line + 1u, "a structure tail whose structure's alignment is "
                        "not known: an import lists its fields");
    start = m->body_size % s->align;
    for (j = 0; j < m->tail_count; j++) {
        const Tail *before = &m->tails[j];

        if (before->shape == TAIL_STRUCT)
            start = (start + before->element_size % s->align) % s->align;
        else if (before->element_size % s->align != 0)
            fail(line + 1u, "a structure tail after a counted tail whose "
                            "elements are not a multiple of its alignment");
    }
    if (start != 0)
        fail(line + 1u, "a structure tail at an offset that is not a "
                        "multiple of its structure's alignment");
}

/* Parse a `tail` line (P3, `tail`; P8). */
static void parse_tail(unsigned line, Message *m)
{
    STATEMENT(line, w, n);
    Tail *t;
    unsigned i, k;
    Struct *s;
    char name[CNAME_CHARS + 1u];

    if (m->tail_count >= TAILS_PER_MESSAGE_MAX)
        TABLE_FULL(line + 1u, "tails in a message");
    t = &m->tails[m->tail_count];
    memset(t, 0, sizeof *t);
    t->desc = need_desc(line);
    t->line = line + 1u;
    t->count.tail = -1;
    if (n < 3u || !is_name(&w[1]))
        fail(line + 1u, "a tail is `tail` and a name");
    word_copy(&w[1], t->name, sizeof t->name);
    if (is_c_keyword(t->name))
        fail_name(line + 1u, "a tail named for a C keyword", t->name);
    for (k = 0; k < m->tail_count; k++)
        if (strcmp(m->tails[k].name, t->name) == 0)
            fail_name(line + 1u, "a tail declared twice in a message",
                      t->name);
    if (m->tail_count && m->tails[m->tail_count - 1u].shape == TAIL_REST)
        fail(line + 1u, "a tail after the tail that runs to the end");
    if (n == 3u) {
        if (!is_cname(&w[2]))
            fail(line + 1u, "a structure tail names a structure");
        word_copy(&w[2], name, sizeof name);
        s = find_struct(name);
        if (!s)
            fail_name(line + 1u, "a structure used before it is declared",
                      name);
        check_struct_tail(line, m, s);
        t->shape = TAIL_STRUCT;
        t->element = ELEMENT_STRUCT;
        t->struct_index = (unsigned)(s - structs);
        t->element_size = s->size;
        m->tail_count++;
        return;
    }
    if (word_is(&w[2], "bytes")) {
        t->element = ELEMENT_BYTES;
        t->element_size = 1;
    } else if (word_is(&w[2], "text")) {
        t->element = ELEMENT_TEXT;
        t->element_size = 1;
    } else if (is_cname(&w[2])) {
        word_copy(&w[2], name, sizeof name);
        s = find_struct(name);
        if (!s)
            fail_name(line + 1u, "a structure used before it is declared",
                      name);
        check_struct_tail(line, m, s);
        t->element = ELEMENT_STRUCT;
        t->struct_index = (unsigned)(s - structs);
        t->element_size = s->size;
    } else {
        fail(line + 1u, "a tail is bytes, text or a structure");
    }
    i = 3;
    if (i < n && word_is(&w[i], "count")) {
        i++;
        if (i >= n)
            fail(line + 1u, "`count` takes a path");
        t->shape = TAIL_COUNTED;
        resolve_path(line, m, &w[i], &t->count);
        if (SCALARS[t->count.scalar].is_signed)
            fail(line + 1u, "a tail's count is an unsigned field");
        if (t->count.scalar == SC_U8)
            fail(line + 1u, "a tail's count is a u16, u32 or u64 field");
        i++;
    } else if (i < n && word_is(&w[i], "rest")) {
        if (t->element == ELEMENT_STRUCT)
            fail(line + 1u, "`rest` is for bytes and text");
        t->shape = TAIL_REST;
        i++;
    } else {
        fail(line + 1u, "a tail is counted by `count` or runs to the end "
                        "with `rest`");
    }
    if (i >= n || !word_is(&w[i], "max"))
        fail(line + 1u, "a counted tail needs `max`");
    i++;
    parse_value(line, w, n, &i, &t->max);
    if (t->max.number == 0)
        fail(line + 1u, "a tail with a maximum of 0");
    if (i != n)
        fail(line + 1u, "an unexpected word at the end of a tail");
    m->tail_count++;
}

/* Parse a `field` line (P3, `message-line`; P6, rule 3; P8, rule 4). */
static void parse_field_rule(unsigned line, Message *m)
{
    STATEMENT(line, w, n);
    FieldRule *r;
    unsigned i = 2;

    if (n < 3u)
        fail(line + 1u, "`field` takes a path and a constraint");
    if (m->rule_count >= FIELD_LINES_PER_MESSAGE_MAX)
        TABLE_FULL(line + 1u, "field lines in a message");
    r = &m->rules[m->rule_count];
    memset(r, 0, sizeof *r);
    r->line = line + 1u;
    resolve_path(line, m, &w[1], &r->at);
    if (r->at.constrained)
        fail(line + 1u, "a field its structure already constrains");
    for (i = 0; i < m->rule_count; i++)
        if (m->rules[i].at.tail == r->at.tail &&
            m->rules[i].at.offset == r->at.offset)
            fail(line + 1u, "a field constrained twice");
    i = 2;
    if (n == 4u && word_is(&w[2], "=") && word_is(&w[3], "tails")) {
        if (r->at.tail >= 0)
            fail(line + 1u, "a field that holds the bytes after the body is "
                            "a field of the body, not of a structure tail");
        if (r->at.scalar != SC_U16 && r->at.scalar != SC_U32 &&
            r->at.scalar != SC_U64)
            fail(line + 1u, "a field that holds the bytes after the body is "
                            "a u16, u32 or u64");
        r->is_tails = 1;
    } else {
        parse_constraint(line, w, n, &i, r->at.scalar, &r->constraint);
        if (i != n)
            fail(line + 1u, "an unexpected word at the end of a field line");
    }
    m->rule_count++;
}

/* Parse the policy words of an `invalid` line. */
static void parse_policy(unsigned line, const Word *w, unsigned n,
                         Policy *p, int server)
{
    memset(p, 0, sizeof *p);
    p->line = line + 1u;
    if (word_is(&w[0], "close") && n == 1u) {
        p->kind = POLICY_CLOSE;
    } else if (word_is(&w[0], "drop") && n == 1u) {
        p->kind = POLICY_DROP;
    } else if (word_is(&w[0], "exit") && n == 1u) {
        p->kind = POLICY_EXIT;
    } else if (word_is(&w[0], "answer") && (n == 2u || n == 3u)) {
        if (!server)
            fail(line + 1u, "`answer` is a server's policy");
        if (!is_name(&w[1]))
            fail(line + 1u, "`answer` takes the name of a reply");
        p->kind = POLICY_ANSWER;
        word_copy(&w[1], p->reply, sizeof p->reply);
        if (n == 3u) {
            Symbol *s;

            if (!is_cname(&w[2]))
                fail(line + 1u, "`answer` takes a status's C name");
            word_copy(&w[2], p->status, sizeof p->status);
            s = find_symbol(p->status);
            if (!s || s->kind != SYM_NUMBER)
                fail_name(line + 1u, "a status that is no number declared "
                                     "before",
                          p->status);
            p->has_status = 1;
        }
    } else {
        fail(line + 1u, "a policy is close, drop, exit or answer REPLY "
                        "[STATUS]");
    }
}

/* Parse a message from line to its `end` (P3, `message`). Returns the
   line of the `end`. */
static unsigned parse_message(unsigned line, Channel *ch)
{
    STATEMENT(line, w, n);
    Message *m = new_message(line);
    unsigned at = line + 1u, k;
    int have_body = 0;

    m->line = line + 1u;
    m->channel = (unsigned)(ch - channels);
    m->reply_message = -1;
    m->kind = word_is(&w[0], "request") ? M_REQUEST
              : word_is(&w[0], "event")  ? M_EVENT
                                         : M_REPLY;
    m->desc = need_desc(line);
    if (n < 2u || !is_name(&w[1]))
        fail(line + 1u, "a message is a keyword and a name");
    word_copy(&w[1], m->name, sizeof m->name);
    for (k = 0; k < ch->message_count; k++)
        if (strcmp(messages[ch->first_message + k]->name, m->name) == 0)
            fail_name(line + 1u, "a message declared twice in a channel",
                      m->name);
    if (m->kind == M_REPLY) {
        if (n != 2u)
            fail(line + 1u, "a reply has a name alone");
    } else if (n == 4u) {
        Symbol *sym;

        if (!word_number(&w[3], &m->value) || w[3].length >= VALUE_TEXT_CHARS)
            fail(line + 1u, "a message's value is a number");
        word_copy(&w[3], m->value_text, sizeof m->value_text);
        sym = add_symbol(line, &w[2], SYM_NUMBER);
        sym->value = m->value;
        m->has_cname = 1;
        word_copy(&w[2], m->cname, sizeof m->cname);
    } else if (n != 2u) {
        fail(line + 1u, "a request or event is a name, and a C name and "
                        "a value");
    }
    ch->message_count++;
    for (;;) {
        unsigned cur = next_statement(at);
        const Word *lw;
        unsigned ln;

        if (cur >= line_count)
            fail(line + 1u, "a block is not closed by `end`");
        lw = line_words(cur);
        ln = lines[cur].word_count;
        at = cur + 1u;
        if (word_is(&lw[0], "end") && ln == 1u) {
            if (!have_body)
                fail(line + 1u, "a message has no `body`");
            return cur;
        }
        if (word_is(&lw[0], "body")) {
            Struct *s;
            char name[CNAME_CHARS + 1u];

            if (have_body || ln != 2u)
                fail(cur + 1u, "`body` is once, with a structure, "
                               "`transaction` or `none`");
            have_body = 1;
            if (word_is(&lw[1], "transaction")) {
                m->body = BODY_TRANSACTION;
                m->body_size = 4;
            } else if (word_is(&lw[1], "none")) {
                m->body = BODY_NONE;
                m->body_size = 0;
            } else if (is_cname(&lw[1])) {
                word_copy(&lw[1], name, sizeof name);
                s = find_struct(name);
                if (!s)
                    fail_name(cur + 1u, "a structure used before it is "
                                        "declared",
                              name);
                m->body = BODY_STRUCT;
                m->body_struct = (unsigned)(s - structs);
                m->body_size = s->size;
            } else {
                fail(cur + 1u, "a body is a structure, `transaction` or "
                               "`none`");
            }
            continue;
        }
        if (!have_body)
            fail(cur + 1u, "a message's `body` comes first");
        if (word_is(&lw[0], "first") && ln == 1u) {
            m->first = 1;
        } else if (word_is(&lw[0], "retired") && ln == 1u) {
            m->retired = 1;
        } else if (word_is(&lw[0], "status") && ln == 2u) {
            if (m->kind != M_REPLY)
                fail(cur + 1u, "`status` is for a reply");
            m->has_status = 1;
            resolve_path(cur, m, &lw[1], &m->status);
            if (m->status.scalar != SC_U32 || m->status.tail >= 0)
                fail(cur + 1u, "a reply's status is a u32 field of its "
                               "body");
        } else if (word_is(&lw[0], "reply") && (ln == 2u || ln == 4u)) {
            if (m->kind != M_REQUEST)
                fail(cur + 1u, "`reply` is for a request");
            if (m->has_reply)
                fail(cur + 1u, "a request names one reply");
            if (!is_name(&lw[1]))
                fail(cur + 1u, "`reply` takes a name");
            m->has_reply = 1;
            word_copy(&lw[1], m->reply_name, sizeof m->reply_name);
            if (ln == 4u) {
                if (!word_is(&lw[2], "on") || !is_name(&lw[3]))
                    fail(cur + 1u, "`reply NAME on CHANNEL`");
                m->reply_on = 1;
                word_copy(&lw[3], m->reply_channel,
                          sizeof m->reply_channel);
            }
        } else if (word_is(&lw[0], "command") && ln == 2u) {
            if (m->kind != M_REQUEST)
                fail(cur + 1u, "`command` is for a request");
            if (m->has_command)
                fail(cur + 1u, "a request names one command");
            if (!is_setting_word(&lw[1]))
                fail(cur + 1u, "a command is a word of [a-z][a-z0-9-]*");
            m->has_command = 1;
            word_copy(&lw[1], m->command, sizeof m->command);
        } else if (word_is(&lw[0], "field")) {
            parse_field_rule(cur, m);
        } else if (word_is(&lw[0], "tail")) {
            parse_tail(cur, m);
        } else if (word_is(&lw[0], "handle")) {
            parse_handle(cur, m);
        } else if (word_is(&lw[0], "handles") && ln == 2u) {
            Symbol *sym;

            if (m->has_handles_macro)
                fail(cur + 1u, "`handles` is once in a message");
            m->handles_desc = need_desc(cur);
            sym = add_symbol(cur, &lw[1], SYM_NUMBER);
            m->has_handles_macro = 1;
            word_copy(&lw[1], m->handles_name, sizeof m->handles_name);
            (void)sym;
        } else {
            fail(cur + 1u, "an unknown line in a message");
        }
    }
}

/* Parse a `channel` block from line (P3, `channel`; P7). Returns the line
   of its `end`. */
static unsigned parse_channel(unsigned line)
{
    STATEMENT(line, w, n);
    Channel *ch;
    unsigned at = line + 1u, k;
    int in_messages = 0;

    if (n != 2u || !is_name(&w[1]))
        fail(line + 1u, "`channel` takes a name");
    if (channel_count >= CHANNELS_MAX)
        TABLE_FULL(line + 1u, "channels");
    ch = &channels[channel_count];
    memset(ch, 0, sizeof *ch);
    ch->desc = need_desc(line);
    ch->line = line + 1u;
    word_copy(&w[1], ch->name, sizeof ch->name);
    for (k = 0; k < channel_count; k++)
        if (strcmp(channels[k].name, ch->name) == 0)
            fail_name(line + 1u, "a channel declared twice", ch->name);
    ch->first_message = message_total;
    channel_count++;
    for (;;) {
        unsigned cur = next_statement(at);
        const Word *lw;
        unsigned ln;

        if (cur >= line_count)
            fail(line + 1u, "a block is not closed by `end`");
        lw = line_words(cur);
        ln = lines[cur].word_count;
        if (word_is(&lw[0], "end") && ln == 1u) {
            add_decl(line, D_CHANNEL, (unsigned)(ch - channels));
            return cur;
        }
        if (word_is(&lw[0], "request") || word_is(&lw[0], "event") ||
            word_is(&lw[0], "reply")) {
            in_messages = 1;
            at = parse_message(cur, ch) + 1u;
            continue;
        }
        if (in_messages)
            fail(cur + 1u, "a channel's own lines come before its messages");
        if (word_is(&lw[0], "capacity") && ln == 2u) {
            unsigned i = 1;

            if (ch->has_capacity)
                fail(cur + 1u, "`capacity` is once in a channel");
            if (!is_cname(&lw[1]))
                fail(cur + 1u, "`capacity` takes a C name");
            parse_value(cur, lw, ln, &i, &ch->capacity);
            ch->has_capacity = 1;
            ch->capacity_line = cur + 1u;
            if (ch->capacity.number == 0 ||
                ch->capacity.number > CHANNEL_MAX_CAPACITY_VALUE)
                fail(cur + 1u, "a capacity of 1 to CHANNEL_MAX_CAPACITY");
        } else if (word_is(&lw[0], "select") && ln == 2u) {
            if (ch->has_select)
                fail(cur + 1u, "`select` is once in a channel");
            if (!is_name(&lw[1]))
                fail(cur + 1u, "`select` takes a name");
            ch->has_select = 1;
            word_copy(&lw[1], ch->select, sizeof ch->select);
            ch->select_line = cur + 1u;
        } else if (word_is(&lw[0], "invalid") && ln >= 3u) {
            if (word_is(&lw[1], "server")) {
                if (ch->server.kind != POLICY_NONE)
                    fail(cur + 1u, "`invalid server` is once in a channel");
                parse_policy(cur, lw + 2, ln - 2u, &ch->server, 1);
            } else if (word_is(&lw[1], "client")) {
                if (ch->client.kind != POLICY_NONE)
                    fail(cur + 1u, "`invalid client` is once in a channel");
                parse_policy(cur, lw + 2, ln - 2u, &ch->client, 0);
            } else {
                fail(cur + 1u, "`invalid` is for `server` or `client`");
            }
        } else {
            fail(cur + 1u, "an unknown line in a channel");
        }
        at = cur + 1u;
    }
}

/* ------------------------------------------------------------------------
   Settings (P12). */

typedef enum {
    ST_BOOL,
    ST_INT,
    ST_ENUM,
    ST_TEXT,
    ST_COLOUR,
    ST_MATERIAL,
    ST_OUTLINE
} SettingType;

/* The kinds of point an outline's contour holds, as the flags of
   font-format.md give them with the cubic's added (P12). */
#define POINT_ON_CURVE 1u
#define POINT_QUADRATIC 0u
#define POINT_CUBIC 2u

typedef struct {
    char name[PATH_CHARS + 1u];
    char cname[CNAME_CHARS + 1u];
    unsigned line;
    Desc desc;
    SettingType type;
    int has_default;
    int bool_default;
    I64 int_default;
    int has_min;
    int has_max;
    I64 int_min;
    I64 int_max;
    unsigned allowed_count;
    char allowed[ALLOWED_WORDS_MAX][NAME_CHARS + 1u];
    unsigned enum_default;
    U64 text_max;
    char text_default[LINE_BYTES_MAX];
    char colour[VALUE_TEXT_CHARS];
    char tint[VALUE_TEXT_CHARS];
    U64 amount;
    unsigned default_line;
    unsigned point_count;
    unsigned contour_count;
    I64 points[POINTS_MAX][2];
    unsigned flags[POINTS_MAX];
    unsigned ends[CONTOURS_MAX];
} Setting;

static Setting settings[SETTINGS_MAX];
static unsigned setting_count;

/* A colour word: `0x` and exactly six hexadecimal digits (P12). */
static int is_colour_word(const Word *w)
{
    unsigned i;

    if (w->length != 8u || w->text[0] != '0' || w->text[1] != 'x')
        return 0;
    for (i = 2; i < 8u; i++)
        if (!is_hex_digit(w->text[i]))
            return 0;
    return 1;
}

/* Parse the contour on line into setting s (P12, `outline`): points on
   the curve bare, a quadratic's control point after `q` and a cubic's
   two after `c`. Between two points on the curve, the last joined to the
   first, there are none, one `q`, or two `c`. */
static void parse_contour(unsigned line, Setting *s)
{
    STATEMENT(line, w, n);
    unsigned i = 1, start = s->point_count, k, on_count = 0;

    if (s->contour_count >= CONTOURS_MAX)
        TABLE_FULL(line + 1u, "contours");
    while (i < n) {
        unsigned kind = POINT_ON_CURVE;
        I64 x = 0, y = 0;

        if (word_is(&w[i], "q") || word_is(&w[i], "c")) {
            kind = word_is(&w[i], "q") ? POINT_QUADRATIC : POINT_CUBIC;
            i++;
        }
        if (i >= n || !word_point(&w[i], &x, &y))
            fail(line + 1u, "a contour's points are two decimal numbers "
                            "joined by a comma");
        if (x > OUTLINE_COORDINATE_LIMIT || x < -OUTLINE_COORDINATE_LIMIT ||
            y > OUTLINE_COORDINATE_LIMIT || y < -OUTLINE_COORDINATE_LIMIT)
            fail(line + 1u, "a coordinate beyond 2^24 of 0");
        if (s->point_count >= POINTS_MAX)
            TABLE_FULL(line + 1u, "points");
        s->points[s->point_count][0] = x;
        s->points[s->point_count][1] = y;
        s->flags[s->point_count] = kind;
        s->point_count++;
        i++;
    }
    if (s->point_count == start || s->flags[start] != POINT_ON_CURVE)
        fail(line + 1u, "a contour starts with a point on the curve");
    for (k = start; k < s->point_count; k++)
        if (s->flags[k] == POINT_ON_CURVE)
            on_count++;
    if (on_count < 2u)
        fail(line + 1u, "a contour holds at least two points on the curve");
    /* The controls between two points on the curve, taken cyclically. */
    for (k = start; k < s->point_count; k++) {
        unsigned j = k + 1u, between = 0, kinds = 0;

        if (s->flags[k] != POINT_ON_CURVE)
            continue;
        for (;; j++) {
            unsigned at = j < s->point_count ? j : start;

            if (s->flags[at] == POINT_ON_CURVE)
                break;
            between++;
            kinds |= s->flags[at] == POINT_QUADRATIC ? 1u : 2u;
        }
        if (!((between == 0) || (between == 1 && kinds == 1u) ||
              (between == 2 && kinds == 2u)))
            fail(line + 1u, "between two points on the curve a contour has "
                            "none, one `q` or two `c`");
    }
    s->ends[s->contour_count++] = s->point_count;
}

/* Parse a `setting` block from line (P3, `setting`; P12). Returns the
   line of its `end`. */
static unsigned parse_setting(unsigned line)
{
    STATEMENT(line, w, n);
    Setting *s;
    unsigned at = line + 1u, k;

    if (n != 3u || !is_setting_name(&w[1]))
        fail(line + 1u, "`setting` takes a name and a type");
    if (setting_count >= SETTINGS_MAX)
        TABLE_FULL(line + 1u, "settings");
    s = &settings[setting_count];
    memset(s, 0, sizeof *s);
    s->desc = need_desc(line);
    s->line = line + 1u;
    word_copy(&w[1], s->name, sizeof s->name);
    for (k = 0; k < setting_count; k++)
        if (strcmp(settings[k].name, s->name) == 0)
            fail_name(line + 1u, "a setting declared twice", s->name);
    if (word_is(&w[2], "bool"))
        s->type = ST_BOOL;
    else if (word_is(&w[2], "int"))
        s->type = ST_INT;
    else if (word_is(&w[2], "enum"))
        s->type = ST_ENUM;
    else if (word_is(&w[2], "text"))
        s->type = ST_TEXT;
    else if (word_is(&w[2], "colour"))
        s->type = ST_COLOUR;
    else if (word_is(&w[2], "material"))
        s->type = ST_MATERIAL;
    else if (word_is(&w[2], "outline"))
        s->type = ST_OUTLINE;
    else
        fail(line + 1u, "an unknown setting type");
    /* The C name: capitals, each `.` and `-` made `_` (P12). */
    if (strlen(s->name) > CNAME_CHARS)
        fail(line + 1u, "a setting name whose C name is past 63 characters");
    for (k = 0; s->name[k]; k++) {
        char c = s->name[k];

        s->cname[k] = (char)(is_lower(c) ? c - 'a' + 'A'
                                         : (c == '.' || c == '-') ? '_' : c);
    }
    s->cname[k] = '\0';
    for (;;) {
        unsigned cur = next_statement(at);
        const Word *lw;
        unsigned ln, i;

        if (cur >= line_count)
            fail(line + 1u, "a block is not closed by `end`");
        lw = line_words(cur);
        ln = lines[cur].word_count;
        at = cur + 1u;
        if (word_is(&lw[0], "end") && ln == 1u)
            break;
        if (word_is(&lw[0], "default")) {
            if (s->has_default)
                fail(cur + 1u, "`default` is once in a setting");
            s->has_default = 1;
            s->default_line = cur + 1u;
            i = 1;
            switch (s->type) {
            case ST_BOOL:
                if (ln != 2u ||
                    (!word_is(&lw[1], "true") && !word_is(&lw[1], "false")))
                    fail(cur + 1u, "a bool's default is true or false");
                s->bool_default = word_is(&lw[1], "true");
                break;
            case ST_INT:
                if (ln != 2u || !word_integer(&lw[1], &s->int_default))
                    fail(cur + 1u, "an int's default is a signed number");
                break;
            case ST_ENUM:
                if (ln != 2u || !is_setting_word(&lw[1]))
                    fail(cur + 1u, "an enum's default is one of its words");
                word_copy(&lw[1], s->text_default, sizeof s->text_default);
                break;
            case ST_TEXT:
                if (ln != 2u || !word_is_string(&lw[1]))
                    fail(cur + 1u, "a text's default is a string");
                memcpy(s->text_default, lw[1].text, lw[1].length);
                s->text_default[lw[1].length] = '\0';
                break;
            case ST_COLOUR:
                if (ln != 2u || !is_colour_word(&lw[1]))
                    fail(cur + 1u, "a colour's default is 0xRRGGBB");
                word_copy(&lw[1], s->colour, sizeof s->colour);
                break;
            case ST_MATERIAL:
                if (ln != 3u || !is_colour_word(&lw[1]) ||
                    !word_number(&lw[2], &s->amount) ||
                    s->amount > PERCENT_MAX)
                    fail(cur + 1u, "a material's default is 0xRRGGBB and a "
                                   "percent from 0 to 100");
                word_copy(&lw[1], s->tint, sizeof s->tint);
                break;
            case ST_OUTLINE:
                fail(cur + 1u, "an outline has contours, no default");
            }
        } else if (word_is(&lw[0], "allowed")) {
            if (s->type != ST_ENUM)
                fail(cur + 1u, "`allowed` is for an enum");
            if (s->allowed_count || ln < 3u)
                fail(cur + 1u, "`allowed` is once, with two or more words");
            for (i = 1; i < ln; i++) {
                if (!is_setting_word(&lw[i]))
                    fail(cur + 1u, "an allowed word is [a-z][a-z0-9-]*");
                if (s->allowed_count >= ALLOWED_WORDS_MAX)
                    TABLE_FULL(cur + 1u, "allowed words");
                for (k = 0; k < s->allowed_count; k++)
                    if (strlen(s->allowed[k]) == lw[i].length &&
                        memcmp(s->allowed[k], lw[i].text, lw[i].length) == 0)
                        fail(cur + 1u, "a word allowed twice");
                word_copy(&lw[i], s->allowed[s->allowed_count],
                          sizeof s->allowed[0]);
                s->allowed_count++;
            }
        } else if (word_is(&lw[0], "min") || word_is(&lw[0], "max")) {
            int is_min = word_is(&lw[0], "min");

            if (s->type == ST_TEXT && !is_min) {
                if (ln != 2u || !word_number(&lw[1], &s->text_max) ||
                    s->text_max == 0)
                    fail(cur + 1u, "a text's max is a number above 0");
            } else if (s->type == ST_INT) {
                I64 v;

                if (ln != 2u || !word_integer(&lw[1], &v))
                    fail(cur + 1u, "an int's bound is a signed number");
                if (is_min) {
                    if (s->has_min)
                        fail(cur + 1u, "`min` is once in a setting");
                    s->has_min = 1;
                    s->int_min = v;
                } else {
                    if (s->has_max)
                        fail(cur + 1u, "`max` is once in a setting");
                    s->has_max = 1;
                    s->int_max = v;
                }
            } else {
                fail(cur + 1u, "`min` and `max` are for an int, and `max` "
                               "for a text");
            }
        } else if (word_is(&lw[0], "contour")) {
            if (s->type != ST_OUTLINE)
                fail(cur + 1u, "`contour` is for an outline");
            parse_contour(cur, s);
        } else {
            fail(cur + 1u, "an unknown line in a setting");
        }
    }
    switch (s->type) {
    case ST_BOOL:
    case ST_COLOUR:
    case ST_MATERIAL:
        if (!s->has_default)
            fail(line + 1u, "a setting without a default");
        break;
    case ST_INT:
        if (!s->has_default)
            fail(line + 1u, "a setting without a default");
        if (s->has_min && s->has_max && s->int_min > s->int_max)
            fail(line + 1u, "a min above its max");
        if ((s->has_min && s->int_default < s->int_min) ||
            (s->has_max && s->int_default > s->int_max))
            fail(line + 1u, "a default outside the bounds");
        break;
    case ST_ENUM:
        if (s->allowed_count < 2u)
            fail(line + 1u, "an enum allows two or more words");
        if (!s->has_default)
            fail(line + 1u, "a setting without a default");
        s->enum_default = ALLOWED_WORDS_MAX;
        for (k = 0; k < s->allowed_count; k++)
            if (strcmp(s->allowed[k], s->text_default) == 0)
                s->enum_default = k;
        if (s->enum_default == ALLOWED_WORDS_MAX)
            fail(s->default_line, "an enum's default is one of the words "
                                  "`allowed` gives");
        break;
    case ST_TEXT:
        if (s->text_max == 0)
            fail(line + 1u, "a text has a `max`");
        if (!s->has_default)
            fail(line + 1u, "a setting without a default");
        if (strlen(s->text_default) - 2u > s->text_max)
            fail(s->default_line, "a text's default is within its `max`");
        break;
    case ST_OUTLINE:
        if (s->contour_count == 0)
            fail(line + 1u, "an outline has one or more contours");
        break;
    }
    add_decl(line, D_SETTING, setting_count);
    setting_count++;
    return at - 1u;
}

/* ------------------------------------------------------------------------
   The description, top to bottom (P3). */

/* The description's file name without its directory and `.desc`, which
   the interface is named for (P3, rule 1). */
static void expected_interface_name(const char *path, char *name,
                                    unsigned capacity)
{
    const char *base = path, *p;
    size_t length;

    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    length = strlen(base);
    if (length >= 5u && strcmp(base + length - 5u, ".desc") == 0)
        length -= 5u;
    if (length + 1u > capacity)
        length = capacity - 1u;
    memcpy(name, base, length);
    name[length] = '\0';
}

/* Declare the channel's limits, which are operands of every description
   with no import (P4, rule 5). */
static void add_channel_limits(void)
{
    static const struct {
        const char *name;
        U64 value;
    } LIMITS[] = {{"CHANNEL_MAX_BYTES", CHANNEL_MAX_BYTES_VALUE},
                  {"CHANNEL_MAX_HANDLES", CHANNEL_MAX_HANDLES_VALUE},
                  {"CHANNEL_MAX_CAPACITY", CHANNEL_MAX_CAPACITY_VALUE}};
    unsigned i;

    for (i = 0; i < sizeof LIMITS / sizeof LIMITS[0]; i++) {
        Symbol *s = &symbols[symbol_count++];

        memcpy(s->name, LIMITS[i].name, strlen(LIMITS[i].name) + 1u);
        s->kind = SYM_NUMBER;
        s->value = LIMITS[i].value;
    }
}

/* Parse every statement of the description. */
static void parse_description(void)
{
    unsigned at = 0;
    char expected[NAME_CHARS + 2u];
    int seen_interface = 0;

    add_channel_limits();
    expected_interface_name(description_path, expected, sizeof expected);
    for (;;) {
        unsigned line = next_statement(at);
        const Word *w;
        unsigned n;

        if (line >= line_count)
            break;
        w = line_words(line);
        n = lines[line].word_count;
        if (!seen_interface) {
            Word name;

            if (!word_is(&w[0], "interface") || n != 2u || !is_name(&w[1]))
                fail(line + 1u, "the first statement is `interface` and a "
                                "name");
            name = w[1];
            if (name.length != strlen(expected) ||
                memcmp(name.text, expected, name.length) != 0)
                fail(line + 1u, "the interface is named for the "
                                "description's file name without .desc");
            interface_desc = need_desc(line);
            word_copy(&name, interface_name, sizeof interface_name);
            interface_line = line + 1u;
            seen_interface = 1;
            at = line + 1u;
            continue;
        }
        if (word_is(&w[0], "import"))
            at = parse_import(line) + 1u;
        else if (word_is(&w[0], "const")) {
            parse_const(line);
            at = line + 1u;
        } else if (word_is(&w[0], "rights")) {
            parse_rights(line);
            at = line + 1u;
        } else if (word_is(&w[0], "struct"))
            at = parse_struct(line) + 1u;
        else if (word_is(&w[0], "channel"))
            at = parse_channel(line) + 1u;
        else if (word_is(&w[0], "setting"))
            at = parse_setting(line) + 1u;
        else if (word_is(&w[0], "interface"))
            fail(line + 1u, "`interface` is the first statement alone");
        else if (word_is(&w[0], "end"))
            fail(line + 1u, "`end` closes no block");
        else
            fail(line + 1u, "an unknown statement");
    }
    if (!seen_interface)
        fail(line_count ? line_count : 1u, "no `interface` statement");
}

/* ------------------------------------------------------------------------
   Names the generator makes (P14, rule 4). */

static void put_case(Buf *b, const char *s, int upper)
{
    for (; *s; s++) {
        char c = *s;

        if (upper && is_lower(c))
            c = (char)(c - 'a' + 'A');
        buf_ch(b, c);
    }
}

/* The header's include guard, `NOVA_I_PROTOCOL_H` for interface i. */
static void put_guard(Buf *b)
{
    buf_put(b, "NOVA_");
    put_case(b, interface_name, 1);
    buf_put(b, "_PROTOCOL_H");
}

/* `I_C` or `i_c` for interface i and channel c. */
static void put_channel_name(Buf *b, const Channel *ch, int upper)
{
    put_case(b, interface_name, upper);
    buf_ch(b, '_');
    put_case(b, ch->name, upper);
}

/* `I_C_M` or `i_c_m`, then suffix as given. */
static void put_message_name(Buf *b, const Channel *ch, const Message *m,
                             int upper, const char *suffix)
{
    put_channel_name(b, ch, upper);
    buf_ch(b, '_');
    put_case(b, m->name, upper);
    buf_put(b, suffix);
}

/* The names of the parameters and locals of the generated functions
   (P3, rule 2: no generated function has two parameters of one name;
   here the locals too). */
static const char *const FIXED_NAMES[] = {
    "bytes", "byte_count", "handles", "handle_count", "transfers", "which",
    "transaction", "body", "out", "p", "at", "total", "i", "j", "n", "c",
    "k", "lo", "hi"};
#define FIXED_NAME_COUNT 19u

typedef struct {
    char name[GENERATED_CHARS + NAME_CHARS + 8u];
} GeneratedName;

#define GENERATED_MAX 8192u
static GeneratedName generated[GENERATED_MAX];
static unsigned generated_count;

/* Register a name the generator makes, refused if it is past 63
   characters, declared by the description, or made twice (P14, rule 4).
 */
static void register_generated(unsigned line, const Buf *name)
{
    unsigned i;

    if (name->length > GENERATED_CHARS)
        fail_name(line, "a name the generator makes is past 63 characters",
                  name->data);
    if (find_symbol(name->data))
        fail_name(line, "a name the generator makes that the description "
                        "declares",
                  name->data);
    if (is_kernel_name(name->data))
        fail_name(line, KERNEL_NAME_REFUSED, name->data);
    for (i = 0; i < generated_count; i++)
        if (strcmp(generated[i].name, name->data) == 0)
            fail_name(line, "a name two rules of the generator make",
                      name->data);
    if (generated_count >= GENERATED_MAX)
        TABLE_FULL(line, "generated names");
    memcpy(generated[generated_count].name, name->data, name->length + 1u);
    generated_count++;
}

/* ------------------------------------------------------------------------
   Resolving the messages of the channels (P7 to P10). */

/* Find the selector field `name` in structure s, among its own fields
   or in the structure that is its first field, and so on down. */
static int find_selector(unsigned line, const Struct *s, const char *name,
                         U64 base, Resolved *r)
{
    Field *f = struct_field(s, name, (unsigned)strlen(name));

    if (f && !f->is_struct && !f->is_array) {
        r->offset = base + f->offset;
        r->scalar = f->scalar;
        member_append(line, r->member, f->name, (unsigned)strlen(f->name));
        return 1;
    }
    if (s->field_count) {
        Field *first = &fields[s->first_field];

        if (first->is_struct && !first->is_array) {
            member_append(line, r->member, first->name,
                          (unsigned)strlen(first->name));
            return find_selector(line, &structs[first->struct_index], name,
                                 base + first->offset, r);
        }
    }
    return 0;
}

/* The transaction of a body: a u32 field `transaction`, its own first
   field or its first structure's (P7, rule 4). */
static void resolve_transaction(Message *m)
{
    memset(&m->transaction, 0, sizeof m->transaction);
    m->transaction.tail = -1;
    if (m->body == BODY_TRANSACTION) {
        m->has_transaction = 1;
        m->transaction.scalar = SC_U32;
        memcpy(m->transaction.member, "transaction", 12u);
    } else if (m->body == BODY_STRUCT) {
        const Struct *s = &structs[m->body_struct];
        U64 base = 0;

        while (s->field_count) {
            const Field *f = &fields[s->first_field];

            if (f->is_struct && !f->is_array) {
                member_append(m->line - 1u, m->transaction.member, f->name,
                              (unsigned)strlen(f->name));
                base += f->offset;
                s = &structs[f->struct_index];
                continue;
            }
            if (!f->is_struct && !f->is_array && f->scalar == SC_U32 &&
                strcmp(f->name, "transaction") == 0) {
                member_append(m->line - 1u, m->transaction.member, f->name,
                              (unsigned)strlen(f->name));
                m->transaction.offset = base + f->offset;
                m->transaction.scalar = SC_U32;
                m->has_transaction = 1;
            }
            break;
        }
    }
    if (!m->has_transaction && !m->first)
        fail(m->line, "a message's body begins with a u32 `transaction`; "
                      "only a `first` message may begin otherwise or hold "
                      "no bytes");
}

/* The most bytes of a message: its body and every tail at its maximum. */
static U64 message_bytes_max(const Message *m)
{
    U64 total = m->body_size;
    unsigned t;

    for (t = 0; t < m->tail_count; t++) {
        const Tail *tail = &m->tails[t];
        U64 add = 0;

        if (tail->shape == TAIL_STRUCT)
            add = tail->element_size;
        else if (tail->max.number > ~(U64)0 / tail->element_size)
            fail(m->line, "a message of more than 2^64 - 1 bytes");
        else
            add = tail->max.number * tail->element_size;
        if (total + add < total)
            fail(m->line, "a message of more than 2^64 - 1 bytes");
        total += add;
    }
    return total;
}

/* Add a parameter name to the set of one generated function, refused if
   it repeats one or names a local. */
static void add_param(unsigned line, char names[][NAME_CHARS + 8u],
                      unsigned *count, const char *name, unsigned length)
{
    unsigned i;

    if (length >= NAME_CHARS + 8u)
        fail(line, "a parameter name is too long");
    for (i = 0; i < *count; i++)
        if (strlen(names[i]) == length && memcmp(names[i], name, length) == 0)
            fail_name(line, "a tail whose parameter another parameter of "
                            "the generated functions has",
                      names[i]);
    for (i = 0; i < FIXED_NAME_COUNT; i++)
        if (strlen(FIXED_NAMES[i]) == length &&
            memcmp(FIXED_NAMES[i], name, length) == 0)
            fail_name(line, "a tail whose parameter is a name the "
                            "generated functions use",
                      FIXED_NAMES[i]);
    memcpy(names[*count], name, length);
    names[*count][length] = '\0';
    (*count)++;
}

/* The parameters of a message's encoding, each distinct (P3, rule 2). */
static void check_params(const Message *m)
{
    char names[TAILS_PER_MESSAGE_MAX * 2u + 4u][NAME_CHARS + 8u];
    unsigned count = 0, t;

    for (t = 0; t < m->tail_count; t++) {
        const Tail *tail = &m->tails[t];
        unsigned length = (unsigned)strlen(tail->name);
        char counted[NAME_CHARS + 8u];

        add_param(tail->line, names, &count, tail->name, length);
        if (tail->shape != TAIL_STRUCT) {
            memcpy(counted, tail->name, length);
            memcpy(counted + length, "_count", 7u);
            add_param(tail->line, names, &count, counted, length + 6u);
        }
    }
}

/* Resolve every channel's messages: selectors, transactions, handle
   positions, replies and sizes. */
static void resolve_channels(void)
{
    unsigned c, k, t;

    for (c = 0; c < channel_count; c++) {
        Channel *ch = &channels[c];
        int selector_known = 0;
        unsigned first_to[2] = {0, 0}, other_to[2] = {0, 0};

        if (ch->message_count == 0)
            fail(ch->line, "a channel with no message");
        for (k = 0; k < ch->message_count; k++) {
            Message *m = messages[ch->first_message + k];
            unsigned direction = m->kind == M_REQUEST ? 0u : 1u;
            U64 position = 0;

            if (m->kind == M_REQUEST)
                ch->has_requests = 1;
            else if (m->kind == M_REPLY)
                ch->has_replies = 1;
            else
                ch->has_events = 1;
            if (m->body == BODY_NONE && !m->first)
                fail(m->line, "only a `first` message has `body none`");
            resolve_transaction(m);

            /* The selector (P7, rule 2). */
            if (ch->has_select && m->kind != M_REPLY && m->body != BODY_NONE) {
                Resolved r;

                memset(&r, 0, sizeof r);
                r.tail = -1;
                if (m->body == BODY_STRUCT &&
                    find_selector(m->line - 1u,
                                  &structs[m->body_struct], ch->select, 0,
                                  &r)) {
                    if (r.scalar != SC_U16 && r.scalar != SC_U32)
                        fail(m->line, "a selector is a u16 or u32 field");
                    m->has_selector = 1;
                    m->selector = r;
                    if (!selector_known) {
                        selector_known = 1;
                        ch->select_offset = r.offset;
                        ch->select_scalar = r.scalar;
                    } else if (ch->select_offset != r.offset ||
                               ch->select_scalar != r.scalar) {
                        fail(m->line, "a selector at another offset or of "
                                      "another size than the channel's "
                                      "other messages'");
                    }
                }
            }
            if (m->kind != M_REPLY && ch->has_select) {
                if (m->has_selector && !m->has_cname)
                    fail(m->line, "a message that holds the selector has a "
                                  "C name and a value");
                if (!m->has_selector && !m->first)
                    fail(m->line, "a request or event without the "
                                  "selector, which only a `first` message "
                                  "may lack");
                if (!m->has_selector && m->has_cname)
                    fail(m->line, "a C name and a value for a message that "
                                  "holds no selector");
                if (m->has_cname) {
                    unsigned other;

                    if (m->value >= UNNAMED_VALUE ||
                        m->value > scalar_max(m->selector.scalar))
                        fail(m->line, "a value the selector cannot hold or "
                                      "0xFFFFFFFF");
                    for (other = 0; other < k; other++) {
                        const Message *o =
                            messages[ch->first_message + other];

                        if (o->kind == m->kind && o->has_cname &&
                            o->value == m->value)
                            fail(m->line, "a value used twice in a "
                                          "direction");
                    }
                }
            } else if (m->kind != M_REPLY && m->has_cname) {
                fail(m->line, "a C name and a value on a channel without "
                              "`select`");
            }
            /* Without `select`: one message a direction, or a first
               message and one other (P7, rule 2). */
            if (!ch->has_select) {
                if (m->first)
                    first_to[direction]++;
                else
                    other_to[direction]++;
            }

            /* Handles: positions, counts, and the macros that name them
               (P9). */
            m->handles_min = 0;
            m->handles_max = 0;
            for (t = 0; t < m->handle_count; t++) {
                HandleLine *h = &m->handles[t];
                Symbol *sym;

                if (t + 1u < m->handle_count &&
                    h->min_count != h->max_count)
                    fail(h->line, "only a message's last `handle` line may "
                                  "vary in number");
                if (h->max_count > CHANNEL_MAX_HANDLES_VALUE - position)
                    fail(m->line, "a message that carries more than "
                                  "CHANNEL_MAX_HANDLES handles");
                h->position = position;
                position += h->max_count;
                m->handles_min += h->min_count;
                m->handles_max += h->max_count;
                if (h->has_index) {
                    sym = find_symbol(h->index_name);
                    sym->value = h->position;
                }
            }
            if (m->has_handles_macro)
                find_symbol(m->handles_name)->value = m->handles_max;

            /* Sizes. */
            m->bytes_max = message_bytes_max(m);
            if (m->bytes_max > CHANNEL_MAX_BYTES_VALUE)
                fail(m->line, "a message of more than CHANNEL_MAX_BYTES "
                              "bytes");
            if (m->kind == M_REQUEST) {
                if (m->bytes_max > ch->server_bytes_max)
                    ch->server_bytes_max = m->bytes_max;
                if (m->handles_max > ch->server_handles_max)
                    ch->server_handles_max = m->handles_max;
            } else {
                if (m->bytes_max > ch->client_bytes_max)
                    ch->client_bytes_max = m->bytes_max;
                if (m->handles_max > ch->client_handles_max)
                    ch->client_handles_max = m->handles_max;
            }
            check_params(m);
        }
        for (k = 0; k < 2u; k++)
            if (first_to[k] > 1u || other_to[k] > 1u)
                fail(ch->line, "a channel without `select` holds one "
                               "message a direction, or a `first` message "
                               "and one other");
        if (ch->has_select && !selector_known)
            fail(ch->select_line, "no message of the channel holds the "
                                  "selector");
        if (ch->has_requests && ch->server.kind == POLICY_NONE)
            fail(ch->line, "a channel whose server receives requests needs "
                           "`invalid server`");
        if ((ch->has_replies || ch->has_events) &&
            ch->client.kind == POLICY_NONE)
            fail(ch->line, "a channel whose client receives replies or "
                           "events needs `invalid client`");
    }

    /* A command word names one request (P13, rule 2). */
    for (k = 0; k < message_total; k++)
        for (t = k + 1u; t < message_total; t++)
            if (messages[k]->has_command && messages[t]->has_command &&
                strcmp(messages[k]->command, messages[t]->command) == 0)
                fail_name(messages[t]->line, "a command named by two requests",
                          messages[t]->command);
    /* A message's field line may give a tail's count a min alone, the
       tail's own maximum being its maximum (P8, rule 2). */
    for (k = 0; k < message_total; k++) {
        const Message *m = messages[k];
        unsigned r;

        for (r = 0; r < m->rule_count; r++)
            for (t = 0; t < m->tail_count; t++)
                if (m->tails[t].shape == TAIL_COUNTED &&
                    m->tails[t].count.tail == m->rules[r].at.tail &&
                    m->tails[t].count.offset == m->rules[r].at.offset &&
                    (m->rules[r].is_tails ||
                     m->rules[r].constraint.kind != CON_RANGE ||
                     m->rules[r].constraint.has_max))
                    fail(m->rules[r].line, "a tail's count takes a min alone, "
                                           "its maximum being the tail's");
    }

    /* Replies (P7, rule 5). */
    for (c = 0; c < channel_count; c++) {
        Channel *ch = &channels[c];

        for (k = 0; k < ch->message_count; k++) {
            Message *m = messages[ch->first_message + k];
            const Channel *target = ch;
            unsigned r;
            int found = -1;

            if (m->kind != M_REQUEST || !m->has_reply)
                continue;
            if (m->reply_on) {
                unsigned d;

                target = 0;
                for (d = 0; d < channel_count; d++)
                    if (strcmp(channels[d].name, m->reply_channel) == 0)
                        target = &channels[d];
                if (!target || target == ch)
                    fail(m->line, "`on` names another channel of the "
                                  "description");
            }
            for (r = 0; r < target->message_count; r++) {
                Message *rm = messages[target->first_message + r];

                if (rm->kind == M_REPLY && strcmp(rm->name, m->reply_name) == 0)
                    found = (int)(target->first_message + r);
            }
            if (found < 0)
                fail_name(m->line, "a reply no message of the channel is "
                                   "named",
                          m->reply_name);
            if (m->reply_on) {
                unsigned h;
                int carries_channel = 0;

                if (!messages[found]->first)
                    fail(m->line, "a reply on another channel is that "
                                  "channel's `first` message");
                for (h = 0; h < m->handle_count; h++)
                    carries_channel = carries_channel ||
                                      strcmp(TYPES[m->handles[h].type].name,
                                             "channel") == 0;
                if (!carries_channel)
                    fail(m->line, "a request whose reply is on another "
                                  "channel carries a channel end");
            }
            m->reply_message = found;
            messages[found]->named_by_request = 1;
        }
    }
    for (c = 0; c < channel_count; c++) {
        Channel *ch = &channels[c];

        for (k = 0; k < ch->message_count; k++) {
            Message *m = messages[ch->first_message + k];

            if (m->kind == M_REPLY && !m->named_by_request)
                fail(m->line, "a reply no request names");
        }
        if (ch->server.kind == POLICY_ANSWER) {
            const Message *reply = 0;

            for (k = 0; k < ch->message_count; k++) {
                const Message *m = messages[ch->first_message + k];

                if (m->kind == M_REQUEST && !m->has_reply)
                    fail(ch->server.line, "`answer` needs every request of "
                                          "the channel to have a reply");
                if (m->kind == M_REPLY &&
                    strcmp(m->name, ch->server.reply) == 0)
                    reply = m;
            }
            if (!reply)
                fail(ch->server.line, "`answer` names a reply of the "
                                      "channel");
            if (!reply->has_status)
                fail(ch->server.line, "the reply `answer` names has a "
                                      "`status`");
        }
    }
}

/* The names the generator makes for each channel and message, registered
   so that no two are alike and none is one the description declares
   (P14, rule 4). */
static void register_all_generated(void)
{
    unsigned c, k, s;
    Buf name;

    memset(&name, 0, sizeof name);
    put_guard(&name);
    register_generated(interface_line, &name);
    for (c = 0; c < channel_count; c++) {
        Channel *ch = &channels[c];
        int request_dispatch = 0, event_dispatch = 0;

        if (ch->has_requests) {
            name.length = 0;
            put_channel_name(&name, ch, 1);
            buf_put(&name, "_TO_SERVER_BYTES_MAX");
            register_generated(ch->line, &name);
            name.length = 0;
            put_channel_name(&name, ch, 1);
            buf_put(&name, "_TO_SERVER_HANDLES_MAX");
            register_generated(ch->line, &name);
        }
        if (ch->has_replies || ch->has_events) {
            name.length = 0;
            put_channel_name(&name, ch, 1);
            buf_put(&name, "_TO_CLIENT_BYTES_MAX");
            register_generated(ch->line, &name);
            name.length = 0;
            put_channel_name(&name, ch, 1);
            buf_put(&name, "_TO_CLIENT_HANDLES_MAX");
            register_generated(ch->line, &name);
        }
        if (ch->has_select) {
            name.length = 0;
            put_channel_name(&name, ch, 1);
            buf_put(&name, "_UNNAMED");
            register_generated(ch->line, &name);
        }
        for (k = 0; k < ch->message_count; k++) {
            const Message *m = messages[ch->first_message + k];

            if (m->retired)
                continue;
            name.length = 0;
            put_message_name(&name, ch, m, 0, "_check");
            register_generated(m->line, &name);
            name.length = 0;
            put_message_name(&name, ch, m, 0, "_encode");
            register_generated(m->line, &name);
            for (s = 0; s < m->tail_count; s++) {
                name.length = 0;
                put_message_name(&name, ch, m, 0, "_");
                buf_put(&name, m->tails[s].name);
                register_generated(m->tails[s].line, &name);
            }
            if (ch->has_select && m->has_selector && !m->first) {
                if (m->kind == M_REQUEST)
                    request_dispatch = 1;
                else
                    event_dispatch = 1;
            }
        }
        if (request_dispatch) {
            name.length = 0;
            put_channel_name(&name, ch, 0);
            buf_put(&name, "_request_check");
            register_generated(ch->line, &name);
        }
        if (event_dispatch) {
            name.length = 0;
            put_channel_name(&name, ch, 0);
            buf_put(&name, "_event_check");
            register_generated(ch->line, &name);
        }
    }
    for (s = 0; s < setting_count; s++) {
        const Setting *st = &settings[s];
        unsigned w;

        name.length = 0;
        buf_put(&name, st->cname);
        switch (st->type) {
        case ST_BOOL:
        case ST_INT:
        case ST_TEXT:
        case ST_COLOUR:
            buf_put(&name, "_DEFAULT");
            register_generated(st->line, &name);
            break;
        case ST_ENUM:
            for (w = 0; w < st->allowed_count; w++) {
                unsigned q;

                name.length = 0;
                buf_put(&name, st->cname);
                buf_ch(&name, '_');
                for (q = 0; st->allowed[w][q]; q++)
                    buf_ch(&name, st->allowed[w][q] == '-'
                                      ? '_'
                                      : (char)(is_lower(st->allowed[w][q])
                                                   ? st->allowed[w][q] - 32
                                                   : st->allowed[w][q]));
                register_generated(st->line, &name);
            }
            name.length = 0;
            buf_put(&name, st->cname);
            buf_put(&name, "_DEFAULT");
            register_generated(st->line, &name);
            break;
        case ST_MATERIAL:
            buf_put(&name, "_DEFAULT_TINT");
            register_generated(st->line, &name);
            name.length = 0;
            buf_put(&name, st->cname);
            buf_put(&name, "_DEFAULT_AMOUNT");
            register_generated(st->line, &name);
            break;
        case ST_OUTLINE: {
            static const char *const SUFFIXES[] = {
                "_DEFAULT_POINTS", "_DEFAULT_FLAGS", "_DEFAULT_ENDS",
                "_DEFAULT_POINT_COUNT", "_DEFAULT_CONTOURS"};

            for (w = 0; w < 5u; w++) {
                name.length = 0;
                buf_put(&name, st->cname);
                buf_put(&name, SUFFIXES[w]);
                register_generated(st->line, &name);
            }
            break;
        }
        }
    }
    free(name.data);
}

/* ------------------------------------------------------------------------
   The header (P14): comments and the declarations of the types. */

static Buf header;

/* The most lines one comment is made of. */
#define DOC_LINES_MAX 512u

/* The most characters of a line of a generated sentence in a comment. */
#define DOC_SENTENCE_COLUMNS 66u

typedef struct {
    const char *text;
    unsigned length;
} DocLine;

/* Append a Doxygen block comment: its first line after the opening, each
   later line indented four more than the comment, the last closed. The
   lines are an optional fixed sentence, then the lines of d above its
   declaration, or its trailing comment as one line (P14, rule 7). */
static void put_doc(Buf *b, unsigned indent, const char *sentence,
                    const Desc *d)
{
    DocLine doc[DOC_LINES_MAX];
    unsigned count = 0, i;

    if (sentence) {
        const char *at = sentence;

        /* A fixed sentence is carried over lines at its spaces. */
        while (*at) {
            const char *end = at;
            const char *last_space = 0;

            while (*end && (unsigned)(end - at) < DOC_SENTENCE_COLUMNS) {
                if (*end == ' ')
                    last_space = end;
                end++;
            }
            if (*end && last_space)
                end = last_space;
            else
                while (*end && *end != ' ')
                    end++;
            if (count >= DOC_LINES_MAX)
                fail(d->first + 1u, "a description of too many lines");
            doc[count].text = at;
            doc[count].length = (unsigned)(end - at);
            count++;
            at = *end ? end + 1 : end;
        }
    }
    for (i = 0; i < d->count; i++) {
        if (count >= DOC_LINES_MAX)
            fail(d->first + 1u, "a description of too many lines");
        doc[count].text = lines[d->first + i].comment;
        doc[count].length = lines[d->first + i].comment_length;
        count++;
    }
    if (d->trail_length) {
        doc[count].text = d->trail;
        doc[count].length = d->trail_length;
        count++;
    }
    for (i = 0; i < count; i++) {
        buf_spaces(b, indent);
        if (i == 0) {
            buf_put(b, "/**");
            if (doc[i].length)
                buf_ch(b, ' ');
        } else if (doc[i].length) {
            buf_spaces(b, INDENT);
        }
        buf_putn(b, doc[i].text, doc[i].length);
        if (i + 1u == count)
            buf_put(b, " */");
        buf_ch(b, '\n');
    }
}

/* Append a Doxygen comment after the declaration, for a trailing
   description. */
static void put_doc_after(Buf *b, const Desc *d)
{
    buf_put(b, " /**< ");
    buf_putn(b, d->trail, d->trail_length);
    buf_put(b, " */");
}

/* Append a string literal's text with each `?` as `\?`, so that no two
   of them form a trigraph under a compiler that reads them (P14, rule
   7). */
static void put_string_text(Buf *b, const char *text)
{
    for (; *text; text++) {
        if (*text == '?')
            buf_ch(b, '\\');
        buf_ch(b, *text);
    }
}

/* Append `#define NAME TEXT`, with a long text carried over lines at its
   spaces with a backslash, the continuation lines indented. A string is
   never carried over. */
static void put_define(Buf *b, const char *name, const char *text,
                       int is_string)
{
    const char *p = text;
    size_t start = b->length;

    buf_put(b, "#define ");
    buf_put(b, name);
    buf_ch(b, ' ');
    if (is_string) {
        put_string_text(b, text);
        return;
    }
    while (*p) {
        const char *end = p;

        while (*end && *end != ' ')
            end++;
        if (p != text && b->length - start > 0 &&
            buf_column(b) + (size_t)(end - p) + 3u > WRAP_COLUMN) {
            buf_put(b, " \\\n");
            buf_spaces(b, INDENT);
        } else if (p != text) {
            buf_ch(b, ' ');
        }
        buf_putn(b, p, (size_t)(end - p));
        p = *end ? end + 1 : end;
    }
}

/* Append a declaration made of a #define with its description (P14, rule
   7): above it, or after it on its line. */
static void put_described_define(Buf *b, const Desc *d, const char *name,
                                 const char *text, int is_string)
{
    if (d->count)
        put_doc(b, 0, 0, d);
    put_define(b, name, text, is_string);
    if (!d->count && d->trail_length)
        put_doc_after(b, d);
    buf_ch(b, '\n');
}

/* Append a #define of value in decimal, with a fixed sentence and the
   description d above it. */
static void put_decimal_define(Buf *b, const Desc *d, const char *sentence,
                               const char *name, U64 value)
{
    Buf text;

    memset(&text, 0, sizeof text);
    buf_u64(&text, value);
    put_doc(b, 0, sentence, d);
    put_define(b, name, text.data, 0);
    buf_ch(b, '\n');
    free(text.data);
}

/* `_Static_assert(CONDITION, "MESSAGE");` for the condition `left == value`
 */
static void put_assert_equal(Buf *b, const char *left, U64 value,
                             const char *message)
{
    buf_put(b, "_Static_assert(");
    buf_put(b, left);
    buf_put(b, " == ");
    buf_u64(b, value);
    buf_put(b, ", \"");
    buf_put(b, message);
    buf_put(b, "\");\n");
}

/* The field's C declaration, `TYPE name;` or `TYPE name[N];`. */
static void put_field_declaration(Buf *b, const Field *f)
{
    buf_put(b, f->is_struct ? structs[f->struct_index].name
                            : SCALARS[f->scalar].ctype);
    buf_ch(b, ' ');
    buf_put(b, f->name);
    if (f->is_array) {
        buf_ch(b, '[');
        buf_put(b, f->array_size.text);
        buf_ch(b, ']');
    }
    buf_ch(b, ';');
}

/* The assertions that give a structure's size and each field's offset
   (P6, rule 2). */
static void put_layout_asserts(Buf *b, const Struct *s)
{
    unsigned i;
    Buf left;

    memset(&left, 0, sizeof left);
    buf_put(&left, "sizeof(");
    buf_put(&left, s->name);
    buf_ch(&left, ')');
    put_assert_equal(b, left.data, s->size, LAYOUT_MESSAGE);
    for (i = 0; i < s->field_count; i++) {
        const Field *f = &fields[s->first_field + i];

        left.length = 0;
        buf_put(&left, "offsetof(");
        buf_put(&left, s->name);
        buf_put(&left, ", ");
        buf_put(&left, f->name);
        buf_ch(&left, ')');
        put_assert_equal(b, left.data, f->offset, LAYOUT_MESSAGE);
    }
    free(left.data);
}

/* A structure as a typedef, its fields' descriptions above them or after
   them in one column, and its assertions (P14, rules 1 and 7). */
static void put_struct(Buf *b, const Struct *s)
{
    unsigned i;
    size_t column = 0;
    Buf decl;

    memset(&decl, 0, sizeof decl);
    for (i = 0; i < s->field_count; i++) {
        const Field *f = &fields[s->first_field + i];

        if (!f->desc.count && f->desc.trail_length) {
            size_t length;

            decl.length = 0;
            put_field_declaration(&decl, f);
            length = INDENT + decl.length + 1u;
            if (length > column)
                column = length;
        }
    }
    put_doc(b, 0, 0, &s->desc);
    buf_put(b, "typedef struct {\n");
    for (i = 0; i < s->field_count; i++) {
        const Field *f = &fields[s->first_field + i];

        if (f->desc.count)
            put_doc(b, INDENT, 0, &f->desc);
        buf_spaces(b, INDENT);
        put_field_declaration(b, f);
        if (!f->desc.count && f->desc.trail_length) {
            decl.length = 0;
            put_field_declaration(&decl, f);
            buf_spaces(b, (unsigned)(column - INDENT - decl.length));
            buf_put(b, "/**< ");
            buf_putn(b, f->desc.trail, f->desc.trail_length);
            buf_put(b, " */");
        }
        buf_ch(b, '\n');
    }
    buf_put(b, "} ");
    buf_put(b, s->name);
    buf_put(b, ";\n\n");
    put_layout_asserts(b, s);
    free(decl.data);
}

/* A `rights` constant: the macros of its rights joined by `|` (P4, rule
   4). */
static void put_rights_decl(Buf *b, const RightsDecl *r)
{
    Buf text;

    memset(&text, 0, sizeof text);
    buf_ch(&text, '(');
    put_rights(&text, &r->list);
    buf_ch(&text, ')');
    put_described_define(b, &r->desc, symbols[r->symbol].name, text.data, 0);
    free(text.data);
}

/* ------------------------------------------------------------------------
   Settings' defaults (P12). */

/* Append name, made of word with each `-` a `_` and capitals. */
static void put_word_macro(Buf *b, const char *prefix, const char *word)
{
    unsigned i;

    buf_put(b, prefix);
    buf_ch(b, '_');
    for (i = 0; word[i]; i++)
        buf_ch(b, word[i] == '-'
                      ? '_'
                      : (char)(is_lower(word[i]) ? word[i] - 32 : word[i]));
}

/* An initializer of numbers, `{ a, b, ... }`, carried over lines. */
static void put_initializer(Buf *b, const char *name, const I64 *values,
                            unsigned count, unsigned stride)
{
    unsigned i;
    size_t start = b->length;

    buf_put(b, "#define ");
    buf_put(b, name);
    buf_put(b, " { ");
    for (i = 0; i < count; i++) {
        Buf number;

        memset(&number, 0, sizeof number);
        buf_i64(&number, values[(size_t)i * stride]);
        if (i && buf_column(b) + number.length + 4u > WRAP_COLUMN) {
            buf_put(b, " \\\n");
            buf_spaces(b, INDENT);
        } else if (i) {
            buf_ch(b, ' ');
        }
        buf_put(b, number.data);
        buf_put(b, i + 1u == count ? " }" : ",");
        free(number.data);
    }
    (void)start;
    buf_ch(b, '\n');
}

/* Append an initializer of count unsigned numbers as a #define. */
static void put_flag_initializer(Buf *b, const char *name,
                                 const unsigned *values, unsigned count)
{
    I64 *copy = (I64 *)xalloc(sizeof(I64) * (count + 1u));
    unsigned i;

    for (i = 0; i < count; i++)
        copy[i] = (I64)values[i];
    put_initializer(b, name, copy, count, 1u);
    free(copy);
}

/* A setting's defaults as macros, each with the setting's description
   (P12). */
static void put_setting(Buf *b, const Setting *s)
{
    Buf name, text;
    unsigned i;

    memset(&name, 0, sizeof name);
    memset(&text, 0, sizeof text);
    switch (s->type) {
    case ST_BOOL:
        buf_put(&name, s->cname);
        buf_put(&name, "_DEFAULT");
        put_doc(b, 0, "The default of the setting, 1 or 0.", &s->desc);
        put_define(b, name.data, s->bool_default ? "1" : "0", 0);
        buf_ch(b, '\n');
        break;
    case ST_INT:
        buf_put(&name, s->cname);
        buf_put(&name, "_DEFAULT");
        buf_i64(&text, s->int_default);
        put_doc(b, 0, "The default of the setting.", &s->desc);
        put_define(b, name.data, text.data, 0);
        buf_ch(b, '\n');
        break;
    case ST_ENUM:
        for (i = 0; i < s->allowed_count; i++) {
            name.length = 0;
            text.length = 0;
            put_word_macro(&name, s->cname, s->allowed[i]);
            buf_u64(&text, i);
            put_doc(b, 0, "A value of the setting.", &s->desc);
            put_define(b, name.data, text.data, 0);
            buf_ch(b, '\n');
        }
        name.length = 0;
        text.length = 0;
        buf_put(&name, s->cname);
        buf_put(&name, "_DEFAULT");
        put_word_macro(&text, s->cname, s->allowed[s->enum_default]);
        put_doc(b, 0, "The default of the setting.", &s->desc);
        put_define(b, name.data, text.data, 0);
        buf_ch(b, '\n');
        break;
    case ST_TEXT:
        buf_put(&name, s->cname);
        buf_put(&name, "_DEFAULT");
        put_doc(b, 0, "The default of the setting.", &s->desc);
        put_define(b, name.data, s->text_default, 1);
        buf_ch(b, '\n');
        break;
    case ST_COLOUR:
        buf_put(&name, s->cname);
        buf_put(&name, "_DEFAULT");
        put_doc(b, 0, "The default of the setting.", &s->desc);
        put_define(b, name.data, s->colour, 0);
        buf_ch(b, '\n');
        break;
    case ST_MATERIAL:
        buf_put(&name, s->cname);
        buf_put(&name, "_DEFAULT_TINT");
        put_doc(b, 0, "The default tint of the setting.", &s->desc);
        put_define(b, name.data, s->tint, 0);
        buf_ch(b, '\n');
        name.length = 0;
        buf_put(&name, s->cname);
        buf_put(&name, "_DEFAULT_AMOUNT");
        buf_u64(&text, s->amount);
        put_doc(b, 0, "The default percent of the tint mixed over what lies "
                      "beneath.",
                &s->desc);
        put_define(b, name.data, text.data, 0);
        buf_ch(b, '\n');
        break;
    case ST_OUTLINE: {
        I64 *flat = (I64 *)xalloc(sizeof(I64) * 2u * (s->point_count + 1u));
        I64 *ends = (I64 *)xalloc(sizeof(I64) * (s->contour_count + 1u));

        for (i = 0; i < s->point_count; i++) {
            flat[2u * i] = s->points[i][0];
            flat[2u * i + 1u] = s->points[i][1];
        }
        for (i = 0; i < s->contour_count; i++)
            ends[i] = (I64)s->ends[i];
        buf_put(&name, s->cname);
        buf_put(&name, "_DEFAULT_POINTS");
        put_doc(b, 0, "The coordinates of the outline's points, x then y, in "
                      "256ths of a point.",
                &s->desc);
        put_initializer(b, name.data, flat, 2u * s->point_count, 1u);
        buf_ch(b, '\n');
        name.length = 0;
        buf_put(&name, s->cname);
        buf_put(&name, "_DEFAULT_FLAGS");
        put_doc(b, 0, "The kind of each of the outline's points, 1 on the "
                      "curve, 0 a quadratic's control point, 2 a cubic's.",
                &s->desc);
        put_flag_initializer(b, name.data, s->flags, s->point_count);
        buf_ch(b, '\n');
        name.length = 0;
        buf_put(&name, s->cname);
        buf_put(&name, "_DEFAULT_ENDS");
        put_doc(b, 0, "The index one past each contour's last point.",
                &s->desc);
        put_initializer(b, name.data, ends, s->contour_count, 1u);
        buf_ch(b, '\n');
        name.length = 0;
        text.length = 0;
        buf_put(&name, s->cname);
        buf_put(&name, "_DEFAULT_POINT_COUNT");
        buf_u64(&text, s->point_count);
        put_doc(b, 0, "The number of the outline's points.", &s->desc);
        put_define(b, name.data, text.data, 0);
        buf_ch(b, '\n');
        name.length = 0;
        text.length = 0;
        buf_put(&name, s->cname);
        buf_put(&name, "_DEFAULT_CONTOURS");
        buf_u64(&text, s->contour_count);
        put_doc(b, 0, "The number of the outline's contours.", &s->desc);
        put_define(b, name.data, text.data, 0);
        buf_ch(b, '\n');
        free(flat);
        free(ends);
        break;
    }
    }
    buf_ch(b, '\n');
    free(name.data);
    free(text.data);
}

/* ------------------------------------------------------------------------
   The header (P14): the functions of each message. */

/* What one generated function is being written: its statements, which
   locals they used, and the names of the views of the body. check
   functions read the body through `body`, an encoding through `out`. */
typedef struct {
    Buf code;
    const Message *m;
    const Channel *ch;
    const char *view;
    int encode;
    unsigned indent;
    int used_view;
    int used_p;
    int used_at;
    int used_i;
    int used_j;
    int used_n;
    int used_utf8;
    int used_handles;
} Gen;

#define CHECK_VIEW "body"
#define ENCODE_VIEW "out"
#define FAIL_STATEMENT "return ERR_INVALID_ARGS;"

/* Indent the line being written to the function's depth and extra more
   levels. */
static void g_indent(Gen *g, unsigned extra)
{
    buf_spaces(&g->code, (g->indent + extra) * INDENT);
}

/* Append one statement or declaration line. */
static void g_line(Gen *g, const char *text)
{
    g_indent(g, 0);
    buf_put(&g->code, text);
    buf_ch(&g->code, '\n');
}

/* `if (CONDITION)` and a refusal. A condition with line breaks carries
   them aligned under its first character. */
static void g_if(Gen *g, const Buf *condition)
{
    g_indent(g, 0);
    buf_put(&g->code, "if (");
    buf_put(&g->code, condition->data);
    buf_put(&g->code, ")\n");
    g_indent(g, 1);
    buf_put(&g->code, FAIL_STATEMENT "\n");
}

/* A cast of a value as the description wrote it: `(T)7` for a number
   and `(T)(NAME)` for a name. */
static void put_cast_value(Buf *b, const char *type, const Value *v)
{
    buf_ch(b, '(');
    buf_put(b, type);
    buf_ch(b, ')');
    if (is_digit(v->text[0])) {
        buf_put(b, v->text);
    } else {
        buf_ch(b, '(');
        buf_put(b, v->text);
        buf_ch(b, ')');
    }
}

/* Append a load of scalar s from p[base + offset], bytes in
   little-endian order, as a 64-bit value: unsigned, or signed for i32
   and i64. base may be empty. */
static void put_load(Buf *b, Scalar s, const char *base, U64 offset)
{
    unsigned size = SCALARS[s].size, i;

    if (SCALARS[s].is_signed)
        buf_put(b, size == 4u ? "(INT64)(INT32)(UINT32)(" : "(INT64)(");
    else
        buf_ch(b, '(');
    for (i = 0; i < size; i++) {
        if (i)
            buf_put(b, " | ");
        buf_put(b, "(UINT64)p[");
        if (base[0]) {
            buf_put(b, base);
            buf_put(b, " + ");
        }
        buf_u64(b, offset + i);
        buf_ch(b, ']');
        if (i) {
            buf_put(b, " << ");
            buf_u64(b, 8u * i);
        }
    }
    buf_ch(b, ')');
}

static void put_tail_start(Gen *g, Buf *b, const Message *m, unsigned t);

/* Append the value of the field r resolves to, as a 64-bit value of type
   (`UINT64` or `INT64`), read from the body or from a structure tail of
   the message. */
static void put_read(Gen *g, Buf *b, const Resolved *r, const char *type)
{
    const Message *m = g->m;

    if (r->tail < 0) {
        g->used_view = 1;
        buf_ch(b, '(');
        buf_put(b, type);
        buf_ch(b, ')');
        if (m->body == BODY_TRANSACTION) {
            buf_put(b, g->view);
            buf_put(b, "[0]");
        } else {
            buf_put(b, g->view);
            buf_put(b, "->");
            buf_put(b, r->member);
        }
    } else {
        Buf start;

        memset(&start, 0, sizeof start);
        put_tail_start(g, &start, m, (unsigned)r->tail);
        g->used_p = 1;
        if (SCALARS[r->scalar].is_signed || strcmp(type, "UINT64") == 0) {
            put_load(b, r->scalar, start.data, r->offset);
        } else {
            buf_put(b, "(INT64)");
            put_load(b, r->scalar, start.data, r->offset);
        }
        free(start.data);
    }
}

/* Append where tail t of m starts: the body's size and the bytes of
   each tail before it. */
static void put_tail_start(Gen *g, Buf *b, const Message *m, unsigned t)
{
    unsigned j;

    buf_u64(b, m->body_size);
    for (j = 0; j < t; j++) {
        const Tail *tail = &m->tails[j];

        buf_put(b, " + ");
        if (tail->shape == TAIL_STRUCT) {
            buf_u64(b, tail->element_size);
        } else {
            buf_put(b, "(");
            put_read(g, b, &tail->count, "UINT64");
            buf_put(b, ")");
            if (tail->element_size != 1u) {
                buf_put(b, " * ");
                buf_u64(b, tail->element_size);
            }
        }
    }
}

/* The condition under which a constraint on x (typed as `UINT64` or
   `INT64`) is broken (P6, rule 3). */
static void put_broken(Buf *b, const char *x, const char *type,
                       const Constraint *c, unsigned align)
{
    unsigned i;

    switch (c->kind) {
    case CON_NONE:
        break;
    case CON_EQ:
        buf_put(b, x);
        buf_put(b, " != ");
        put_cast_value(b, type, &c->a);
        break;
    case CON_IN:
        buf_put(b, "!(");
        for (i = 0; i < c->in_count; i++) {
            if (i) {
                buf_put(b, " ||\n");
                buf_spaces(b, align + 2u);
            }
            buf_put(b, x);
            buf_put(b, " == ");
            put_cast_value(b, type, &c->in[i]);
        }
        buf_ch(b, ')');
        break;
    case CON_MASK:
        buf_put(b, "(");
        buf_put(b, x);
        buf_put(b, " & ~");
        put_cast_value(b, type, &c->a);
        buf_put(b, ") != 0");
        break;
    case CON_RANGE:
        if (c->has_min) {
            buf_put(b, x);
            buf_put(b, " < ");
            put_cast_value(b, type, &c->min);
        }
        if (c->has_min && c->has_max) {
            buf_put(b, " ||\n");
            buf_spaces(b, align);
        }
        if (c->has_max) {
            buf_put(b, x);
            buf_put(b, " > ");
            put_cast_value(b, type, &c->max);
        }
        break;
    }
}

/* Emit the check of one constraint on the body field r. */
static void emit_constraint(Gen *g, const Resolved *r, const Constraint *c)
{
    Buf x, cond;
    const char *type = SCALARS[r->scalar].is_signed && c->kind != CON_MASK
                           ? "INT64"
                           : "UINT64";

    memset(&x, 0, sizeof x);
    memset(&cond, 0, sizeof cond);
    put_read(g, &x, r, type);
    put_broken(&cond, x.data, type, c, g->indent * INDENT + 4u);
    g_if(g, &cond);
    free(x.data);
    free(cond.data);
}

/* One constraint the body's structures or the message's lines give, and
   where its field lies. */
typedef struct {
    Resolved r;
    Constraint c;
} ConstraintItem;

/* One array the body's structures hold, with its count's path. */
typedef struct {
    const Field *array;
    U64 offset;
    char prefix[PATH_CHARS + 1u];
} ArrayItem;

#define ITEMS_MAX 1024u

/* Whether structure s, or one it embeds, constrains a scalar. */
static int struct_constrains(const Struct *s)
{
    unsigned i;

    for (i = 0; i < s->field_count; i++) {
        const Field *f = &fields[s->first_field + i];

        if (f->is_array)
            continue;
        if (f->is_struct ? struct_constrains(&structs[f->struct_index])
                         : f->constraint.kind != CON_NONE)
            return 1;
    }
    return 0;
}

/* Walk structure s, at offset base inside the body and named by prefix,
   collecting the constraints of its scalars and its arrays, embedded
   structures included (P10, rule 1, step 4). */
static void collect_items(unsigned line, const Struct *s, U64 base,
                          const char *prefix, ConstraintItem *cons,
                          unsigned *ncons, ArrayItem *arrays,
                          unsigned *narrays)
{
    unsigned i;

    for (i = 0; i < s->field_count; i++) {
        const Field *f = &fields[s->first_field + i];
        char member[PATH_CHARS + 1u];

        memset(member, 0, sizeof member);
        if (prefix[0])
            memcpy(member, prefix, strlen(prefix) + 1u);
        member_append(line, member, f->name, (unsigned)strlen(f->name));
        if (f->is_array) {
            if (f->has_count ||
                (f->is_struct && struct_constrains(&structs[f->struct_index]))) {
                if (*narrays >= ITEMS_MAX)
                    TABLE_FULL(line, "arrays in a message");
                arrays[*narrays].array = f;
                arrays[*narrays].offset = base + f->offset;
                memcpy(arrays[*narrays].prefix, prefix, strlen(prefix) + 1u);
                (*narrays)++;
            }
        } else if (f->is_struct) {
            collect_items(line, &structs[f->struct_index], base + f->offset,
                          member, cons, ncons, arrays, narrays);
        } else if (f->constraint.kind != CON_NONE) {
            if (*ncons >= ITEMS_MAX)
                TABLE_FULL(line, "constraints in a message");
            memset(&cons[*ncons], 0, sizeof cons[*ncons]);
            cons[*ncons].r.tail = -1;
            cons[*ncons].r.offset = base + f->offset;
            cons[*ncons].r.scalar = f->scalar;
            memcpy(cons[*ncons].r.member, member, strlen(member) + 1u);
            cons[*ncons].c = f->constraint;
            (*ncons)++;
        }
    }
}

/* Emit the UTF-8 check of RFC 3629 over n bytes at p[start]: no overlong
   form, no surrogate, nothing above U+10FFFF, no NUL (P8, rule 6). The
   count is in n. */
static void emit_utf8(Gen *g, const char *start)
{
    static const char *const BODY[] = {
        "for (i = 0; i < n;) {",
        "    c = p[%s + i];",
        "    i++;",
        "    if (c == 0)",
        "        " FAIL_STATEMENT,
        "    if (c < 0x80)",
        "        continue;",
        "    if (c < 0xC2 || c > 0xF4)",
        "        " FAIL_STATEMENT,
        "    k = c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;",
        "    lo = 0x80;",
        "    hi = 0xBF;",
        "    if (c == 0xE0)",
        "        lo = 0xA0;",
        "    else if (c == 0xED)",
        "        hi = 0x9F;",
        "    else if (c == 0xF0)",
        "        lo = 0x90;",
        "    else if (c == 0xF4)",
        "        hi = 0x8F;",
        "    if (i + k > n || p[%s + i] < lo || p[%s + i] > hi)",
        "        " FAIL_STATEMENT,
        "    for (j = 1; j < k; j++)",
        "        if ((p[%s + i + j] & 0xC0) != 0x80)",
        "            " FAIL_STATEMENT,
        "    i += k;",
        "}"};
    unsigned l, q;

    g->used_p = g->used_i = g->used_j = g->used_n = g->used_utf8 = 1;
    for (l = 0; l < sizeof BODY / sizeof BODY[0]; l++) {
        const char *s = BODY[l];

        g_indent(g, 0);
        for (q = 0; s[q]; q++) {
            if (s[q] == '%' && s[q + 1u] == 's') {
                buf_put(&g->code, start);
                q++;
            } else {
                buf_ch(&g->code, s[q]);
            }
        }
        buf_ch(&g->code, '\n');
    }
}

/* Emit the constraints of each scalar of structure s, which starts at
   p[base]: every element of a tail is checked from the bytes. */
static void emit_element_constraints(Gen *g, const Struct *s,
                                     const char *base, U64 offset)
{
    unsigned i;

    for (i = 0; i < s->field_count; i++) {
        const Field *f = &fields[s->first_field + i];

        if (f->is_array)
            continue;
        if (f->is_struct) {
            emit_element_constraints(g, &structs[f->struct_index], base,
                                     offset + f->offset);
        } else if (f->constraint.kind != CON_NONE) {
            Buf x, cond;
            const char *type = SCALARS[f->scalar].is_signed &&
                                       f->constraint.kind != CON_MASK
                                   ? "INT64"
                                   : "UINT64";

            memset(&x, 0, sizeof x);
            memset(&cond, 0, sizeof cond);
            g->used_p = 1;
            if (SCALARS[f->scalar].is_signed && strcmp(type, "UINT64") == 0)
                buf_put(&x, "(UINT64)");
            put_load(&x, f->scalar, base, offset + f->offset);
            put_broken(&cond, x.data, type, &f->constraint,
                       g->indent * INDENT + 4u);
            g_if(g, &cond);
            free(x.data);
            free(cond.data);
        }
    }
}

/* Emit the encoder's refusal of a handle count outside the sum of the
   runs' minimums and maximums (P14, rule 5). */
static void emit_encode_handle_count(Gen *g)
{
    const Message *m = g->m;
    Buf cond;

    memset(&cond, 0, sizeof cond);
    if (m->handles_min) {
        buf_put(&cond, "(UINT64)handle_count < ");
        buf_u64(&cond, m->handles_min);
        buf_put(&cond, " ||\n");
        buf_spaces(&cond, g->indent * INDENT + 4u);
    }
    buf_put(&cond, "(UINT64)handle_count > ");
    buf_u64(&cond, m->handles_max);
    g_if(g, &cond);
    free(cond.data);
}

/* Emit the checks of a message's bytes in the order of P10, rule 1:
   its size, selector, transaction, body, status, tails and exact size;
   the handles are the caller's. */
static void emit_byte_checks(Gen *g)
{
    const Message *m = g->m;
    const Channel *ch = g->ch;
    ConstraintItem *cons = (ConstraintItem *)xalloc(sizeof(ConstraintItem) *
                                                    ITEMS_MAX);
    ArrayItem *arrays = (ArrayItem *)xalloc(sizeof(ArrayItem) * ITEMS_MAX);
    unsigned ncons = 0, narrays = 0, i, j, t;
    const char *total = g->encode ? "total" : "(UINT64)byte_count";
    Buf cond;

    memset(&cond, 0, sizeof cond);
    /* 1: the bytes hold the body. */
    if (!g->encode && m->body_size > 0) {
        buf_put(&cond, total);
        buf_put(&cond, " < ");
        buf_u64(&cond, m->body_size);
        g_if(g, &cond);
        cond.length = 0;
    }
    /* 2: the selector. */
    if (!g->encode && m->has_selector && m->has_cname) {
        Value v;

        memset(&v, 0, sizeof v);
        memcpy(v.text, m->cname, strlen(m->cname) + 1u);
        put_read(g, &cond, &m->selector, "UINT64");
        buf_put(&cond, " != ");
        put_cast_value(&cond, "UINT64", &v);
        g_if(g, &cond);
        cond.length = 0;
    }
    /* 3: the transaction (P7, rule 4). */
    if (m->has_transaction && m->kind == M_EVENT) {
        put_read(g, &cond, &m->transaction, "UINT64");
        buf_put(&cond, " != 0");
        g_if(g, &cond);
        cond.length = 0;
    }
    if (m->has_transaction && m->kind == M_REQUEST && m->has_reply &&
        ch->has_events) {
        put_read(g, &cond, &m->transaction, "UINT64");
        buf_put(&cond, " == 0");
        g_if(g, &cond);
        cond.length = 0;
    }
    /* 4: the body's constraints in offset order, then its arrays'. */
    if (m->body == BODY_STRUCT) {
        collect_items(m->line, &structs[m->body_struct], 0, "", cons, &ncons,
                      arrays, &narrays);
        for (i = 0; i < m->rule_count; i++)
            if (!m->rules[i].is_tails && m->rules[i].at.tail < 0) {
                if (ncons >= ITEMS_MAX)
                    TABLE_FULL(m->line, "constraints in a message");
                cons[ncons].r = m->rules[i].at;
                cons[ncons].c = m->rules[i].constraint;
                ncons++;
            }
    } else if (m->body == BODY_TRANSACTION) {
        for (i = 0; i < m->rule_count; i++)
            if (!m->rules[i].is_tails) {
                cons[ncons].r = m->rules[i].at;
                cons[ncons].c = m->rules[i].constraint;
                ncons++;
            }
    }
    for (i = 1; i < ncons; i++) {
        ConstraintItem moved = cons[i];

        for (j = i; j > 0 && cons[j - 1u].r.offset > moved.r.offset; j--)
            cons[j] = cons[j - 1u];
        cons[j] = moved;
    }
    for (i = 0; i < ncons; i++)
        emit_constraint(g, &cons[i].r, &cons[i].c);
    for (i = 0; i < narrays; i++) {
        const Field *a = arrays[i].array;
        Resolved count;

        memset(&count, 0, sizeof count);
        count.tail = -1;
        if (a->has_count) {
            const Field *cf = &fields[a->count_field];
            Value n;

            count.scalar = cf->scalar;
            memcpy(count.member, arrays[i].prefix,
                   strlen(arrays[i].prefix) + 1u);
            member_append(m->line, count.member, cf->name,
                          (unsigned)strlen(cf->name));
            n = a->array_size;
            /* The count is at most the array's length. */
            put_read(g, &cond, &count, "UINT64");
            buf_put(&cond, " > ");
            put_cast_value(&cond, "UINT64", &n);
            g_if(g, &cond);
            cond.length = 0;
            /* The elements past the count are 0. */
            g->used_p = g->used_i = 1;
            g_indent(g, 0);
            buf_put(&g->code, "for (i = ");
            put_read(g, &g->code, &count, "UINT64");
            if (a->element_size != 1u) {
                buf_put(&g->code, " * ");
                buf_u64(&g->code, a->element_size);
            }
            buf_put(&g->code, "; i < ");
            buf_u64(&g->code, a->array_size.number * a->element_size);
            buf_put(&g->code, "; i++)\n");
            g_indent(g, 1);
            buf_put(&g->code, "if (p[");
            buf_u64(&g->code, arrays[i].offset);
            buf_put(&g->code, " + i] != 0)\n");
            g_indent(g, 2);
            buf_put(&g->code, FAIL_STATEMENT "\n");
        }
        if (a->is_text) {
            Buf start;

            memset(&start, 0, sizeof start);
            buf_u64(&start, arrays[i].offset);
            g_indent(g, 0);
            buf_put(&g->code, "n = ");
            put_read(g, &g->code, &count, "UINT64");
            buf_put(&g->code, ";\n");
            emit_utf8(g, start.data);
            free(start.data);
        }
        /* The constraints of an element's structure hold for every
           element: the counted ones, or all of an array with no count
           (P6, rule 3). */
        if (a->is_struct && struct_constrains(&structs[a->struct_index])) {
            Buf base;

            memset(&base, 0, sizeof base);
            buf_u64(&base, arrays[i].offset);
            buf_put(&base, " + i * ");
            buf_u64(&base, a->element_size);
            g->used_p = g->used_i = 1;
            g_indent(g, 0);
            buf_put(&g->code, "for (i = 0; i < ");
            if (a->has_count)
                put_read(g, &g->code, &count, "UINT64");
            else
                buf_u64(&g->code, a->array_size.number);
            buf_put(&g->code, "; i++) {\n");
            g->indent++;
            emit_element_constraints(g, &structs[a->struct_index], base.data,
                                     0);
            g->indent--;
            g_line(g, "}");
            free(base.data);
        }
    }
    /* 5: a reply with a status other than 0 is its body alone. */
    if (m->has_status) {
        Buf status;

        memset(&status, 0, sizeof status);
        put_read(g, &status, &m->status, "UINT64");
        if (g->encode) {
            buf_put(&cond, status.data);
            buf_put(&cond, " != OK && ");
            if (m->handle_count)
                buf_put(&cond, "(");
            buf_put(&cond, "total != ");
            buf_u64(&cond, m->body_size);
            if (m->handle_count)
                buf_put(&cond, " || (UINT64)handle_count != 0)");
            g_if(g, &cond);
            cond.length = 0;
            g_indent(g, 0);
            buf_put(&g->code, "if (");
            buf_put(&g->code, status.data);
            buf_put(&g->code, " == OK) {\n");
            g->indent++;
            if (m->handle_count)
                emit_encode_handle_count(g);
        } else {
            g_indent(g, 0);
            buf_put(&g->code, "if (");
            buf_put(&g->code, status.data);
            buf_put(&g->code, " != OK)\n");
            g_indent(g, 1);
            buf_put(&g->code, "return (");
            buf_put(&g->code, total);
            buf_put(&g->code, " == ");
            buf_u64(&g->code, m->body_size);
            buf_put(&g->code, " && (UINT64)handle_count == 0) ? OK : "
                              "ERR_INVALID_ARGS;\n");
        }
        free(status.data);
    }
    /* 6: the tails, then each `= tails` field, then the exact size. */
    if (m->tail_count) {
        g->used_at = 1;
        g_indent(g, 0);
        buf_put(&g->code, "at = ");
        buf_u64(&g->code, m->body_size);
        buf_put(&g->code, ";\n");
    }
    for (t = 0; t < m->tail_count; t++) {
        const Tail *tail = &m->tails[t];

        if (tail->shape == TAIL_STRUCT) {
            buf_put(&cond, "at + ");
            buf_u64(&cond, tail->element_size);
            buf_put(&cond, " > ");
            buf_put(&cond, total);
            g_if(g, &cond);
            cond.length = 0;
            if (struct_constrains(&structs[tail->struct_index]))
                emit_element_constraints(g, &structs[tail->struct_index],
                                         "at", 0);
            for (i = 0; i < m->rule_count; i++)
                if (!m->rules[i].is_tails && m->rules[i].at.tail == (int)t)
                    emit_constraint(g, &m->rules[i].at,
                                    &m->rules[i].constraint);
            g_indent(g, 0);
            buf_put(&g->code, "at += ");
            buf_u64(&g->code, tail->element_size);
            buf_put(&g->code, ";\n");
            continue;
        }
        g->used_n = 1;
        g_indent(g, 0);
        buf_put(&g->code, "n = ");
        if (tail->shape == TAIL_REST) {
            buf_put(&g->code, total);
            buf_put(&g->code, " - at");
        } else {
            put_read(g, &g->code, &tail->count, "UINT64");
        }
        buf_put(&g->code, ";\n");
        buf_put(&cond, "n > ");
        put_cast_value(&cond, "UINT64", &tail->max);
        g_if(g, &cond);
        cond.length = 0;
        if (tail->shape == TAIL_COUNTED) {
            buf_put(&cond, "at + n");
            if (tail->element_size != 1u) {
                buf_put(&cond, " * ");
                buf_u64(&cond, tail->element_size);
            }
            buf_put(&cond, " > ");
            buf_put(&cond, total);
            g_if(g, &cond);
            cond.length = 0;
        }
        if (tail->element == ELEMENT_TEXT)
            emit_utf8(g, "at");
        if (tail->element == ELEMENT_STRUCT &&
            struct_constrains(&structs[tail->struct_index])) {
            Buf base;

            memset(&base, 0, sizeof base);
            buf_put(&base, "at + i * ");
            buf_u64(&base, tail->element_size);
            g->used_i = 1;
            g_indent(g, 0);
            buf_put(&g->code, "for (i = 0; i < n; i++) {\n");
            g->indent++;
            emit_element_constraints(g, &structs[tail->struct_index],
                                     base.data, 0);
            g->indent--;
            g_line(g, "}");
            free(base.data);
        }
        g_indent(g, 0);
        buf_put(&g->code, "at += n");
        if (tail->element_size != 1u && tail->shape == TAIL_COUNTED) {
            buf_put(&g->code, " * ");
            buf_u64(&g->code, tail->element_size);
        }
        buf_put(&g->code, ";\n");
    }
    for (i = 0; i < m->rule_count; i++) {
        if (!m->rules[i].is_tails)
            continue;
        put_read(g, &cond, &m->rules[i].at, "UINT64");
        buf_put(&cond, " != ");
        buf_put(&cond, total);
        buf_put(&cond, " - ");
        buf_u64(&cond, m->body_size);
        g_if(g, &cond);
        cond.length = 0;
    }
    if (m->tail_count) {
        buf_put(&cond, "at != ");
        buf_put(&cond, total);
    } else {
        buf_put(&cond, total);
        buf_put(&cond, " != ");
        buf_u64(&cond, m->body_size);
    }
    g_if(g, &cond);
    free(cond.data);
    free(cons);
    free(arrays);
}

/* The expression of a rights specification for a handle (P9, rule 3 and
   4). */
static void put_rights_spec(Buf *b, const RightsSpec *s)
{
    switch (s->kind) {
    case RS_SAME:
        buf_put(b, "RIGHTS_SAME");
        break;
    case RS_NONE:
        buf_put(b, "0");
        break;
    case RS_CONST:
        buf_put(b, s->const_name);
        break;
    case RS_LIST:
        put_rights(b, &s->list);
        break;
    }
}

/* Whether a handle line's slots are walked by a loop: a run that varies, or
   a fixed one of more than one handle. Only a run of exactly one handle is
   the single slot at its position (P9, rule 1). */
static int handle_walks(const HandleLine *h)
{
    return h->min_count != h->max_count || h->max_count > 1u;
}

/* Open the loop over a run's slots, from its position to the handle count
   where it varies and to the end of its fixed number where it does not. */
static void open_handle_loop(Gen *g, const HandleLine *h)
{
    g->used_i = 1;
    g_indent(g, 0);
    buf_put(&g->code, "for (i = ");
    buf_u64(&g->code, h->position);
    buf_put(&g->code, "; i < ");
    if (h->min_count != h->max_count)
        buf_put(&g->code, "handle_count");
    else
        buf_u64(&g->code, h->position + h->max_count);
    buf_put(&g->code, "; i++) {\n");
    g->indent++;
}

/* Emit the handle count and each handle's type and rights (P10, rule 1,
   step 7). */
static void emit_handle_checks(Gen *g)
{
    const Message *m = g->m;
    Buf cond;
    unsigned t;

    memset(&cond, 0, sizeof cond);
    if (m->handles_min == m->handles_max) {
        buf_put(&cond, "(UINT64)handle_count != ");
        buf_u64(&cond, m->handles_max);
        g_if(g, &cond);
    } else {
        if (m->handles_min) {
            buf_put(&cond, "(UINT64)handle_count < ");
            buf_u64(&cond, m->handles_min);
            buf_put(&cond, " ||\n");
            buf_spaces(&cond, g->indent * INDENT + 4u);
        }
        buf_put(&cond, "(UINT64)handle_count > ");
        buf_u64(&cond, m->handles_max);
        g_if(g, &cond);
    }
    for (t = 0; t < m->handle_count; t++) {
        const HandleLine *h = &m->handles[t];
        Buf needs;
        int walks = handle_walks(h);

        g->used_handles = 1;
        memset(&needs, 0, sizeof needs);
        put_rights_spec(&needs, &h->needs);
        if (walks)
            open_handle_loop(g, h);
        cond.length = 0;
        buf_put(&cond, "(UINT64)handles[");
        if (walks)
            buf_ch(&cond, 'i');
        else
            buf_u64(&cond, h->position);
        buf_put(&cond, "].type != (UINT64)");
        buf_put(&cond, TYPES[h->type].macro);
        if (h->needs.kind != RS_NONE && h->needs.list.count) {
            buf_put(&cond, " ||\n");
            buf_spaces(&cond, g->indent * INDENT + 4u);
            buf_put(&cond, "((UINT64)handles[");
            if (walks)
                buf_ch(&cond, 'i');
            else
                buf_u64(&cond, h->position);
            buf_put(&cond, "].rights & (UINT64)(");
            buf_put(&cond, needs.data);
            buf_put(&cond, ")) != (UINT64)(");
            buf_put(&cond, needs.data);
            buf_put(&cond, ")");
        }
        g_if(g, &cond);
        if (walks) {
            g->indent--;
            g_line(g, "}");
        }
        free(needs.data);
    }
    free(cond.data);
}

/* The declarations a function opens with, then its statements, from what
   they used. */
static void put_locals(Buf *b, const Gen *g, int check)
{
    int any = 0;
    const Message *m = g->m;

    if (g->used_view && m->body != BODY_NONE) {
        buf_spaces(b, INDENT);
        if (check) {
            buf_put(b, "const ");
            buf_put(b, m->body == BODY_STRUCT ? structs[m->body_struct].name
                                              : "UINT32");
            buf_put(b, " *" CHECK_VIEW " = (const ");
            buf_put(b, m->body == BODY_STRUCT ? structs[m->body_struct].name
                                              : "UINT32");
            buf_put(b, " *)bytes;\n");
        } else {
            buf_put(b, m->body == BODY_STRUCT ? structs[m->body_struct].name
                                              : "UINT32");
            buf_put(b, " *" ENCODE_VIEW " = (");
            buf_put(b, m->body == BODY_STRUCT ? structs[m->body_struct].name
                                              : "UINT32");
            buf_put(b, " *)bytes;\n");
        }
        any = 1;
    }
    if (g->used_p) {
        buf_spaces(b, INDENT);
        buf_put(b, check ? "const UINT8 *p = (const UINT8 *)bytes;\n"
                         : "UINT8 *p = (UINT8 *)bytes;\n");
        any = 1;
    }
    if (g->used_at || g->used_i || g->used_j || g->used_n) {
        const char *names[4];
        unsigned count = 0, i;

        if (g->used_at)
            names[count++] = "at";
        if (g->used_i)
            names[count++] = "i";
        if (g->used_j)
            names[count++] = "j";
        if (g->used_n)
            names[count++] = "n";
        buf_spaces(b, INDENT);
        buf_put(b, "UINT64 ");
        for (i = 0; i < count; i++) {
            if (i)
                buf_put(b, ", ");
            buf_put(b, names[i]);
        }
        buf_put(b, ";\n");
        any = 1;
    }
    if (g->used_utf8) {
        buf_spaces(b, INDENT);
        buf_put(b, "UINT32 c, k, lo, hi;\n");
        any = 1;
    }
    if (any)
        buf_ch(b, '\n');
}

/* The parameters of a function, one a line aligned with the first. */
static void put_params(Buf *b, const char *name, const char *const *params,
                       unsigned count)
{
    unsigned i;
    size_t align;

    buf_put(b, "static inline UINT32 ");
    buf_put(b, name);
    buf_ch(b, '(');
    align = buf_column(b);
    for (i = 0; i < count; i++) {
        if (i) {
            buf_put(b, ",\n");
            buf_spaces(b, (unsigned)align);
        }
        buf_put(b, params[i]);
    }
    buf_put(b, ")\n");
}

#define FUNCTION_PARAMS_MAX 24u

/* The check of a message (P10, rule 1, and P14, rule 4). */
static void emit_check(Buf *b, const Channel *ch, const Message *m)
{
    Gen g;
    Buf name;
    const char *params[4] = {"const void *bytes", "UINT32 byte_count",
                             "const RECEIVED_HANDLE *handles",
                             "UINT32 handle_count"};

    memset(&g, 0, sizeof g);
    memset(&name, 0, sizeof name);
    g.m = m;
    g.ch = ch;
    g.view = CHECK_VIEW;
    g.indent = 1;
    emit_byte_checks(&g);
    emit_handle_checks(&g);
    put_message_name(&name, ch, m, 0, "_check");
    put_doc(b, 0,
            "Check a message received on the channel: returns OK, or "
            "ERR_INVALID_ARGS at the first rule of its description it "
            "breaks (docs/spec/interface.md P10).",
            &m->desc);
    put_params(b, name.data, params, 4u);
    buf_put(b, "{\n");
    put_locals(b, &g, 1);
    if (!g.used_view && !g.used_p)
        buf_put(b, "    (void)bytes;\n");
    if (!g.used_handles)
        buf_put(b, "    (void)handles;\n");
    if ((!g.used_view && !g.used_p) || !g.used_handles)
        buf_ch(b, '\n');
    buf_put(b, g.code.data ? g.code.data : "");
    buf_put(b, "    return OK;\n}\n\n");
    free(name.data);
    free(g.code.data);
}

/* Emit the filling of `transfers`: each handle of a run with the rights its
   line travels with (P14, rule 5). */
static void emit_transfers(Gen *g)
{
    const Message *m = g->m;
    unsigned t;

    for (t = 0; t < m->handle_count; t++) {
        const HandleLine *h = &m->handles[t];
        Buf rights;

        memset(&rights, 0, sizeof rights);
        put_rights_spec(&rights, &h->travels);
        if (handle_walks(h)) {
            open_handle_loop(g, h);
            g_line(g, "transfers[i].handle = handles[i];");
            g_indent(g, 0);
            buf_put(&g->code, "transfers[i].rights = ");
            buf_put(&g->code, rights.data);
            buf_put(&g->code, ";\n");
            g->indent--;
            g_line(g, "}");
        } else {
            g_indent(g, 0);
            buf_put(&g->code, "transfers[");
            buf_u64(&g->code, h->position);
            buf_put(&g->code, "].handle = handles[");
            buf_u64(&g->code, h->position);
            buf_put(&g->code, "];\n");
            g_indent(g, 0);
            buf_put(&g->code, "transfers[");
            buf_u64(&g->code, h->position);
            buf_put(&g->code, "].rights = ");
            buf_put(&g->code, rights.data);
            buf_put(&g->code, ";\n");
        }
        free(rights.data);
    }
}

/* The encoding of a message (P14, rule 5). */
static void emit_encode(Buf *b, const Channel *ch, const Message *m)
{
    Gen g;
    Buf name, text, cond;
    const char *params[FUNCTION_PARAMS_MAX];
    char *owned[FUNCTION_PARAMS_MAX];
    unsigned count = 0, owned_count = 0, t, i;
    int has_handles = m->handle_count > 0;

    memset(&g, 0, sizeof g);
    memset(&name, 0, sizeof name);
    memset(&cond, 0, sizeof cond);
    g.m = m;
    g.ch = ch;
    g.view = ENCODE_VIEW;
    g.encode = 1;
    g.indent = 1;

    /* The parameters. */
    if (m->body == BODY_STRUCT) {
        memset(&text, 0, sizeof text);
        buf_put(&text, "const ");
        buf_put(&text, structs[m->body_struct].name);
        buf_put(&text, " *body");
        params[count++] = owned[owned_count++] = text.data;
    } else if (m->body == BODY_TRANSACTION) {
        params[count++] = "UINT32 transaction";
    }
    for (t = 0; t < m->tail_count; t++) {
        const Tail *tail = &m->tails[t];

        memset(&text, 0, sizeof text);
        if (tail->element == ELEMENT_STRUCT) {
            buf_put(&text, "const ");
            buf_put(&text, structs[tail->struct_index].name);
            buf_put(&text, " *");
        } else {
            buf_put(&text, "const UINT8 *");
        }
        buf_put(&text, tail->name);
        params[count++] = owned[owned_count++] = text.data;
        if (tail->shape != TAIL_STRUCT) {
            memset(&text, 0, sizeof text);
            buf_put(&text, "UINT32 ");
            buf_put(&text, tail->name);
            buf_put(&text, "_count");
            params[count++] = owned[owned_count++] = text.data;
        }
    }
    if (has_handles) {
        params[count++] = "const UINT32 *handles";
        params[count++] = "UINT32 handle_count";
        params[count++] = "HANDLE_TRANSFER *transfers";
    }
    params[count++] = "void *bytes";
    params[count++] = "UINT32 *byte_count";

    /* Refuse what bounds the message before writing it. */
    g_line(&g, "*byte_count = 0;");
    for (t = 0; t < m->tail_count; t++) {
        const Tail *tail = &m->tails[t];

        if (tail->shape == TAIL_STRUCT)
            continue;
        buf_put(&cond, "(UINT64)");
        buf_put(&cond, tail->name);
        buf_put(&cond, "_count > ");
        put_cast_value(&cond, "UINT64", &tail->max);
        g_if(&g, &cond);
        cond.length = 0;
    }
    /* A reply with a status takes its handles only with status 0, which the
       check of what was written decides. */
    if (has_handles && !m->has_status)
        emit_encode_handle_count(&g);
    /* Write the body, then the selector, the tails and the counts. */
    if (m->body == BODY_STRUCT) {
        g.used_p = 1;
        g.used_i = 1;
        g_indent(&g, 0);
        buf_put(&g.code, "for (i = 0; i < ");
        buf_u64(&g.code, m->body_size);
        buf_put(&g.code, "; i++)\n");
        g_indent(&g, 1);
        buf_put(&g.code, "p[i] = ((const UINT8 *)body)[i];\n");
    } else if (m->body == BODY_TRANSACTION) {
        g.used_view = 1;
        g_line(&g, ENCODE_VIEW "[0] = transaction;");
    }
    if (m->has_selector && m->has_cname) {
        g.used_view = 1;
        g_indent(&g, 0);
        buf_put(&g.code, ENCODE_VIEW "->");
        buf_put(&g.code, m->selector.member);
        buf_put(&g.code, " = (");
        buf_put(&g.code, SCALARS[m->selector.scalar].ctype);
        buf_put(&g.code, ")");
        buf_put(&g.code, m->cname);
        buf_put(&g.code, ";\n");
    }
    if (m->tail_count) {
        g.used_at = 1;
        g_indent(&g, 0);
        buf_put(&g.code, "at = ");
        buf_u64(&g.code, m->body_size);
        buf_put(&g.code, ";\n");
    }
    for (t = 0; t < m->tail_count; t++) {
        const Tail *tail = &m->tails[t];

        g.used_i = 1;
        g.used_p = 1;
        g_indent(&g, 0);
        buf_put(&g.code, "for (i = 0; i < ");
        if (tail->shape == TAIL_STRUCT) {
            buf_u64(&g.code, tail->element_size);
        } else {
            buf_put(&g.code, "(UINT64)");
            buf_put(&g.code, tail->name);
            buf_put(&g.code, "_count");
            if (tail->element_size != 1u) {
                buf_put(&g.code, " * ");
                buf_u64(&g.code, tail->element_size);
            }
        }
        buf_put(&g.code, "; i++)\n");
        g_indent(&g, 1);
        buf_put(&g.code, "p[at + i] = ");
        if (tail->element == ELEMENT_STRUCT)
            buf_put(&g.code, "((const UINT8 *)");
        buf_put(&g.code, tail->name);
        if (tail->element == ELEMENT_STRUCT)
            buf_put(&g.code, ")");
        buf_put(&g.code, "[i];\n");
        g_indent(&g, 0);
        buf_put(&g.code, "at += ");
        if (tail->shape == TAIL_STRUCT) {
            buf_u64(&g.code, tail->element_size);
        } else {
            buf_put(&g.code, "(UINT64)");
            buf_put(&g.code, tail->name);
            buf_put(&g.code, "_count");
            if (tail->element_size != 1u) {
                buf_put(&g.code, " * ");
                buf_u64(&g.code, tail->element_size);
            }
        }
        buf_put(&g.code, ";\n");
    }
    for (t = 0; t < m->tail_count; t++) {
        const Tail *tail = &m->tails[t];

        if (tail->shape != TAIL_COUNTED)
            continue;
        if (tail->count.tail < 0) {
            g.used_view = 1;
            g_indent(&g, 0);
            buf_put(&g.code, ENCODE_VIEW "->");
            buf_put(&g.code, tail->count.member);
            buf_put(&g.code, " = (");
            buf_put(&g.code, SCALARS[tail->count.scalar].ctype);
            buf_put(&g.code, ")");
            buf_put(&g.code, tail->name);
            buf_put(&g.code, "_count;\n");
        } else {
            Buf start;
            unsigned size = SCALARS[tail->count.scalar].size;

            memset(&start, 0, sizeof start);
            put_tail_start(&g, &start, m, (unsigned)tail->count.tail);
            for (i = 0; i < size; i++) {
                g_indent(&g, 0);
                buf_put(&g.code, "p[");
                buf_put(&g.code, start.data);
                buf_put(&g.code, " + ");
                buf_u64(&g.code, tail->count.offset + i);
                buf_put(&g.code, "] = (UINT8)((UINT64)");
                buf_put(&g.code, tail->name);
                buf_put(&g.code, "_count >> ");
                buf_u64(&g.code, 8u * i);
                buf_put(&g.code, ");\n");
            }
            free(start.data);
        }
    }
    for (i = 0; i < m->rule_count; i++) {
        if (!m->rules[i].is_tails)
            continue;
        g.used_view = 1;
        g_indent(&g, 0);
        buf_put(&g.code, ENCODE_VIEW "->");
        buf_put(&g.code, m->rules[i].at.member);
        buf_put(&g.code, " = (");
        buf_put(&g.code, SCALARS[m->rules[i].at.scalar].ctype);
        buf_put(&g.code, ")(at - ");
        buf_u64(&g.code, m->body_size);
        buf_put(&g.code, ");\n");
    }
    /* Check what was written as the receiver's check does. */
    g_indent(&g, 0);
    buf_put(&g.code, "{\n");
    {
        Buf total;

        memset(&total, 0, sizeof total);
        g.indent++;
        /* `total` is the size written. */
        g_indent(&g, 0);
        buf_put(&g.code, "const UINT64 total = ");
        if (m->tail_count)
            buf_put(&g.code, "at");
        else
            buf_u64(&g.code, m->body_size);
        buf_put(&g.code, ";\n\n");
        free(total.data);
    }
    emit_byte_checks(&g);
    if (m->has_status) {
        emit_transfers(&g);
        g.indent--;
        g_line(&g, "}");
    }
    g.indent--;
    g_line(&g, "}");
    if (!m->has_status)
        emit_transfers(&g);
    g_indent(&g, 0);
    buf_put(&g.code, "*byte_count = (UINT32)");
    if (m->tail_count)
        buf_put(&g.code, "at");
    else
        buf_u64(&g.code, m->body_size);
    buf_put(&g.code, ";\n");

    put_message_name(&name, ch, m, 0, "_encode");
    put_doc(b, 0,
            "Lay the message out in bytes, which is aligned to 8 bytes and "
            "holds at least the direction's BYTES_MAX: returns OK with the "
            "message's size in *byte_count, or ERR_INVALID_ARGS with 0 "
            "there if it breaks the description (docs/spec/interface.md "
            "P14).",
            &m->desc);
    put_params(b, name.data, params, count);
    buf_put(b, "{\n");
    put_locals(b, &g, 0);
    if (!g.used_view && !g.used_p) {
        buf_put(b, "    (void)bytes;\n\n");
    }
    buf_put(b, g.code.data);
    buf_put(b, "    return OK;\n}\n\n");
    for (i = 0; i < owned_count; i++)
        free(owned[i]);
    free(name.data);
    free(cond.data);
    free(g.code.data);
}

/* ------------------------------------------------------------------------
   The header (P14): channels, and the whole file. */

/* A tail's accessor: where it starts in a message whose check passed
   (P14, rule 4). */
static void emit_tail_accessor(Buf *b, const Channel *ch, const Message *m,
                               unsigned t)
{
    const Tail *tail = &m->tails[t];
    Gen g;
    Buf name, start, params;
    const char *const params_list[1] = {"const void *bytes"};

    memset(&g, 0, sizeof g);
    memset(&name, 0, sizeof name);
    memset(&start, 0, sizeof start);
    memset(&params, 0, sizeof params);
    g.m = m;
    g.ch = ch;
    g.view = CHECK_VIEW;
    g.indent = 1;
    put_tail_start(&g, &start, m, t);
    put_message_name(&name, ch, m, 0, "_");
    buf_put(&name, tail->name);
    put_doc(b, 0,
            "Where this tail starts in a message whose check passed "
            "(docs/spec/interface.md P14).",
            &tail->desc);
    buf_put(b, "static inline const ");
    buf_put(b, tail->element == ELEMENT_STRUCT ? structs[tail->struct_index].name
                                               : "UINT8");
    buf_put(b, " *");
    buf_put(b, name.data);
    buf_put(b, "(");
    buf_put(b, params_list[0]);
    buf_put(b, ")\n{\n");
    if (g.used_view && m->body != BODY_NONE) {
        buf_put(b, "    const ");
        buf_put(b, m->body == BODY_STRUCT ? structs[m->body_struct].name
                                          : "UINT32");
        buf_put(b, " *" CHECK_VIEW " = (const ");
        buf_put(b, m->body == BODY_STRUCT ? structs[m->body_struct].name
                                          : "UINT32");
        buf_put(b, " *)bytes;\n");
    }
    if (g.used_p)
        buf_put(b, "    const UINT8 *p = (const UINT8 *)bytes;\n");
    if (g.used_view || g.used_p)
        buf_ch(b, '\n');
    if (tail->element == ELEMENT_STRUCT) {
        buf_put(b, "    return (const ");
        buf_put(b, structs[tail->struct_index].name);
        buf_put(b, " *)((const UINT8 *)bytes + ");
    } else {
        buf_put(b, "    return (const UINT8 *)bytes + ");
    }
    buf_put(b, start.data);
    buf_put(b, tail->element == ELEMENT_STRUCT ? ");\n}\n\n" : ";\n}\n\n");
    free(name.data);
    free(start.data);
    free(params.data);
    free(g.code.data);
}

/* A dispatcher over the requests or the events of a channel with a
   selector (P10, rule 2, and P14, rule 4). */
static void emit_dispatcher(Buf *b, const Channel *ch, int requests)
{
    Buf name, unnamed;
    const char *params[5] = {"const void *bytes", "UINT32 byte_count",
                             "const RECEIVED_HANDLE *handles",
                             "UINT32 handle_count", "UINT32 *which"};
    unsigned k;
    unsigned size = SCALARS[ch->select_scalar].size, i;

    memset(&name, 0, sizeof name);
    memset(&unnamed, 0, sizeof unnamed);
    put_channel_name(&name, ch, 0);
    buf_put(&name, requests ? "_request_check" : "_event_check");
    put_channel_name(&unnamed, ch, 1);
    buf_put(&unnamed, "_UNNAMED");
    put_doc(b, 0, requests
                      ? "Check a request received on the channel as its "
                        "description gives: writes the selector's value, or "
                        "the channel's UNNAMED where the bytes are too "
                        "short to hold it, to *which, and returns "
                        "ERR_INVALID_ARGS for a message that breaks its "
                        "description, ERR_NOT_SUPPORTED for a value no "
                        "request has (docs/spec/interface.md P10)."
                      : "Check an event received on the channel as its "
                        "description gives: writes the selector's value, or "
                        "the channel's UNNAMED where the bytes are too "
                        "short to hold it, to *which, and returns "
                        "ERR_INVALID_ARGS for a message that breaks its "
                        "description, ERR_NOT_SUPPORTED for a value no "
                        "event has (docs/spec/interface.md P10).",
            &NO_DESC);
    put_params(b, name.data, params, 5u);
    buf_put(b, "{\n    UINT64 selector;\n    const UINT8 *p = (const UINT8 *)bytes;\n\n");
    buf_put(b, "    if ((UINT64)byte_count < ");
    buf_u64(b, ch->select_offset + size);
    buf_put(b, ") {\n        *which = ");
    buf_put(b, unnamed.data);
    buf_put(b, ";\n        return ERR_INVALID_ARGS;\n    }\n    selector = (");
    for (i = 0; i < size; i++) {
        if (i)
            buf_put(b, " | ");
        buf_put(b, "(UINT64)p[");
        buf_u64(b, ch->select_offset + i);
        buf_ch(b, ']');
        if (i) {
            buf_put(b, " << ");
            buf_u64(b, 8u * i);
        }
    }
    buf_put(b, ");\n    *which = (UINT32)selector;\n");
    for (k = 0; k < ch->message_count; k++) {
        const Message *m = messages[ch->first_message + k];
        Buf call;

        if ((requests && m->kind != M_REQUEST) ||
            (!requests && m->kind != M_EVENT) || m->first || m->retired ||
            !m->has_selector)
            continue;
        memset(&call, 0, sizeof call);
        put_message_name(&call, ch, m, 0, "_check");
        buf_put(b, "    if (selector == (UINT64)");
        buf_put(b, m->cname);
        buf_put(b, ")\n        return ");
        buf_put(b, call.data);
        buf_put(b, "(bytes, byte_count, handles, handle_count);\n");
        free(call.data);
    }
    buf_put(b, "    return ERR_NOT_SUPPORTED;\n}\n\n");
    free(name.data);
    free(unnamed.data);
}

/* Append text, carried over lines of at most WRAP_COLUMN columns, each
   after indent. */
static void put_wrapped(Buf *b, const char *indent, const char *text)
{
    const char *p = text;
    size_t line_start = b->length;

    buf_put(b, indent);
    while (*p) {
        const char *end = p;

        while (*end && *end != ' ')
            end++;
        if (b->length - line_start > strlen(indent) &&
            buf_column(b) + (size_t)(end - p) + 1u > WRAP_COLUMN) {
            buf_ch(b, '\n');
            line_start = b->length;
            buf_put(b, indent);
        } else if (b->length - line_start > strlen(indent)) {
            buf_ch(b, ' ');
        }
        buf_putn(b, p, (size_t)(end - p));
        p = *end ? end + 1 : end;
    }
    buf_ch(b, '\n');
}

/* The sentence for a channel end's policy (P10, rule 3). */
static void put_policy(Buf *b, const char *end, const char *what,
                       const Policy *p)
{
    Buf text;

    memset(&text, 0, sizeof text);
    switch (p->kind) {
    case POLICY_NONE:
        free(text.data);
        return;
    case POLICY_CLOSE:
        buf_put(&text, "A ");
        buf_put(&text, what);
        buf_put(&text, " that fails its check ends the connection: the ");
        buf_put(&text, end);
        buf_put(&text, " closes the handles it carried and its own end, and "
                       "answers nothing.");
        break;
    case POLICY_DROP:
        buf_put(&text, "A ");
        buf_put(&text, what);
        buf_put(&text, " that fails its check is dropped: the ");
        buf_put(&text, end);
        buf_put(&text, " closes the handles it carried and goes on");
        if (strcmp(end, "server") == 0)
            buf_put(&text, ", and answers a request the dispatcher named "
                           "that has a reply with that reply's body alone: "
                           "the request's transaction, the check's status "
                           "in its status field if it has one, and every "
                           "other byte 0.");
        else
            buf_put(&text, "; a call the message answers fails.");
        break;
    case POLICY_EXIT:
        buf_put(&text, "A ");
        buf_put(&text, what);
        buf_put(&text, " that fails its check ends the ");
        buf_put(&text, end);
        buf_put(&text, "'s process.");
        break;
    case POLICY_ANSWER:
        buf_put(&text, "A ");
        buf_put(&text, what);
        buf_put(&text, " that fails its check is answered with the reply ");
        buf_put(&text, p->reply);
        buf_put(&text, "'s body alone, once the handles it carried are "
                       "closed: its first four bytes are the transaction, "
                       "and its status is ");
        if (p->has_status)
            buf_put(&text, p->status);
        else
            buf_put(&text, "the check's status");
        buf_put(&text, "; a message under four bytes ends the connection.");
        break;
    }
    put_wrapped(b, "    ", text.data);
    free(text.data);
}

/* A channel: its group comment, its messages' macros, its limits and
   its functions (P14, rule 1 and rule 4). */
static void emit_channel(Buf *b, const Channel *ch)
{
    unsigned k, t;
    Buf name;

    memset(&name, 0, sizeof name);
    /* The group, with the channel's description and its policies. */
    buf_put(b, "/** @name ");
    buf_put(b, ch->name);
    buf_ch(b, '\n');
    for (k = 0; k < ch->desc.count; k++) {
        buf_spaces(b, INDENT);
        buf_putn(b, lines[ch->desc.first + k].comment,
                 lines[ch->desc.first + k].comment_length);
        buf_ch(b, '\n');
    }
    if (ch->desc.trail_length) {
        buf_spaces(b, INDENT);
        buf_putn(b, ch->desc.trail, ch->desc.trail_length);
        buf_ch(b, '\n');
    }
    put_policy(b, "server", "request", &ch->server);
    put_policy(b, "client", "reply or event", &ch->client);
    buf_put(b, "    @{ */\n\n");

    /* Each message's macros. */
    for (k = 0; k < ch->message_count; k++) {
        const Message *m = messages[ch->first_message + k];

        if (m->has_cname)
            put_described_define(b, &m->desc, m->cname, m->value_text, 0);
        for (t = 0; t < m->handle_count; t++)
            if (m->handles[t].has_index) {
                Buf text;

                memset(&text, 0, sizeof text);
                buf_u64(&text, m->handles[t].position);
                put_described_define(b, &m->handles[t].desc,
                                     m->handles[t].index_name, text.data, 0);
                free(text.data);
            }
        if (m->has_handles_macro) {
            Buf text;

            memset(&text, 0, sizeof text);
            buf_u64(&text, m->handles_max);
            put_described_define(b, &m->handles_desc, m->handles_name,
                                 text.data, 0);
            free(text.data);
        }
    }
    if (ch->has_select) {
        name.length = 0;
        put_channel_name(&name, ch, 1);
        buf_put(&name, "_UNNAMED");
        put_doc(b, 0,
                "A dispatcher's `which` for a message too short to hold the "
                "selector.",
                &NO_DESC);
        put_define(b, name.data, "0xFFFFFFFF", 0);
        buf_ch(b, '\n');
    }
    if (ch->has_requests || ch->has_replies || ch->has_events)
        buf_ch(b, '\n');

    /* The limits. */
    for (k = 0; k < 2u; k++) {
        int to_server = k == 0;
        U64 bytes = to_server ? ch->server_bytes_max : ch->client_bytes_max;
        U64 handles_max =
            to_server ? ch->server_handles_max : ch->client_handles_max;
        Buf bytes_name, handles_name;

        if (to_server ? !ch->has_requests : !(ch->has_replies ||
                                              ch->has_events))
            continue;
        memset(&bytes_name, 0, sizeof bytes_name);
        memset(&handles_name, 0, sizeof handles_name);
        put_channel_name(&bytes_name, ch, 1);
        buf_put(&bytes_name,
                to_server ? "_TO_SERVER_BYTES_MAX" : "_TO_CLIENT_BYTES_MAX");
        put_channel_name(&handles_name, ch, 1);
        buf_put(&handles_name, to_server ? "_TO_SERVER_HANDLES_MAX"
                                         : "_TO_CLIENT_HANDLES_MAX");
        put_decimal_define(b, &NO_DESC,
                           to_server
                               ? "The most bytes a request on the channel "
                                 "holds."
                               : "The most bytes a reply or event on the "
                                 "channel holds.",
                           bytes_name.data, bytes);
        put_decimal_define(b, &NO_DESC,
                           to_server
                               ? "The most handles a request on the channel "
                                 "carries."
                               : "The most handles a reply or event on the "
                                 "channel carries.",
                           handles_name.data, handles_max);
        buf_put(b, "_Static_assert(");
        buf_put(b, bytes_name.data);
        buf_put(b, " <= CHANNEL_MAX_BYTES, \"a message fits a channel\");\n");
        buf_put(b, "_Static_assert(");
        buf_put(b, handles_name.data);
        buf_put(b, " <= CHANNEL_MAX_HANDLES, \"a message's handles fit a "
                   "channel\");\n\n");
        free(bytes_name.data);
        free(handles_name.data);
    }

    /* The functions. */
    for (k = 0; k < ch->message_count; k++) {
        const Message *m = messages[ch->first_message + k];

        if (m->retired)
            continue;
        emit_check(b, ch, m);
        emit_encode(b, ch, m);
        for (t = 0; t < m->tail_count; t++)
            emit_tail_accessor(b, ch, m, t);
    }
    if (ch->has_select) {
        int request_dispatch = 0, event_dispatch = 0;

        for (k = 0; k < ch->message_count; k++) {
            const Message *m = messages[ch->first_message + k];

            if (m->first || m->retired || !m->has_selector)
                continue;
            if (m->kind == M_REQUEST)
                request_dispatch = 1;
            else if (m->kind == M_EVENT)
                event_dispatch = 1;
        }
        if (request_dispatch)
            emit_dispatcher(b, ch, 1);
        if (event_dispatch)
            emit_dispatcher(b, ch, 0);
    }
    buf_put(b, "/** @} */\n\n");
    free(name.data);
}

/* The header's opening: its comment, guard, includes and the assertions
   of the limits and the imports (P14, rule 1). */
static void emit_prologue(Buf *b)
{
    unsigned i, k;
    Buf guard;

    memset(&guard, 0, sizeof guard);
    put_guard(&guard);
    buf_put(b, "/** @file\n");
    for (i = 0; i < interface_desc.count; i++) {
        buf_spaces(b, INDENT);
        buf_putn(b, lines[interface_desc.first + i].comment,
                 lines[interface_desc.first + i].comment_length);
        buf_ch(b, '\n');
    }
    if (interface_desc.trail_length) {
        buf_spaces(b, INDENT);
        buf_putn(b, interface_desc.trail, interface_desc.trail_length);
        buf_ch(b, '\n');
    }
    buf_put(b, "\n    Generated by tools/mkinterface.c from interfaces/");
    buf_put(b, interface_name);
    buf_put(b, ".desc\n    (docs/spec/interface.md). */\n\n#ifndef ");
    buf_put(b, guard.data);
    buf_put(b, "\n#define ");
    buf_put(b, guard.data);
    buf_put(b, "\n\n#include <stddef.h>\n\n#include \"native_abi.h\"\n");
    for (i = 0; i < import_count; i++)
        if (strcmp(imports[i].header, "native_abi.h") != 0) {
            buf_put(b, "#include \"");
            buf_put(b, imports[i].header);
            buf_put(b, "\"\n");
        }
    buf_ch(b, '\n');
    put_assert_equal(b, "CHANNEL_MAX_BYTES", CHANNEL_MAX_BYTES_VALUE,
                     "the limit the generator checks messages against");
    put_assert_equal(b, "CHANNEL_MAX_HANDLES", CHANNEL_MAX_HANDLES_VALUE,
                     "the limit the generator checks messages against");
    put_assert_equal(b, "CHANNEL_MAX_CAPACITY", CHANNEL_MAX_CAPACITY_VALUE,
                     "the limit the generator checks capacities against");
    for (i = 0; i < import_count; i++) {
        unsigned end = i + 1u < import_count ? imports[i + 1u].items
                                             : import_item_count;

        for (k = imports[i].items; k < end; k++) {
            const ImportItem *item = &import_items[k];
            const Symbol *sym = &symbols[item->symbol];

            if (!item->is_struct) {
                buf_put(b, "_Static_assert(");
                buf_put(b, sym->name);
                buf_put(b, " == ");
                buf_put(b, item->text);
                buf_put(b, ", \"as ");
                buf_put(b, imports[i].header);
                buf_put(b, " gives it\");\n");
            } else {
                put_layout_asserts(b, &structs[sym->index]);
            }
        }
    }
    buf_ch(b, '\n');
    free(guard.data);
}

/* Build the whole header in memory. */
static void build_header(void)
{
    unsigned i;

    emit_prologue(&header);
    for (i = 0; i < decl_count; i++) {
        switch (decls[i].kind) {
        case D_CONST: {
            const ConstDecl *c = &consts[decls[i].index];

            put_described_define(&header, &c->desc, symbols[c->symbol].name,
                                 c->text, c->is_string);
            buf_ch(&header, '\n');
            break;
        }
        case D_RIGHTS:
            put_rights_decl(&header, &rights_decls[decls[i].index]);
            buf_ch(&header, '\n');
            break;
        case D_STRUCT:
            put_struct(&header, &structs[decls[i].index]);
            buf_ch(&header, '\n');
            break;
        case D_CHANNEL:
            emit_channel(&header, &channels[decls[i].index]);
            break;
        case D_SETTING:
            put_setting(&header, &settings[decls[i].index]);
            break;
        }
    }
    buf_put(&header, "#endif\n");
}

/* The command line (P15, rule 2): the description, then the header; exits 0,
   1 for a refused description and 2 for the wrong arguments. */
int main(int argc, char **argv)
{
    FILE *f;
    size_t length;

    if (argc != 3) {
        fprintf(stderr, "usage: %s DESCRIPTION HEADER\n", argv[0]);
        return 2;
    }
    description_path = argv[1];
    length = read_description(argv[1]);
    split_lines(length);
    parse_description();
    resolve_channels();
    register_all_generated();
    build_header();
    f = fopen(argv[2], "wb");
    if (!f || fwrite(header.data, 1u, header.length, f) != header.length ||
        fclose(f) != 0) {
        fprintf(stderr, "mkinterface: cannot write %s\n", argv[2]);
        return 1;
    }
    return 0;
}
