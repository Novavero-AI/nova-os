/* Reading and drawing the reference sheet (sheet.h): the items and
   arithmetic of sheet.py, which is their prototype. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "limb.h"
#include "sheet.h"

#define LINE_MAX_BYTES 4096
#define NAME_MAX_BYTES 64
#define FACES_MAX 16
#define FIELDS_MAX 8
#define HUNDREDTHS 100
#define PERCENT_HUNDREDTHS 10000
#define INK_DIGITS 6
#define ESCAPE_DIGITS 4
#define PPM_MAXVAL 255
#define CHANNEL_MASK 0xFFu

/* UTF-8's encodings of a code point up to U+FFFF (RFC 3629, 3). */
#define UTF8_ONE_BYTE_END 0x80
#define UTF8_TWO_BYTE_END 0x800

OUTLINE sheet_outline;

static SHEET_FACE faces[FACES_MAX];
static char names[FACES_MAX][NAME_MAX_BYTES];
static int face_count;
static long line_number;

static void die(const char *what, const char *detail)
{
    fprintf(stderr, "sheet: line %ld: %s%s%s\n", line_number, what,
            detail ? ": " : "", detail ? detail : "");
    exit(1);
}

/* a / b rounded to the nearest integer, halves up, for a >= 0, b > 0. */
static INT64 rounded(INT64 a, INT64 b)
{
    return (2 * a + b) / (2 * b);
}

/* A decimal of at most two places, in hundredths. */
static INT64 hundredths(const char *text)
{
    INT64 whole = 0, fraction = 0;
    int digits = 0;
    const char *p = text;

    if (*p < '0' || *p > '9')
        die("not a decimal", text);
    while (*p >= '0' && *p <= '9')
        whole = whole * 10 + (*p++ - '0');
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9' && digits < 2) {
            fraction = fraction * 10 + (*p++ - '0');
            digits++;
        }
        if (digits == 0)
            die("not a decimal", text);
    }
    if (*p != '\0')
        die("not a decimal of at most two places", text);
    while (digits++ < 2)
        fraction *= 10;
    return whole * HUNDREDTHS + fraction;
}

static INT64 length(const char *text)
{
    return rounded(hundredths(text) * OUTLINE_ONE, HUNDREDTHS);
}

static UINT32 opacity(const char *text)
{
    INT64 percent = hundredths(text);

    if (percent > PERCENT_HUNDREDTHS)
        die("an opacity past 100 percent", text);
    return (UINT32)rounded(percent * OUTLINE_OPACITY_ONE, PERCENT_HUNDREDTHS);
}

static UINT32 ink(const char *text)
{
    char *end;
    unsigned long value = strtoul(text, &end, 16);

    if (strlen(text) != INK_DIGITS || *end != '\0')
        die("not an ink", text);
    return (UINT32)value;
}

/* text's bytes into out, each \uXXXX as its code point's UTF-8;
   returns their length. */
static UINT64 utf8(const char *text, UINT8 *out)
{
    UINT64 n = 0;

    while (*text) {
        if (text[0] == '\\' && text[1] == 'u') {
            char digits[ESCAPE_DIGITS + 1];
            unsigned long code;

            memcpy(digits, text + 2, ESCAPE_DIGITS);
            digits[ESCAPE_DIGITS] = '\0';
            code = strtoul(digits, 0, 16);
            if (code < UTF8_ONE_BYTE_END) {
                out[n++] = (UINT8)code;
            } else if (code < UTF8_TWO_BYTE_END) {
                out[n++] = (UINT8)(0xC0 | code >> 6);
                out[n++] = (UINT8)(0x80 | (code & 0x3F));
            } else {
                out[n++] = (UINT8)(0xE0 | code >> 12);
                out[n++] = (UINT8)(0x80 | (code >> 6 & 0x3F));
                out[n++] = (UINT8)(0x80 | (code & 0x3F));
            }
            text += 2 + ESCAPE_DIGITS;
        } else {
            out[n++] = (UINT8)*text++;
        }
    }
    return n;
}

/* The bytes of the file at path, and their count in *size. */
static UINT8 *read_all(const char *path, UINT64 *size)
{
    FILE *file = fopen(path, "rb");
    UINT8 *bytes = 0;
    long n = -1;

    if (file && fseek(file, 0, SEEK_END) == 0)
        n = ftell(file);
    if (n >= 0 && fseek(file, 0, SEEK_SET) == 0)
        bytes = malloc((size_t)n + 1);
    if (!bytes || fread(bytes, 1, (size_t)n, file) != (size_t)n)
        die("cannot read", path);
    fclose(file);
    *size = (UINT64)n;
    return bytes;
}

void sheet_typeface(const char *path, TYPEFACE *face)
{
    UINT64 size;
    UINT8 *bytes = read_all(path, &size);

    if (!typeface_load(face, bytes, size))
        die("not a nova-os font", path);
}

void sheet_limb(CANVAS *canvas, INT64 width, INT64 height)
{
    LIMB limb;
    INT64 x, y;

    canvas->width = canvas->stride = width;
    canvas->height = height;
    if (!limb_init(&limb, width, height))
        die("a size Limb does not draw", 0);
    canvas->pixels = malloc((size_t)(width * height) * sizeof *canvas->pixels);
    if (!canvas->pixels)
        die("out of memory", 0);
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++)
            canvas->pixels[y * canvas->stride + x] = limb_pixel(&limb, x, y);
}

/* The face named name, loaded from the directory fonts once. */
static const SHEET_FACE *face(const char *fonts, const char *name)
{
    char path[LINE_MAX_BYTES];
    int i;

    for (i = 0; i < face_count; i++)
        if (strcmp(faces[i].name, name) == 0)
            return &faces[i];
    if (face_count == FACES_MAX || strlen(name) >= NAME_MAX_BYTES)
        die("too many faces, or a name too long", name);
    snprintf(path, sizeof path, "%s/%s.nvf", fonts, name);
    sheet_typeface(path, &faces[face_count].face);
    strcpy(names[face_count], name);
    faces[face_count].name = names[face_count];
    return &faces[face_count++];
}

/* Split line at its first count spaces into fields; returns the rest. */
static char *split(char *line, char **fields, int count)
{
    int i;

    for (i = 0; i < count; i++) {
        char *space = strchr(line, ' ');

        if (!space)
            die("too few fields", 0);
        *space = '\0';
        fields[i] = line;
        line = space + 1;
    }
    return line;
}

/* Every field of line, split at each space; fails unless there are
   count. */
static void split_all(char *line, char **fields, int count)
{
    char *rest = split(line, fields, count - 1);

    if (strchr(rest, ' '))
        die("too many fields", 0);
    fields[count - 1] = rest;
}

static void fill(const CANVAS *canvas, UINT32 colour, UINT32 alpha)
{
    if (!outline_fill(&sheet_outline, canvas, colour, alpha))
        die("an outline that does not fill", 0);
}

/* Draw text or wrap's lines from cursor, moving it to the last
   baseline. */
static void text_lines(const CANVAS *canvas, const char *fonts, char *line,
                       int wrap, INT64 left, INT64 *cursor, SHEET_TEXT *text)
{
    static UINT8 bytes[LINE_MAX_BYTES * 2];
    char *f[FIELDS_MAX];
    char *rest = split(line, f, wrap ? 5 : 4);
    const SHEET_FACE *t = face(fonts, f[1]);
    INT64 size = length(f[2]), limit = wrap ? length(f[3]) : 0;
    UINT32 colour = ink(f[wrap ? 4 : 3]);
    UINT64 count = utf8(rest, bytes), start = 0;

    do {
        UINT64 end = count - start, next = count - start;

        if (wrap)
            end = typeset_break(&t->face, size, bytes + start, count - start,
                                limit, &next);
        *cursor += typeset_line_height(&t->face, size);
        text(canvas, t, size, left, *cursor, bytes + start, end, colour);
        start += next;
    } while (start < count);
}

void sheet_render(const char *path, const char *fonts, SHEET_TEXT *text,
                  CANVAS *canvas)
{
    char line[LINE_MAX_BYTES];
    char *f[FIELDS_MAX];
    INT64 left = 0, cursor = 0;
    FILE *sheet = fopen(path, "r");
    int sized = 0;

    line_number = 0;
    if (!sheet)
        die("cannot read", path);
    while (fgets(line, sizeof line, sheet)) {
        size_t n = strlen(line);

        line_number++;
        if (n && line[n - 1] == '\n')
            line[--n] = '\0';
        if (n == 0 || line[0] == '#')
            continue;
        if (!sized) {
            split_all(line, f, 3);
            if (strcmp(f[0], "sheet") != 0)
                die("the first item is not the sheet's size", 0);
            sheet_limb(canvas, atol(f[1]), atol(f[2]));
            sized = 1;
        } else if (strncmp(line, "at ", 3) == 0) {
            split_all(line, f, 3);
            left = length(f[1]);
            cursor = atol(f[2]);
        } else if (strncmp(line, "text ", 5) == 0) {
            text_lines(canvas, fonts, line, 0, left, &cursor, text);
        } else if (strncmp(line, "wrap ", 5) == 0) {
            text_lines(canvas, fonts, line, 1, left, &cursor, text);
        } else if (strncmp(line, "rect ", 5) == 0) {
            split_all(line, f, 8);
            outline_start(&sheet_outline);
            outline_rounded_rect(&sheet_outline, length(f[1]), length(f[2]),
                                 length(f[3]), length(f[4]), length(f[5]));
            fill(canvas, ink(f[6]), opacity(f[7]));
        } else if (strncmp(line, "circle ", 7) == 0) {
            split_all(line, f, 6);
            outline_start(&sheet_outline);
            outline_circle(&sheet_outline, length(f[1]), length(f[2]),
                           length(f[3]));
            fill(canvas, ink(f[4]), opacity(f[5]));
        } else {
            die("an unknown item", line);
        }
    }
    fclose(sheet);
    if (!sized)
        die("no sheet", 0);
}

int sheet_write(const CANVAS *canvas)
{
    INT64 x, y;

    printf("P6\n%ld %ld\n%d\n", (long)canvas->width, (long)canvas->height,
           PPM_MAXVAL);
    for (y = 0; y < canvas->height; y++)
        for (x = 0; x < canvas->width; x++) {
            UINT32 p = canvas->pixels[y * canvas->stride + x];

            putchar((int)(p >> 16 & CHANNEL_MASK));
            putchar((int)(p >> 8 & CHANNEL_MASK));
            putchar((int)(p & CHANNEL_MASK));
        }
    return fflush(stdout) != 0 || ferror(stdout);
}
