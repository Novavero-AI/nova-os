/* mkfont: writes a nova-os font (include/font_format.h,
   docs/spec/font-format.md) from a TrueType font, keeping the glyphs of
   the code points given and glyph 0, the missing-glyph shape: each
   glyph's advance and outline, composites resolved into their
   components' contours, the character map of the code points kept, and
   the pair kerning of the font's GPOS 'kern' feature among the glyphs
   kept, as a matrix of classes. Hinting, GSUB and every other GPOS
   feature are left out.

   usage: mkfont FONT OUT RANGE...

   RANGE is a code point, or two joined by '-', in hexadecimal: "20-7E".
   A code point the font maps to no glyph is left out.

   Formats, from the OpenType specification 1.9.1: the table directory;
   head, hhea, maxp, hmtx, OS/2, loca and glyf; cmap format 4 of the
   Windows Unicode BMP encoding (3, 1); GDEF's glyph classes; and GPOS's
   script, feature and lookup lists, pair adjustment lookups (type 2,
   formats 1 and 2) and extension lookups (type 9), with coverage and
   class definition tables. Within a lookup the first subtable that
   applies to a pair gives its adjustment, and the adjustments of the
   feature's lookups add, in LookupList order. A pair adjustment
   subtable applies when its coverage holds the first glyph and, in
   format 1, its pair set holds the second; in format 2 the glyphs'
   classes pick the adjustment, 0 where the matrix gives none. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font_format.h"

#define FIRST_RANGE_ARGUMENT 3
#define CODE_POINT_MAX 0xFFFF

/* The table directory (OpenType, "Organization of an OpenType font"):
   TrueType outlines' sfntVersion, and each record's fields. */
#define SFNT_TRUETYPE 0x00010000u
#define DIRECTORY_TABLES 4
#define DIRECTORY_RECORDS 12
#define RECORD_SIZE 16
#define RECORD_OFFSET 8
#define RECORD_LENGTH 12

/* Offsets within the tables read. */
#define HEAD_UNITS 18
#define HEAD_LOCA_FORMAT 50
#define MAXP_GLYPHS 4
#define HHEA_METRICS 34
#define HMTX_RECORD 4
#define OS2_VERSION 0
#define OS2_TYPO_ASCENDER 68
#define OS2_TYPO_DESCENDER 70
#define OS2_TYPO_LINE_GAP 72
#define OS2_X_HEIGHT 86
#define OS2_CAP_HEIGHT 88
#define OS2_HEIGHTS_VERSION 2

/* cmap: its encoding records, and format 4's fields. */
#define CMAP_TABLES 2
#define CMAP_RECORDS 4
#define CMAP_RECORD_SIZE 8
#define PLATFORM_WINDOWS 3
#define ENCODING_UNICODE_BMP 1
#define CMAP_FORMAT_4 4
#define FORMAT4_SEGMENTS_X2 6
#define FORMAT4_END_CODES 14
#define GLYPH_ID_MASK 0xFFFF

/* glyf: a glyph's header, a simple glyph's flags and a component's. */
#define GLYPH_CONTOURS 0
#define GLYPH_HEADER_SIZE 10
#define ON_CURVE_POINT 0x01
#define X_SHORT_VECTOR 0x02
#define Y_SHORT_VECTOR 0x04
#define REPEAT_FLAG 0x08
#define X_IS_SAME_OR_POSITIVE 0x10
#define Y_IS_SAME_OR_POSITIVE 0x20
#define ARG_1_AND_2_ARE_WORDS 0x0001
#define ARGS_ARE_XY_VALUES 0x0002
#define WE_HAVE_A_SCALE 0x0008
#define MORE_COMPONENTS 0x0020
#define WE_HAVE_AN_X_AND_Y_SCALE 0x0040
#define WE_HAVE_A_TWO_BY_TWO 0x0080
#define SCALED (WE_HAVE_A_SCALE | WE_HAVE_AN_X_AND_Y_SCALE | WE_HAVE_A_TWO_BY_TWO)
#define COMPONENT_DEPTH_MAX 8
#define COORDINATE_MIN (-32768)
#define COORDINATE_MAX 32767

/* GDEF's glyph class of a mark. */
#define GDEF_CLASSES 4
#define GLYPH_CLASS_MARK 3

/* GPOS: its lists, a lookup's fields and flags, the lookup types read,
   and a pair adjustment's fields. */
#define GPOS_SCRIPTS 4
#define GPOS_FEATURES 6
#define GPOS_LOOKUPS 8
#define LIST_RECORD_SIZE 6
#define TAG_SIZE 4
#define LANGSYS_FEATURES 4
#define FEATURE_LOOKUPS 2
#define LOOKUP_FLAG 2
#define LOOKUP_SUBTABLES 4
#define LOOKUP_SUBTABLE_OFFSETS 6
#define IGNORE_BASE_GLYPHS 0x0002
#define IGNORE_LIGATURES 0x0004
#define LOOKUP_PAIR 2
#define LOOKUP_EXTENSION 9
#define EXTENSION_TYPE 2
#define EXTENSION_OFFSET 4
#define PAIR_COVERAGE 2
#define PAIR_VALUE_FORMAT_1 4
#define PAIR_VALUE_FORMAT_2 6
#define PAIR1_SETS 8
#define PAIR1_SET_OFFSETS 10
#define PAIR2_CLASS_DEF_1 8
#define PAIR2_CLASS_DEF_2 10
#define PAIR2_CLASS_1_COUNT 12
#define PAIR2_CLASS_2_COUNT 14
#define PAIR2_RECORDS 16
#define VALUE_X_ADVANCE 0x0004
#define VALUE_FIELD_SIZE 2
#define VALUE_FIELDS_MASK 0xFF
#define KERN_MIN (-32768)
#define KERN_MAX 32767
#define LOOKUPS_MAX 1024

/* An outline as the format holds it, and the glyphs, contours and points
   kept. */
struct point {
    int32_t x, y;
    int on;
};

struct glyph {
    uint16_t source;   /* its glyph ID in the TrueType font */
    uint16_t advance;
    uint32_t first_contour, contours;
    unsigned left_class, right_class;
};

static const uint8_t *font;
static size_t font_size;
static uint32_t glyf_at, glyf_length, loca_at, hmtx_at, cmap4_at;
static uint32_t gpos_at, gdef_at;
static unsigned source_glyphs, metric_count, loca_long;

static struct glyph *glyphs;
static size_t glyph_count;
static uint32_t *ends;
static size_t contour_count, contour_capacity;
static struct point *points;
static size_t point_count, point_capacity;

/* The map: for each code point kept, its glyph among those kept. */
static uint32_t *codes;
static uint16_t *code_glyphs;
static size_t code_count;

static uint16_t kern_lookups[LOOKUPS_MAX];
static size_t kern_lookup_count;

static void die(const char *what, const char *detail)
{
    fprintf(stderr, "mkfont: %s%s%s\n", what, detail ? ": " : "",
            detail ? detail : "");
    exit(1);
}

static void *allocate(size_t count, size_t size)
{
    void *p = calloc(count ? count : 1, size);

    if (!p)
        die("out of memory", 0);
    return p;
}

/* ------------------------------------------------------------------------
   Reading the font: big-endian fields, each checked against its end. */

static uint32_t field(size_t at, unsigned bytes)
{
    uint32_t value = 0;
    unsigned i;

    if (at > font_size || bytes > font_size - at)
        die("the font ends inside a table", 0);
    for (i = 0; i < bytes; i++)
        value = value << 8 | font[at + i];
    return value;
}

static unsigned u8(size_t at) { return (unsigned)field(at, 1); }
static unsigned u16(size_t at) { return (unsigned)field(at, 2); }
static uint32_t u32(size_t at) { return field(at, 4); }

static int32_t s16(size_t at)
{
    uint32_t v = field(at, 2);

    return v & 0x8000 ? (int32_t)v - 0x10000 : (int32_t)v;
}

static int32_t s8(size_t at)
{
    uint32_t v = field(at, 1);

    return v & 0x80 ? (int32_t)v - 0x100 : (int32_t)v;
}

/* Whether the four bytes at at are tag. */
static int tag_at(size_t at, const char *tag)
{
    return field(at, TAG_SIZE) ==
           ((uint32_t)(uint8_t)tag[0] << 24 | (uint32_t)(uint8_t)tag[1] << 16 |
            (uint32_t)(uint8_t)tag[2] << 8 | (uint32_t)(uint8_t)tag[3]);
}

/* The table tagged tag; its length in *length. Dies without it unless
   optional, then returns 0. */
static uint32_t table(const char *tag, uint32_t *length, int optional)
{
    unsigned count = u16(DIRECTORY_TABLES), i;

    for (i = 0; i < count; i++) {
        size_t record = DIRECTORY_RECORDS + (size_t)i * RECORD_SIZE;

        if (tag_at(record, tag)) {
            uint32_t at = u32(record + RECORD_OFFSET);
            uint32_t size = u32(record + RECORD_LENGTH);

            if (at > font_size || size > font_size - at)
                die("a table lies outside the font", tag);
            if (length)
                *length = size;
            return at;
        }
    }
    if (!optional)
        die("the font has no table", tag);
    return 0;
}

static void read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data;
    long size = -1;

    if (f && fseek(f, 0, SEEK_END) == 0)
        size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0)
        die("cannot read", path);
    data = allocate((size_t)size, 1);
    if (fread(data, 1, (size_t)size, f) != (size_t)size || fclose(f) != 0)
        die("cannot read", path);
    font = data;
    font_size = (size_t)size;
}

/* ------------------------------------------------------------------------
   The character map: cmap format 4 of (3, 1). */

static void find_cmap(void)
{
    uint32_t cmap = table("cmap", 0, 0);
    unsigned count = u16(cmap + CMAP_TABLES), i;

    for (i = 0; i < count; i++) {
        size_t record = cmap + CMAP_RECORDS + (size_t)i * CMAP_RECORD_SIZE;

        if (u16(record) == PLATFORM_WINDOWS &&
            u16(record + 2) == ENCODING_UNICODE_BMP) {
            cmap4_at = cmap + u32(record + 4);
            if (u16(cmap4_at) != CMAP_FORMAT_4)
                die("the font's (3, 1) cmap is not format 4", 0);
            return;
        }
    }
    die("the font has no (3, 1) cmap", 0);
}

/* The glyph ID the font maps code to, 0 for none. */
static unsigned glyph_of(uint32_t code)
{
    unsigned segments = u16(cmap4_at + FORMAT4_SEGMENTS_X2) / 2, i;
    size_t end_codes = cmap4_at + FORMAT4_END_CODES;
    size_t start_codes = end_codes + 2 * (size_t)segments + 2;
    size_t deltas = start_codes + 2 * (size_t)segments;
    size_t range_offsets = deltas + 2 * (size_t)segments;

    for (i = 0; i < segments; i++) {
        unsigned start, offset;
        int32_t delta;

        if (u16(end_codes + 2 * i) < code)
            continue;
        start = u16(start_codes + 2 * i);
        if (start > code)
            return 0;
        delta = s16(deltas + 2 * i);
        offset = u16(range_offsets + 2 * i);
        if (offset == 0)
            return (unsigned)((int32_t)code + delta) & GLYPH_ID_MASK;
        offset = u16(range_offsets + 2 * i + offset + 2 * (code - start));
        return offset ? (unsigned)((int32_t)offset + delta) & GLYPH_ID_MASK
                      : 0;
    }
    return 0;
}

/* ------------------------------------------------------------------------
   Outlines. */

static void add_point(int32_t x, int32_t y, int on)
{
    if (x < COORDINATE_MIN || x > COORDINATE_MAX || y < COORDINATE_MIN ||
        y > COORDINATE_MAX)
        die("a point lies outside 16 bits", 0);
    if (point_count == point_capacity) {
        point_capacity = point_capacity ? 2 * point_capacity : 1024;
        points = realloc(points, point_capacity * sizeof *points);
        if (!points)
            die("out of memory", 0);
    }
    points[point_count].x = x;
    points[point_count].y = y;
    points[point_count].on = on;
    point_count++;
}

/* End a contour one past point end. */
static void end_contour(size_t end)
{
    if (contour_count == contour_capacity) {
        contour_capacity = contour_capacity ? 2 * contour_capacity : 256;
        ends = realloc(ends, contour_capacity * sizeof *ends);
        if (!ends)
            die("out of memory", 0);
    }
    ends[contour_count++] = (uint32_t)end;
}

/* The bytes of glyph id in glyf, and their length in *length. */
static uint32_t glyph_data(unsigned id, uint32_t *length)
{
    uint32_t start, end;

    if (id >= source_glyphs)
        die("a component names a glyph past the font's", 0);
    if (loca_long) {
        start = u32(loca_at + 4 * (size_t)id);
        end = u32(loca_at + 4 * (size_t)id + 4);
    } else {
        start = 2 * u16(loca_at + 2 * (size_t)id);
        end = 2 * u16(loca_at + 2 * (size_t)id + 2);
    }
    if (start > end || end > glyf_length)
        die("loca names bytes outside glyf", 0);
    *length = end - start;
    return glyf_at + start;
}

/* Add a simple glyph's contours, at at, moved by (dx, dy). */
static void add_simple(uint32_t at, uint32_t length, unsigned contours,
                       int32_t dx, int32_t dy)
{
    size_t first = point_count, count, i;
    size_t ends_at = at + GLYPH_HEADER_SIZE;
    size_t p = ends_at + 2 * (size_t)contours;
    unsigned last = 0, c;
    uint8_t *flags;
    int32_t x = dx, y = dy;

    if (contours == 0)
        return;
    for (c = 0; c < contours; c++) {
        unsigned end = u16(ends_at + 2 * (size_t)c);

        if (c > 0 && end <= last)
            die("a glyph's contour ends do not rise", 0);
        last = end;
    }
    count = (size_t)last + 1;
    p += 2 + u16(p); /* the instructions */
    flags = allocate(count, 1);
    for (i = 0; i < count;) {
        unsigned flag = u8(p++), repeat = 0;

        if (flag & REPEAT_FLAG)
            repeat = u8(p++);
        if (repeat >= count - i)
            repeat = (unsigned)(count - i - 1);
        do
            flags[i++] = (uint8_t)flag;
        while (repeat-- > 0);
    }
    for (i = 0; i < count; i++) {
        if (flags[i] & X_SHORT_VECTOR) {
            x += flags[i] & X_IS_SAME_OR_POSITIVE ? (int32_t)u8(p)
                                                  : -(int32_t)u8(p);
            p += 1;
        } else if (!(flags[i] & X_IS_SAME_OR_POSITIVE)) {
            x += s16(p);
            p += 2;
        }
        add_point(x, 0, flags[i] & ON_CURVE_POINT);
    }
    for (i = 0; i < count; i++) {
        if (flags[i] & Y_SHORT_VECTOR) {
            y += flags[i] & Y_IS_SAME_OR_POSITIVE ? (int32_t)u8(p)
                                                  : -(int32_t)u8(p);
            p += 1;
        } else if (!(flags[i] & Y_IS_SAME_OR_POSITIVE)) {
            y += s16(p);
            p += 2;
        }
        if (y < COORDINATE_MIN || y > COORDINATE_MAX)
            die("a point lies outside 16 bits", 0);
        points[first + i].y = y;
    }
    if (p > at + length)
        die("a glyph's points run past its bytes", 0);
    free(flags);
    for (c = 0; c < contours; c++)
        end_contour(first + u16(ends_at + 2 * (size_t)c) + 1);
}

/* Add glyph id's contours, moved by (dx, dy): a simple glyph's own, a
   composite's components' at their offsets. */
static void add_glyph(unsigned id, int32_t dx, int32_t dy, int depth)
{
    uint32_t length, at = glyph_data(id, &length);
    int32_t contours;
    size_t p;
    unsigned flags;

    if (length == 0)
        return;
    contours = s16(at + GLYPH_CONTOURS);
    if (contours >= 0) {
        add_simple(at, length, (unsigned)contours, dx, dy);
        return;
    }
    if (depth == COMPONENT_DEPTH_MAX)
        die("components nest too deep", 0);
    p = at + GLYPH_HEADER_SIZE;
    do {
        int32_t x, y;
        unsigned component;

        flags = u16(p);
        component = u16(p + 2);
        if (!(flags & ARGS_ARE_XY_VALUES))
            die("a component placed by point numbers", 0);
        if (flags & SCALED)
            die("a scaled component", 0);
        if (flags & ARG_1_AND_2_ARE_WORDS) {
            x = s16(p + 4);
            y = s16(p + 6);
            p += 8;
        } else {
            x = s8(p + 4);
            y = s8(p + 5);
            p += 6;
        }
        add_glyph(component, dx + x, dy + y, depth + 1);
    } while (flags & MORE_COMPONENTS);
    if (p > at + length)
        die("a composite's components run past its bytes", 0);
}

static unsigned advance_of(unsigned id)
{
    unsigned record = id < metric_count ? id : metric_count - 1;

    return u16(hmtx_at + HMTX_RECORD * (size_t)record);
}

/* Keep glyph id, if it is not kept yet; returns its index among those
   kept. */
static size_t keep(unsigned id)
{
    size_t i, first = contour_count;

    for (i = 0; i < glyph_count; i++)
        if (glyphs[i].source == id)
            return i;
    add_glyph(id, 0, 0, 0);
    glyphs[glyph_count].source = (uint16_t)id;
    glyphs[glyph_count].advance = (uint16_t)advance_of(id);
    glyphs[glyph_count].first_contour = (uint32_t)first;
    glyphs[glyph_count].contours = (uint32_t)(contour_count - first);
    if (glyphs[glyph_count].contours > GLYPH_ID_MASK)
        die("a glyph has more contours than 16 bits count", 0);
    return glyph_count++;
}

/* ------------------------------------------------------------------------
   Coverage and class definitions (OpenType, common table formats). */

/* The coverage index of glyph id in the coverage table at at, or -1. */
static long coverage(size_t at, unsigned id)
{
    unsigned format = u16(at), count = u16(at + 2), i;

    for (i = 0; i < count; i++) {
        if (format == 1) {
            if (u16(at + 4 + 2 * (size_t)i) == id)
                return (long)i;
        } else if (format == 2) {
            size_t range = at + 4 + 6 * (size_t)i;

            if (u16(range) <= id && id <= u16(range + 2))
                return (long)(u16(range + 4) + id - u16(range));
        } else {
            die("a coverage table of an unknown format", 0);
        }
    }
    return -1;
}

/* The class of glyph id in the class definition at at. */
static unsigned class_of(size_t at, unsigned id)
{
    unsigned format = u16(at), i, count;

    if (format == 1) {
        unsigned start = u16(at + 2);

        count = u16(at + 4);
        return id >= start && id - start < count
                   ? u16(at + 6 + 2 * (size_t)(id - start))
                   : 0;
    }
    if (format != 2)
        die("a class definition of an unknown format", 0);
    count = u16(at + 2);
    for (i = 0; i < count; i++) {
        size_t range = at + 4 + 6 * (size_t)i;

        if (u16(range) <= id && id <= u16(range + 2))
            return u16(range + 4);
    }
    return 0;
}

/* ------------------------------------------------------------------------
   Kerning. */

static unsigned value_size(unsigned format)
{
    unsigned bits = format & VALUE_FIELDS_MASK, size = 0;

    for (; bits; bits >>= 1)
        size += (bits & 1) * VALUE_FIELD_SIZE;
    return size;
}

/* The kern feature's lookups, from the default language system of the
   'latn' script, or else of 'DFLT', in LookupList order; none if neither
   has one, as a layout engine then applies none. */
static void find_kern_lookups(void)
{
    size_t scripts, features, script = 0, langsys;
    unsigned count, i, j;

    if (!gpos_at)
        return;
    scripts = gpos_at + u16(gpos_at + GPOS_SCRIPTS);
    features = gpos_at + u16(gpos_at + GPOS_FEATURES);
    count = u16(scripts);
    for (i = 0; i < count; i++) {
        size_t record = scripts + 2 + (size_t)i * LIST_RECORD_SIZE;

        if (tag_at(record, "latn") ||
            (!script && tag_at(record, "DFLT")))
            script = scripts + u16(record + TAG_SIZE);
    }
    if (!script || !u16(script))
        return;
    langsys = script + u16(script);
    count = u16(langsys + LANGSYS_FEATURES);
    for (i = 0; i < count; i++) {
        unsigned index = u16(langsys + LANGSYS_FEATURES + 2 + 2 * (size_t)i);
        size_t record = features + 2 + (size_t)index * LIST_RECORD_SIZE;
        size_t feature;

        if (index >= u16(features))
            die("a language system names a feature past the list", 0);
        if (!tag_at(record, "kern"))
            continue;
        feature = features + u16(record + TAG_SIZE);
        for (j = 0; j < u16(feature + FEATURE_LOOKUPS); j++) {
            unsigned lookup = u16(feature + FEATURE_LOOKUPS + 2 + 2 * (size_t)j);
            size_t k = 0;

            while (k < kern_lookup_count && kern_lookups[k] < lookup)
                k++;
            if (k < kern_lookup_count && kern_lookups[k] == lookup)
                continue;
            if (kern_lookup_count == LOOKUPS_MAX)
                die("the kern feature has too many lookups", 0);
            memmove(kern_lookups + k + 1, kern_lookups + k,
                    (kern_lookup_count - k) * sizeof *kern_lookups);
            kern_lookups[k] = (uint16_t)lookup;
            kern_lookup_count++;
        }
    }
}

/* Whether the pair adjustment subtable at at applies to (first, second),
   and if it does its adjustment in *value. */
static int pair_value(size_t at, unsigned first, unsigned second,
                      int32_t *value)
{
    unsigned format = u16(at), format1 = u16(at + PAIR_VALUE_FORMAT_1);
    unsigned format2 = u16(at + PAIR_VALUE_FORMAT_2);
    unsigned size1 = value_size(format1), size2 = value_size(format2), i;
    long index = coverage(at + u16(at + PAIR_COVERAGE), first);

    if (format1 != VALUE_X_ADVANCE || format2 != 0)
        die("a pair adjustment other than the first glyph's advance", 0);
    if (index < 0)
        return 0;
    if (format == 1) {
        size_t set;
        unsigned count;

        if ((unsigned long)index >= u16(at + PAIR1_SETS))
            die("a coverage index past the pair sets", 0);
        set = at + u16(at + PAIR1_SET_OFFSETS + 2 * (size_t)index);
        count = u16(set);
        for (i = 0; i < count; i++) {
            size_t record = set + 2 + (size_t)i * (2 + size1 + size2);

            if (u16(record) == second) {
                *value = s16(record + 2);
                return 1;
            }
        }
        return 0;
    }
    if (format == 2) {
        unsigned class1 = class_of(at + u16(at + PAIR2_CLASS_DEF_1), first);
        unsigned class2 = class_of(at + u16(at + PAIR2_CLASS_DEF_2), second);
        unsigned count1 = u16(at + PAIR2_CLASS_1_COUNT);
        unsigned count2 = u16(at + PAIR2_CLASS_2_COUNT);

        if (class1 >= count1 || class2 >= count2)
            die("a class past a pair adjustment's matrix", 0);
        *value = s16(at + PAIR2_RECORDS +
                     ((size_t)class1 * count2 + class2) * (size1 + size2));
        return 1;
    }
    die("a pair adjustment of an unknown format", 0);
    return 0;
}

/* The kern feature's adjustment of the pair (first, second), in font
   units. */
static int32_t kerning(unsigned first, unsigned second)
{
    size_t lookups = gpos_at + u16(gpos_at + GPOS_LOOKUPS), i;
    int32_t total = 0;

    for (i = 0; i < kern_lookup_count; i++) {
        size_t lookup;
        unsigned type, count, s;

        if (kern_lookups[i] >= u16(lookups))
            die("a feature names a lookup past the list", 0);
        lookup = lookups + u16(lookups + 2 + 2 * (size_t)kern_lookups[i]);
        type = u16(lookup);
        count = u16(lookup + LOOKUP_SUBTABLES);
        if (u16(lookup + LOOKUP_FLAG) & (IGNORE_BASE_GLYPHS | IGNORE_LIGATURES))
            die("a kern lookup skips base glyphs or ligatures", 0);
        for (s = 0; s < count; s++) {
            size_t subtable = lookup + u16(lookup + LOOKUP_SUBTABLE_OFFSETS +
                                            2 * (size_t)s);
            unsigned kind = type;
            int32_t value;

            if (type == LOOKUP_EXTENSION) {
                kind = u16(subtable + EXTENSION_TYPE);
                subtable += u32(subtable + EXTENSION_OFFSET);
            }
            if (kind != LOOKUP_PAIR)
                die("a kern lookup that is not a pair adjustment", 0);
            if (pair_value(subtable, first, second, &value)) {
                total += value;
                break;
            }
        }
    }
    if (total < KERN_MIN || total > KERN_MAX)
        die("a pair's kerning past 16 bits", 0);
    return total;
}

/* Fail if a kept glyph is a mark: the format draws every glyph as a
   base glyph, with no mark attachment. */
static void check_no_marks(void)
{
    size_t classes, i;

    if (!gdef_at || !u16(gdef_at + GDEF_CLASSES))
        return;
    classes = gdef_at + u16(gdef_at + GDEF_CLASSES);
    for (i = 0; i < glyph_count; i++)
        if (class_of(classes, glyphs[i].source) == GLYPH_CLASS_MARK)
            die("a code point given maps to a mark glyph", 0);
}

/* Give each glyph its kerning classes: two glyphs share a left class if
   they kern alike before every glyph, and a right class if they kern
   alike after every glyph. Class 0 of each side is the glyphs that kern
   with none. The matrix's row r and column c are any member's. Returns
   the matrix of every pair, glyph_count by glyph_count. */
static int32_t *kern_classes(unsigned *left_classes, unsigned *right_classes,
                             size_t *pairs)
{
    size_t n = glyph_count, a, b, c;
    int32_t *matrix = allocate(n * n, sizeof *matrix);
    size_t *left = allocate(n + 1, sizeof *left);
    size_t *right = allocate(n + 1, sizeof *right);
    unsigned lefts = 1, rights = 1;

    *pairs = 0;
    for (a = 0; a < n; a++)
        for (b = 0; b < n; b++) {
            matrix[a * n + b] = kerning(glyphs[a].source, glyphs[b].source);
            *pairs += matrix[a * n + b] != 0;
        }
    /* left[k] and right[k] hold a member of class k; a glyph whose row
       or column is all zero is class 0. */
    for (a = 0; a < n; a++) {
        int zero = 1;

        for (b = 0; b < n; b++)
            zero &= matrix[a * n + b] == 0;
        glyphs[a].left_class = 0;
        if (zero)
            continue;
        for (c = 1; c < lefts; c++)
            if (memcmp(matrix + a * n, matrix + left[c] * n,
                       n * sizeof *matrix) == 0)
                break;
        if (c == lefts)
            left[lefts++] = a;
        glyphs[a].left_class = (unsigned)c;
    }
    for (b = 0; b < n; b++) {
        int zero = 1;

        for (a = 0; a < n; a++)
            zero &= matrix[a * n + b] == 0;
        glyphs[b].right_class = 0;
        if (zero)
            continue;
        for (c = 1; c < rights; c++) {
            for (a = 0; a < n; a++)
                if (matrix[a * n + b] != matrix[a * n + right[c]])
                    break;
            if (a == n)
                break;
        }
        if (c == rights)
            right[rights++] = b;
        glyphs[b].right_class = (unsigned)c;
    }
    free(left);
    free(right);
    *left_classes = lefts;
    *right_classes = rights;
    return matrix;
}

/* ------------------------------------------------------------------------
   Writing. */

struct out {
    uint8_t *bytes;
    size_t size, at;
};

static void put(struct out *o, uint32_t value, unsigned bytes)
{
    unsigned i;

    for (i = 0; i < bytes; i++)
        o->bytes[o->at++] = (uint8_t)(value >> 8 * i);
}

static uint32_t parse_code(const char *text, const char **end)
{
    char *stop;
    unsigned long code = strtoul(text, &stop, 16);

    if (stop == text || code > CODE_POINT_MAX)
        die("a range is not hexadecimal code points", text);
    *end = stop;
    return (uint32_t)code;
}

/* Note each code point of the ranges given that the font maps. */
static void read_ranges(int count, char **ranges)
{
    static uint8_t wanted[CODE_POINT_MAX + 1];
    uint32_t code;
    int i;

    for (i = 0; i < count; i++) {
        const char *end;
        uint32_t first = parse_code(ranges[i], &end), last = first;

        if (*end == '-')
            last = parse_code(end + 1, &end);
        if (*end != '\0' || last < first)
            die("a range is not FIRST or FIRST-LAST", ranges[i]);
        for (code = first; code <= last; code++)
            wanted[code] = 1;
    }
    codes = allocate(CODE_POINT_MAX + 1, sizeof *codes);
    code_glyphs = allocate(CODE_POINT_MAX + 1, sizeof *code_glyphs);
    glyphs = allocate(CODE_POINT_MAX + 2, sizeof *glyphs);
    keep(0);
    for (code = 0; code <= CODE_POINT_MAX; code++) {
        unsigned id;

        if (!wanted[code] || !(id = glyph_of(code)))
            continue;
        codes[code_count] = code;
        code_glyphs[code_count] = (uint16_t)keep(id);
        code_count++;
    }
}

/* The map's ranges: runs of consecutive code points with consecutive
   glyphs. Writes them if o is given; returns how many. */
static size_t write_ranges(struct out *o)
{
    size_t count = 0, i = 0;

    while (i < code_count) {
        size_t run = 1;

        while (i + run < code_count && codes[i + run] == codes[i] + run &&
               code_glyphs[i + run] == code_glyphs[i] + run)
            run++;
        if (o) {
            put(o, codes[i], 4);
            put(o, (uint32_t)run, 2);
            put(o, code_glyphs[i], 2);
        }
        count++;
        i += run;
    }
    return count;
}

int main(int argc, char **argv)
{
    uint32_t head, os2, length;
    unsigned left_classes, right_classes;
    size_t ranges, pairs, i, j, size;
    int32_t *matrix;
    struct out o;
    FILE *f;

    if (argc <= FIRST_RANGE_ARGUMENT) {
        fprintf(stderr, "usage: mkfont FONT OUT RANGE...\n");
        return 2;
    }
    read_file(argv[1]);
    if (u32(0) != SFNT_TRUETYPE)
        die("not a font with TrueType outlines", argv[1]);
    head = table("head", 0, 0);
    loca_long = u16(head + HEAD_LOCA_FORMAT) != 0;
    source_glyphs = u16(table("maxp", 0, 0) + MAXP_GLYPHS);
    metric_count = u16(table("hhea", 0, 0) + HHEA_METRICS);
    hmtx_at = table("hmtx", &length, 0);
    if (metric_count == 0 || length < HMTX_RECORD * (size_t)metric_count)
        die("hmtx is shorter than hhea gives", 0);
    loca_at = table("loca", &length, 0);
    if (length < (loca_long ? 4u : 2u) * ((size_t)source_glyphs + 1))
        die("loca is shorter than maxp gives", 0);
    glyf_at = table("glyf", &glyf_length, 0);
    os2 = table("OS/2", 0, 0);
    if (u16(os2 + OS2_VERSION) < OS2_HEIGHTS_VERSION)
        die("OS/2 gives no x-height and cap height", 0);
    gpos_at = table("GPOS", 0, 1);
    gdef_at = table("GDEF", 0, 1);
    find_cmap();
    read_ranges(argc - FIRST_RANGE_ARGUMENT, argv + FIRST_RANGE_ARGUMENT);
    check_no_marks();
    find_kern_lookups();
    matrix = kern_classes(&left_classes, &right_classes, &pairs);
    ranges = write_ranges(0);

    size = FONT_HEADER_SIZE + ranges * FONT_RANGE_SIZE +
           glyph_count * FONT_GLYPH_SIZE + contour_count * FONT_CONTOUR_SIZE +
           point_count * FONT_POINT_SIZE +
           (size_t)left_classes * right_classes * FONT_KERN_SIZE +
           point_count * FONT_FLAG_SIZE;
    o.bytes = allocate(size, 1);
    o.size = size;
    o.at = 0;
    memcpy(o.bytes, FONT_MAGIC, FONT_MAGIC_SIZE);
    o.at = FONT_MAGIC_SIZE;
    put(&o, FONT_VERSION, 4);
    put(&o, u16(head + HEAD_UNITS), 2);
    put(&o, (uint32_t)s16(os2 + OS2_TYPO_ASCENDER), 2);
    put(&o, (uint32_t)s16(os2 + OS2_TYPO_DESCENDER), 2);
    put(&o, (uint32_t)s16(os2 + OS2_TYPO_LINE_GAP), 2);
    put(&o, (uint32_t)s16(os2 + OS2_X_HEIGHT), 2);
    put(&o, (uint32_t)s16(os2 + OS2_CAP_HEIGHT), 2);
    put(&o, (uint32_t)glyph_count, 2);
    put(&o, (uint32_t)ranges, 2);
    put(&o, left_classes, 2);
    put(&o, right_classes, 2);
    put(&o, (uint32_t)contour_count, 4);
    put(&o, (uint32_t)point_count, 4);
    write_ranges(&o);
    for (i = 0; i < glyph_count; i++) {
        put(&o, glyphs[i].advance, 2);
        put(&o, glyphs[i].left_class, 2);
        put(&o, glyphs[i].right_class, 2);
        put(&o, glyphs[i].contours, 2);
        put(&o, glyphs[i].first_contour, 4);
    }
    for (i = 0; i < contour_count; i++)
        put(&o, ends[i], 4);
    for (i = 0; i < point_count; i++) {
        put(&o, (uint32_t)points[i].x, 2);
        put(&o, (uint32_t)points[i].y, 2);
    }
    /* Row l, column r: the pair of class l's first member and class r's. */
    for (i = 0; i < left_classes; i++)
        for (j = 0; j < right_classes; j++) {
            size_t a = 0, b = 0;

            while (glyphs[a].left_class != i)
                a++;
            while (glyphs[b].right_class != j)
                b++;
            put(&o, (uint32_t)(i && j ? matrix[a * glyph_count + b] : 0), 2);
        }
    for (i = 0; i < point_count; i++)
        put(&o, points[i].on ? FONT_ON_CURVE : 0, 1);

    f = fopen(argv[2], "wb");
    if (!f || fwrite(o.bytes, 1, o.size, f) != o.size || fclose(f) != 0)
        die("cannot write", argv[2]);
    /* No %zu: msvcrt.dll, the C library TinyCC links on Windows, has
       none. */
    printf("%s: %lu code points, %lu glyphs, %lu contours, %lu points, "
           "%lu kerning pairs in %u x %u classes, %lu bytes\n",
           argv[2], (unsigned long)code_count, (unsigned long)glyph_count,
           (unsigned long)contour_count, (unsigned long)point_count,
           (unsigned long)pairs, left_classes, right_classes,
           (unsigned long)o.size);
    return 0;
}
