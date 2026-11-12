/* Checks the composer, lib/compose.c (docs/spec/display.md D7 rule 2, D8
   and D10 rule 7; look.md V4 and V15), on the host. Every expectation is
   derived here and not by calling the function under test:

   - the mix: gfx_cross_fade against a mix of decoded light computed in
     floating point from the sRGB formulas, so a mix in code space, a
     swapped operand or a reversed weight fails;
   - a material, against that mix and against the console's panel, whose
     pixels it must give wherever a corner does not cut them;
   - a window's corners, against outline_coverage of the whole window's
     rounded rectangle, which display.md D8 gives as a corner's cover:
     every pixel of windows of many sizes, at several positions, for
     radii that are whole and fractional pixels, the console's windows
     and radii among them, must equal it byte for byte;
   - a frame, against a per-pixel statement of D8 written here from the
     spec's text: the wallpaper, then each window, with its own loops,
     its own cover plane from outline_coverage of the window's rounded
     rectangle, and the cover rules; then in bands of TILE_ROWS and of
     other heights, and whole, which must agree bit for bit;
   - the wallpaper alone, through the output, against the kernel's image
     in both pixel formats at the sizes the boots run, in bands of every
     height and over a screen that held other pixels, and the reasons the
     kernel's background is refused as the wallpaper;
   - a window's buffer against its place, which D7 rule 4 says the window
     shows cut or padded with its material: buffers smaller and larger
     than the place, in memory of exactly their size and in a block
     surrounded by pixels no window may show;
   - the output, against the block each pixel's address lies in, for
     framebuffers whose rows start at every alignment, bands that hold
     more or less than a rectangle's blocks, and rectangles off the
     screen;
   - the surfaces' colours over Limb's brightest pixel, which look.md V4
     gives;
   - the pointer, against the arrow's polygon clipped to each pixel in
     floating point, against the pixels V15's table gives, against the
     box the arrow's geometry gives, at the greatest scale a screen gives
     and far past it, and for malformed shapes, which must draw nothing;
   - the pointer as a frame draws it (D19 rule 7): a frame of any
     rectangle and band height, with the hotspot anywhere on or off the
     screen, against the whole frame composed with the arrow drawn over it
     as one band; a pointer with no place, and one whose image is refused,
     drawn nowhere; and walks of the hotspot, of one to three moves between
     frames, in which frames over only what the moves damage must leave the
     screen as the whole frame composed with the arrow where the pointer is,
     and each move must record the boxes the arrow's geometry and the
     output's blocks give;
   - what a frame composes for the pointer's moves (D19's note on the
     pointer's two boxes), against the rectangles that note gives for the
     boxes the geometry gives, with the pixels they share found by trying
     every pixel: a move longer than the image composes its old box and its
     new and leaves every pixel between them as it was, at the pixels D9's
     row counts, a move whose boxes share a pixel composes their join, and
     moves between two frames compose nothing at the places between;
   - the pixels a frame says it composed, which the compositor's budget
     lines count (D19 rule 8), against the pixels it wrote, counted on the
     screen, for the frames of rectangles and of the pointer's moves
     above; and the pixels of the rule's example line, read from
     docs/spec/display.md, against a move across the screen at the scale
     of the 2880 x 1800 screen that line is of.

   The pointer's outlines, the surfaces' tints and amounts, the edge's
   opacity and TILE_ROWS are read from interfaces/display.desc, which
   owns them. The corners' coverage is not checked against areas worked
   out apart from lib/outline.c: tools/check-outline.c does that for the
   coverage itself, and this checks that a corner is that coverage.

   Prints each failure and a count, and exits nonzero if any check fails.

   cc -std=c11 -Wall -Wextra -pedantic -I include -I lib -I gfx \
     -I console -o check-compose tools/check-compose.c lib/compose.c \
     lib/outline.c console/panel.c console/text.c gfx/gfx.c gfx/font.c \
     gfx/raster.c gfx/limb.c -lm && \
     ./check-compose interfaces/display.desc docs/spec/display.md */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compose.h"
#include "limb.h"
#include "panel.h"
#include "window.h"

#define CHANNEL_BITS 8
#define CHANNEL_MASK 0xFFu
#define CODES 256
#define FAILURES_SHOWN 12

/* gfx_cross_fade decodes to integer light and truncates, so it can differ
   from a mix computed in floating point by one code where the exact
   result lies at a rounding boundary; a mix in code space, or with an
   operand swapped, differs by many. */
#define MIX_TOLERANCE 1

/* The pointer's edge is flattened to lines within a sixteenth of a pixel
   of its curves (outline.h), so a pixel it crosses has an area up to
   sqrt(2) / 16 of a pixel from the curve's, which is this many of the 255
   coverages, rounded up. */
#define EDGE_COVER_TOLERANCE 23

/* The coverage's own rounding to the nearest of 255, and the cut of its
   lines to 256ths of a pixel, move a straight edge's coverage by one. */
#define BODY_COVER_TOLERANCE 1

/* look.md V15's table: the pixels the arrow covers at scales 1, 2 and
   2.25, in quarters. */
static const struct {
    UINT32 quarters;
    INT64 width, height;
} arrow_table[] = {{4, 16, 23}, {8, 32, 46}, {9, 36, 52}};
#define ARROW_SCALES (sizeof arrow_table / sizeof arrow_table[0])

/* The sizes the boots run, and a width whose rows do not fill whole
   blocks. */
static const struct {
    INT64 width, height;
} sizes[] = {{1280, 800}, {2560, 1600}, {2880, 1800}, {1366, 768}};
#define SIZES (sizeof sizes / sizeof sizes[0])

static const UINT32 formats[] = {PIXEL_RGB, PIXEL_BGR};
#define FORMATS (sizeof formats / sizeof formats[0])

/* The bytes in a block of the output (display.md D8): the unit the
   processor writes a write-combining framebuffer in, which the composer's
   own constant is checked against, not used for. */
#define OUTPUT_BLOCK_BYTES 64

static unsigned failures;
static unsigned long long checks;

/* The memory outline_coverage and outline_fill work in, which is large. */
static OUTLINE work;

static void check(int passed, const char *what, long long a, long long b)
{
    checks++;
    if (!passed && failures++ < FAILURES_SHOWN)
        printf("failed: %s (%lld, %lld)\n", what, a, b);
}

/* ------------------------------------------------------------------------
   The description */

#define DESC_BYTES_MAX (1 << 20)
#define POINTS_MAX 256
#define CONTOURS_MAX 16

/* A shape read from the description, in the form the generator writes. */
typedef struct {
    INT32 points[2 * POINTS_MAX];
    UINT8 flags[POINTS_MAX];
    UINT32 ends[CONTOURS_MAX];
    UINT32 point_count, contours;
} SHAPE_STORE;

static char *desc_text;

static int read_description(const char *path)
{
    FILE *file = fopen(path, "rb");
    size_t size;

    if (!file)
        return 0;
    desc_text = malloc(DESC_BYTES_MAX);
    size = desc_text ? fread(desc_text, 1, DESC_BYTES_MAX - 1, file) : 0;
    fclose(file);
    if (size == 0)
        return 0;
    desc_text[size] = 0;
    return 1;
}

/* The most bytes of the specification the check reads, and where rule
   8's example of the pointer's budget line starts in it, the key of the
   line's pixels, and the mark that ends an example. */
#define SPEC_BYTES_MAX (1 << 20)
#define EXAMPLE_START "`nova-compositor: budget change=pointer"
#define EXAMPLE_PIXELS_KEY "pixels="
#define EXAMPLE_END '`'

/* The scale of the 2880 x 1800 screen the specification's example lines
   are of, 2.25, in quarters (display.md D9). */
#define EXAMPLE_QUARTERS 9

/* The pixels rule 8's example of the pointer's budget line gives, as the
   specification at the path read_example was given holds them; 0 until it
   is read. */
static INT64 example_pixels;

/* Read rule 8's example from the specification at path: the number after
   the pixels key of the first pointer's budget line in it. Returns 0 if
   the file has no such line or its pixels are not a number above 0. */
static int read_example(const char *path)
{
    FILE *file = fopen(path, "rb");
    char *text = file ? malloc(SPEC_BYTES_MAX) : 0;
    size_t size = text ? fread(text, 1, SPEC_BYTES_MAX - 1, file) : 0;
    const char *line, *end, *key;

    if (file)
        fclose(file);
    if (text)
        text[size] = 0;
    line = text ? strstr(text, EXAMPLE_START) : 0;
    end = line ? strchr(line + 1, EXAMPLE_END) : 0;
    key = line ? strstr(line, EXAMPLE_PIXELS_KEY) : 0;
    if (end && key && key < end)
        example_pixels = strtoll(key + strlen(EXAMPLE_PIXELS_KEY), 0, 10);
    free(text);
    return example_pixels > 0;
}

/* The line after the header of setting name, or 0. */
static const char *setting_body(const char *name)
{
    char header[128];
    const char *at;

    snprintf(header, sizeof header, "\nsetting %s ", name);
    at = strstr(desc_text, header);
    at = at ? strchr(at + 1, '\n') : 0;
    return at ? at + 1 : 0;
}

/* Whether the line at body starts with word, then a space. */
static int starts_with(const char *line, const char *word)
{
    size_t length = strlen(word);

    while (*line == ' ')
        line++;
    return strncmp(line, word, length) == 0 && line[length] == ' ';
}

/* The text after the keyword on the first line of body, up to the
   setting's `end`, that starts with keyword, or 0. */
static const char *body_line(const char *body, const char *keyword)
{
    const char *line = body;

    while (line && strncmp(line, "end", 3) != 0) {
        if (starts_with(line, keyword)) {
            while (*line == ' ')
                line++;
            return line + strlen(keyword) + 1;
        }
        line = strchr(line, '\n');
        line = line ? line + 1 : 0;
    }
    return 0;
}

static int read_const(const char *name, long *value)
{
    char header[128];
    const char *at;

    snprintf(header, sizeof header, "\nconst %s ", name);
    at = strstr(desc_text, header);
    if (!at)
        return 0;
    *value = strtol(at + strlen(header), 0, 10);
    return 1;
}

/* The tint and amount of `setting name material`. */
static int read_material(const char *name, UINT32 *tint, UINT32 *amount)
{
    const char *line = body_line(setting_body(name), "default");
    char *end;

    if (!line)
        return 0;
    *tint = (UINT32)strtoul(line, &end, 16);
    *amount = (UINT32)strtoul(end, 0, 10);
    return 1;
}

static int read_default(const char *name, UINT32 *value, int base)
{
    const char *line = body_line(setting_body(name), "default");

    if (!line)
        return 0;
    *value = (UINT32)strtoul(line, 0, base);
    return 1;
}

/* The contour lines of `setting name outline`: each point as `x,y`, after
   `c` where it is a cubic's control point and after `q` where it is a
   quadratic's (interface.md P12). */
static int read_shape(const char *name, SHAPE_STORE *shape)
{
    const char *line = setting_body(name);

    memset(shape, 0, sizeof *shape);
    while (line && strncmp(line, "end", 3) != 0) {
        if (starts_with(line, "contour")) {
            const char *at = strstr(line, "contour") + strlen("contour");

            while (*at && *at != '\n') {
                UINT8 flag = COMPOSE_POINT_ON_CURVE;
                char *end;
                long x, y;

                while (*at == ' ')
                    at++;
                if (*at == 'c' || *at == 'q') {
                    flag = *at == 'c' ? COMPOSE_POINT_CUBIC
                                      : COMPOSE_POINT_QUADRATIC;
                    at++;
                    while (*at == ' ')
                        at++;
                }
                if (*at == '\n' || !*at)
                    break;
                x = strtol(at, &end, 10);
                if (*end != ',' || shape->point_count >= POINTS_MAX)
                    return 0;
                y = strtol(end + 1, &end, 10);
                shape->points[2 * shape->point_count] = (INT32)x;
                shape->points[2 * shape->point_count + 1] = (INT32)y;
                shape->flags[shape->point_count++] = flag;
                at = end;
            }
            if (shape->contours >= CONTOURS_MAX)
                return 0;
            shape->ends[shape->contours++] = shape->point_count;
        }
        line = strchr(line, '\n');
        line = line ? line + 1 : 0;
    }
    return shape->contours != 0;
}

static COMPOSE_SHAPE view(const SHAPE_STORE *store)
{
    COMPOSE_SHAPE shape;

    shape.points = store->points;
    shape.flags = store->flags;
    shape.ends = store->ends;
    shape.point_count = store->point_count;
    shape.contours = store->contours;
    return shape;
}

/* ------------------------------------------------------------------------
   Light, computed from the sRGB formulas in floating point (IEC 61966-2-1,
   as gfx/raster.c cites them) */

static double light[CODES];

static void make_light(void)
{
    int code;

    for (code = 0; code < CODES; code++) {
        double c = code / 255.0;

        light[code] = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
    }
}

static int code_of(double value)
{
    double c = value <= 0.0031308 ? 12.92 * value
                                  : 1.055 * pow(value, 1.0 / 2.4) - 0.055;
    int code = (int)floor(c * 255.0 + 0.5);

    return code < 0 ? 0 : code > 255 ? 255 : code;
}

/* The code of the light `from` and `to` mix to, `to` by weight / total. */
static int mixed(int from, int to, long weight, long total)
{
    double w = (double)weight / (double)total;

    return code_of((1.0 - w) * light[from] + w * light[to]);
}

static int near_code(int a, int b, int tolerance)
{
    return abs(a - b) <= tolerance;
}

static UINT32 channel(UINT32 pixel, int k)
{
    return pixel >> ((2 - k) * CHANNEL_BITS) & CHANNEL_MASK;
}

/* gfx_cross_fade over every channel of two 0x00RRGGBB pixels. */
static UINT32 fade(UINT32 from, UINT32 to, INT64 weight, INT64 total)
{
    UINT32 out = 0;
    int k;

    for (k = 0; k < 3; k++)
        out |= (UINT32)gfx_cross_fade(channel(from, k), channel(to, k), weight,
                                      total)
               << ((2 - k) * CHANNEL_BITS);
    return out;
}

static UINT64 random_state = 0x9e3779b97f4a7c15ULL;

/* xorshift64 (Marsaglia, "Xorshift RNGs", 2003), seeded as check-masks.c
   seeds it, so every run checks the same values. */
static UINT64 next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

static INT64 random_below(INT64 n)
{
    return (INT64)(next_random() % (UINT64)n);
}

/* ------------------------------------------------------------------------
   The mix and the materials */

static void check_mix(void)
{
    long weights[] = {1, 2, 3, 17, 64, 127, 128, 129, 200, 253, 254};
    int from, to;
    size_t i;
    int worst = 0;

    for (from = 0; from < CODES; from++)
        for (to = 0; to < CODES; to++) {
            /* The composer copies a cover of 255 and does not mix a
               cover of 0, which is right only if the mix leaves them
               as they are. */
            check(gfx_cross_fade(from, to, 0, 255) == from,
                  "a mix of weight 0 keeps its from", from, to);
            check(gfx_cross_fade(from, to, 255, 255) == to,
                  "a mix of full weight is its to", from, to);
            for (i = 0; i < sizeof weights / sizeof weights[0]; i++) {
                int got = (int)gfx_cross_fade(from, to, weights[i], 255);
                int want = mixed(from, to, weights[i], 255);

                worst = abs(got - want) > worst ? abs(got - want) : worst;
                check(near_code(got, want, MIX_TOLERANCE),
                      "a mix is the mix of decoded light", from * 256 + to,
                      weights[i]);
            }
        }
    printf("mix: worst distance from floating point %d code(s)\n", worst);
}

static void check_materials(void)
{
    static const char *const names[] = {"theme.material.window",
                                        "theme.material.raised",
                                        "theme.material.panel"};
    static COMPOSE_MATERIAL material, kept;
    size_t i;

    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        UINT32 tint, amount;
        int k, code;

        if (!read_material(names[i], &tint, &amount)) {
            check(0, "the description has the material", (long long)i, 0);
            continue;
        }
        check(compose_material(&material, tint, amount) == 1,
              "a material is made", tint, amount);
        for (k = 0; k < 3; k++)
            for (code = 0; code < CODES; code++) {
                int tint_code = (int)channel(tint, k);

                check(material.code[k][code] ==
                          gfx_cross_fade(code, tint_code, amount, 100),
                      "a material mixes as the console's panel does",
                      k * 256 + code, tint);
                check(near_code(material.code[k][code],
                                mixed(code, tint_code, amount, 100),
                                MIX_TOLERANCE),
                      "a material is a mix of decoded light", k * 256 + code,
                      tint);
            }
    }
    memset(&kept, 0x5A, sizeof kept);
    material = kept;
    check(compose_material(&material, 0x112233, 101) == 0 &&
              memcmp(&material, &kept, sizeof material) == 0,
          "an amount past 100 is refused and changes nothing", 101, 0);
    check(compose_material(&material, 0x112233, 0) == 1 &&
              material.code[0][77] == 77 && material.code[2][200] == 200,
          "an amount of 0 leaves every code", 0, 0);
    check(compose_material(&material, 0x112233, 100) == 1 &&
              material.code[0][77] == 0x11 && material.code[1][3] == 0x22 &&
              material.code[2][255] == 0x33,
          "an amount of 100 is the tint", 100, 0);
}

/* ------------------------------------------------------------------------
   A scene: Limb, and windows with random buffers, places and corners */

#define SCENE_WIDTH 160
#define SCENE_HEIGHT 100
#define SCENE_WINDOWS 8
#define WHOLLY_ON_SCREEN 4
#define WINDOW_WIDTH_MIN 20
#define WINDOW_WIDTH_SPREAD 100
#define WINDOW_HEIGHT_MIN 20
#define WINDOW_HEIGHT_SPREAD 60
#define STRIDE_SPREAD 6
#define PADDING 3
#define SENTINEL 0xC3A5F00Fu

/* The scene's windows' radii, in 256ths of a pixel, by window: a radius
   of 0, which makes square corners; 5.5, 8.25, 10 and 0.625 pixels; and
   NO_CORNERS, which gives the window none to read. The least window is 20
   pixels, twice the greatest reach, 10. */
#define NO_CORNERS (-1)
static const INT64 scene_radii[SCENE_WINDOWS] = {
    0, 1408, NO_CORNERS, 2112, 0, 2560, NO_CORNERS, 160};

/* How much wider and taller than its place each scene window's buffer is:
   equal; narrower; shorter; both; larger, which the place cuts; and
   mixtures. Every buffer is made in memory of its own size. */
static const struct {
    INT64 across, down;
} scene_buffers[SCENE_WINDOWS] = {{0, 0},   {-7, 0}, {0, -9}, {-13, -5},
                                  {6, 4}, {0, 0},  {-3, 5}, {9, -2}};

typedef struct {
    INT64 left, top, width, height, stride, radius;
    INT64 columns, rows; /* the pixels the buffer holds, which the place may
                            be larger or smaller than */
    UINT32 *buffer;
    UINT32 tint, amount;
    COMPOSE_CORNERS corners;
    UINT8 *shape; /* the cover of the shape at each pixel of the window */
    COMPOSE_MATERIAL material;
} TEST_WINDOW;

typedef struct {
    UINT32 *canonical; /* Limb's codes, 0x00RRGGBB */
    TEST_WINDOW windows[SCENE_WINDOWS];
} SCENE;

static void limb_codes(INT64 width, INT64 height, UINT32 *out)
{
    LIMB limb;
    INT64 x, y;

    if (!limb_init(&limb, width, height)) {
        memset(out, 0, (size_t)(width * height) * sizeof *out);
        check(0, "limb takes the size", width, height);
        return;
    }
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++)
            out[y * width + x] = limb_pixel(&limb, x, y);
}

/* An image of the codes as the kernel's holds it in format, each pixel's
   unused byte set. */
static UINT32 *kernel_image(const UINT32 *codes, INT64 width, INT64 height,
                            UINT32 format)
{
    SCREEN screen = {0, width, height, width, format};
    UINT32 *image = malloc((size_t)(width * height) * sizeof *image);
    INT64 i;

    for (i = 0; image && i < width * height; i++)
        image[i] = gfx_pixel(&screen, channel(codes[i], 0),
                             channel(codes[i], 1), channel(codes[i], 2)) |
                   0xA5000000u;
    return image;
}

/* A cover that is mostly 0 or 255, as a window's pixels are, and
   sometimes between. */
static UINT32 random_cover(void)
{
    INT64 pick = random_below(10);

    return pick < 4 ? 255 : pick < 7 ? 0 : (UINT32)(1 + random_below(254));
}

/* The window's shape as D8 gives it: the coverage of its rounded
   rectangle, from the whole window's own plane, or whole everywhere for
   a window with no corners. */
static void make_shape(TEST_WINDOW *w)
{
    w->shape = malloc((size_t)(w->width * w->height));
    memset(w->shape, 255, (size_t)(w->width * w->height));
    if (w->radius == NO_CORNERS)
        return;
    outline_start(&work);
    outline_rounded_rect(&work, 0, 0, w->width * OUTLINE_ONE,
                         w->height * OUTLINE_ONE, w->radius);
    check(outline_coverage(&work, w->shape, w->width, w->height) == 1,
          "a window's rounded rectangle gives its coverage", w->width,
          w->height);
}

static void make_scene(SCENE *scene)
{
    int i;

    scene->canonical =
        malloc((size_t)(SCENE_WIDTH * SCENE_HEIGHT) * sizeof(UINT32));
    limb_codes(SCENE_WIDTH, SCENE_HEIGHT, scene->canonical);
    for (i = 0; i < SCENE_WINDOWS; i++) {
        TEST_WINDOW *w = &scene->windows[i];
        INT64 x, y;

        w->width = WINDOW_WIDTH_MIN + random_below(WINDOW_WIDTH_SPREAD);
        w->height = WINDOW_HEIGHT_MIN + random_below(WINDOW_HEIGHT_SPREAD);
        if (i < WHOLLY_ON_SCREEN) {
            w->left = random_below(SCENE_WIDTH - w->width + 1);
            w->top = random_below(SCENE_HEIGHT - w->height + 1);
        } else {
            w->left = random_below(SCENE_WIDTH + 40) - 30;
            w->top = random_below(SCENE_HEIGHT + 20) - 20;
        }
        w->columns = w->width + scene_buffers[i].across;
        w->rows = w->height + scene_buffers[i].down;
        w->stride = w->columns + random_below(STRIDE_SPREAD);
        w->radius = scene_radii[i];
        w->tint = (UINT32)random_below(1 << 24);
        w->amount = (UINT32)random_below(COMPOSE_PERCENT + 1);
        w->buffer = malloc((size_t)(w->stride * w->rows) * sizeof(UINT32));
        for (y = 0; y < w->rows; y++)
            for (x = 0; x < w->stride; x++)
                w->buffer[y * w->stride + x] = random_cover()
                                                   << COMPOSE_COVER_SHIFT |
                                               (UINT32)random_below(1 << 24);
        if (w->radius != NO_CORNERS)
            check(compose_corners(&work, &w->corners, w->radius) == 1,
                  "a scene's corners are made", i, w->radius);
        make_shape(w);
        compose_material(&w->material, w->tint, w->amount);
    }
}

static void free_scene(SCENE *scene)
{
    int i;

    for (i = 0; i < SCENE_WINDOWS; i++) {
        free(scene->windows[i].buffer);
        free(scene->windows[i].shape);
    }
    free(scene->canonical);
}

/* D8 for one window's pixel, written from its text: the window's pixel
   is its colour mixed over its material, which is the wallpaper at that
   pixel, not what is composed beneath, by its cover (255 copies, 0 is the
   material); past the buffer the pixel is 0, which is the material
   (D7 rule 4); at a corner the pixel so made is mixed over what is
   composed beneath, out, by the shape's cover. */
static UINT32 compose_over(const TEST_WINDOW *w, UINT32 wallpaper, UINT32 out,
                           INT64 x, INT64 y)
{
    UINT32 pixel, colour, cover, material, window, k;
    INT64 dx = x - w->left, dy = y - w->top;
    int c;

    if (dx < 0 || dy < 0 || dx >= w->width || dy >= w->height)
        return out;
    pixel = dx < w->columns && dy < w->rows ? w->buffer[dy * w->stride + dx]
                                            : 0;
    colour = pixel & 0xFFFFFFu;
    cover = pixel >> COMPOSE_COVER_SHIFT;
    material = 0;
    for (c = 0; c < 3; c++)
        material |= (UINT32)gfx_cross_fade(channel(wallpaper, c),
                                           channel(w->tint, c), w->amount, 100)
                    << ((2 - c) * CHANNEL_BITS);
    window = cover == 255 ? colour
             : cover == 0 ? material
                          : fade(material, colour, cover, 255);
    k = w->shape[dy * w->width + dx];
    return k == 255 ? window : k == 0 ? out : fade(out, window, k, 255);
}

/* D8 for one pixel of the scene: the wallpaper, then each window from the
   lowest. */
static UINT32 reference(const SCENE *scene, INT64 x, INT64 y)
{
    UINT32 wallpaper = scene->canonical[y * SCENE_WIDTH + x];
    UINT32 out = wallpaper;
    int i;

    for (i = 0; i < SCENE_WINDOWS; i++)
        out = compose_over(&scene->windows[i], wallpaper, out, x, y);
    return out;
}

static void windows_of(const SCENE *scene, COMPOSE_WINDOW *windows)
{
    int i;

    for (i = 0; i < SCENE_WINDOWS; i++) {
        const TEST_WINDOW *w = &scene->windows[i];

        windows[i].pixels = w->buffer;
        windows[i].stride = w->stride;
        windows[i].columns = w->columns;
        windows[i].rows = w->rows;
        windows[i].place.left = w->left;
        windows[i].place.top = w->top;
        windows[i].place.right = w->left + w->width;
        windows[i].place.bottom = w->top + w->height;
        windows[i].material = &w->material;
        windows[i].corners = w->radius == NO_CORNERS ? 0 : &w->corners;
    }
}

/* Compose rect into a band with padding after each row, and return the
   pixels row by row without it; check the padding is untouched. */
static UINT32 *compose_rect(const COMPOSE_WALLPAPER *wallpaper,
                            const COMPOSE_WINDOW *windows, const RECT *rect)
{
    INT64 width = rect->right - rect->left, height = rect->bottom - rect->top;
    INT64 stride = width + PADDING, x, y;
    UINT32 *memory = malloc((size_t)(stride * height) * sizeof(UINT32));
    UINT32 *out = malloc((size_t)(width * height) * sizeof(UINT32));
    COMPOSE_BAND band;

    for (x = 0; x < stride * height; x++)
        memory[x] = SENTINEL;
    band.pixels = memory;
    band.stride = stride;
    band.rect = *rect;
    compose_band(&band, wallpaper, windows, SCENE_WINDOWS);
    for (y = 0; y < height; y++) {
        for (x = 0; x < PADDING; x++)
            check(memory[y * stride + width + x] == SENTINEL,
                  "a band's padding is left alone", y, x);
        memcpy(out + y * width, memory + y * stride,
               (size_t)width * sizeof(UINT32));
    }
    free(memory);
    return out;
}

/* The pixel the screen holds before a frame, whose unused byte is not 0,
   so that a pixel the frame does not write shows. */
#define SCREEN_BEFORE 0xA5A5A5A5u

/* The pixels in a block of the screen's memory, which starts at a multiple
   of OUTPUT_BLOCK_BYTES. */
#define BLOCK_PIXELS (OUTPUT_BLOCK_BYTES / sizeof(UINT32))

/* The rectangles and band heights the frames are composed in: the whole
   screen, an inner rectangle, a pixel at each end, a row, a column, and
   one that spans most of it. */
static const RECT frame_rects[] = {{0, 0, SCENE_WIDTH, SCENE_HEIGHT},
                                   {13, 9, 131, 71},
                                   {0, 0, 1, 1},
                                   {SCENE_WIDTH - 1, SCENE_HEIGHT - 1,
                                    SCENE_WIDTH, SCENE_HEIGHT},
                                   {20, 40, 100, 41},
                                   {40, 0, 41, SCENE_HEIGHT},
                                   {17, 3, 70, 99}};
#define FRAME_RECTS (sizeof frame_rects / sizeof frame_rects[0])
static const INT64 frame_rows[] = {1, 7, 16, SCENE_HEIGHT + 1};
#define FRAME_ROWS (sizeof frame_rows / sizeof frame_rows[0])

/* compose_frame of rect, over a screen that held SCREEN_BEFORE, in bands
   of rows rows, drawing cursor if it is not 0: counts the pixels that are
   not what a frame is. In each row of rect, every pixel of every block
   that holds a pixel of rect is the one of all, the whole frame as
   0x00RRGGBB, in the screen's order with its unused byte 0 (wrong counts
   those that are not); every other pixel, in the other rows too, is
   untouched (stray counts those that are not). The frame says it composed
   as many pixels as those blocks hold, which every row of this screen
   starts one of (miscount is how many more or fewer it says). */
static void frame_difference(const SCREEN *screen, UINT32 *screen_memory,
                             const COMPOSE_WALLPAPER *wallpaper,
                             const COMPOSE_WINDOW *windows, const RECT *rect,
                             INT64 rows, const COMPOSE_CURSOR *cursor,
                             const UINT32 *all, INT64 *wrong, INT64 *stray,
                             INT64 *miscount)
{
    INT64 band_pixels = (INT64)SCENE_WIDTH * (SCENE_HEIGHT + 1);
    UINT32 *band = malloc((size_t)band_pixels * sizeof(UINT32));
    INT64 x, y, in_blocks = 0;
    UINT64 composed;

    *wrong = 0;
    *stray = 0;
    for (x = 0; x < SCENE_WIDTH * SCENE_HEIGHT; x++)
        screen_memory[x] = SCREEN_BEFORE;
    composed = compose_frame(screen, wallpaper, windows, SCENE_WINDOWS, rect,
                             band, rows, cursor);
    for (y = 0; y < SCENE_HEIGHT; y++) {
        int in_rows = y >= rect->top && y < rect->bottom;
        INT64 first = (y * SCENE_WIDTH + rect->left) / (INT64)BLOCK_PIXELS;
        INT64 last = (y * SCENE_WIDTH + rect->right - 1) / (INT64)BLOCK_PIXELS;

        for (x = 0; x < SCENE_WIDTH; x++) {
            INT64 block = (y * SCENE_WIDTH + x) / (INT64)BLOCK_PIXELS;
            UINT32 got = screen_memory[y * SCENE_WIDTH + x];
            UINT32 colour = all[y * SCENE_WIDTH + x];
            UINT32 wanted = gfx_pixel(screen, colour >> 16 & CHANNEL_MASK,
                                      colour >> 8 & CHANNEL_MASK,
                                      colour & CHANNEL_MASK);

            if (in_rows && block >= first && block <= last) {
                *wrong += got != wanted;
                in_blocks++;
            } else {
                *stray += got != SCREEN_BEFORE;
            }
        }
    }
    *miscount = (INT64)composed - in_blocks;
    free(band);
}

static void check_frame_rect(const SCREEN *screen, UINT32 *screen_memory,
                             const COMPOSE_WALLPAPER *wallpaper,
                             const COMPOSE_WINDOW *windows, const RECT *rect,
                             INT64 rows, const UINT32 *all)
{
    INT64 wrong, stray, miscount;

    frame_difference(screen, screen_memory, wallpaper, windows, rect, rows, 0,
                     all, &wrong, &stray, &miscount);
    check(wrong == 0,
          "a frame of a rectangle writes the frame's pixels in every block "
          "of its rows that holds one of the rectangle's",
          rect->left * 1000 + rect->top, wrong);
    check(stray == 0,
          "a frame of a rectangle writes nothing else",
          rows, stray);
    check(miscount == 0,
          "a frame of a rectangle says it composed the pixels of those "
          "blocks",
          rect->left * 1000 + rect->top, miscount);
}

static void check_frame_rects(const SCENE *scene)
{
    static const RECT nothing[] = {{10, 10, 10, 40}, {10, 40, 30, 40},
                                   {30, 10, 10, 40}, {10, 40, 30, 10}};
    COMPOSE_WINDOW windows[SCENE_WINDOWS];
    size_t f, r, h;

    windows_of(scene, windows);
    for (f = 0; f < FORMATS; f++) {
        UINT32 *image = kernel_image(scene->canonical, SCENE_WIDTH,
                                     SCENE_HEIGHT, formats[f]);
        COMPOSE_WALLPAPER wallpaper = {image, SCENE_WIDTH, formats[f]};
        RECT whole = {0, 0, SCENE_WIDTH, SCENE_HEIGHT};
        UINT32 *all = compose_rect(&wallpaper, windows, &whole);
        UINT32 *screen_memory = aligned_alloc(
            OUTPUT_BLOCK_BYTES, (size_t)SCENE_WIDTH * SCENE_HEIGHT *
                                    sizeof(UINT32));
        UINT32 *band = malloc((size_t)SCENE_WIDTH * (SCENE_HEIGHT + 1) *
                              sizeof(UINT32));
        SCREEN screen = {screen_memory, SCENE_WIDTH, SCENE_HEIGHT, SCENE_WIDTH,
                         formats[f]};
        INT64 i, written;
        UINT64 composed;

        for (r = 0; r < FRAME_RECTS; r++)
            for (h = 0; h < FRAME_ROWS; h++)
                check_frame_rect(&screen, screen_memory, &wallpaper, windows,
                                 &frame_rects[r], frame_rows[h], all);
        for (r = 0; r < sizeof nothing / sizeof nothing[0]; r++) {
            for (i = 0; i < SCENE_WIDTH * SCENE_HEIGHT; i++)
                screen_memory[i] = SCREEN_BEFORE;
            composed = compose_frame(&screen, &wallpaper, windows,
                                     SCENE_WINDOWS, &nothing[r], band, 16, 0);
            for (i = 0, written = 0; i < SCENE_WIDTH * SCENE_HEIGHT; i++)
                written += screen_memory[i] != SCREEN_BEFORE;
            check(written == 0 && composed == 0,
                  "a frame of a rectangle with no pixel writes nothing and "
                  "says it composed none",
                  (long long)r, written);
        }
        for (i = 0; i < SCENE_WIDTH * SCENE_HEIGHT; i++)
            screen_memory[i] = SCREEN_BEFORE;
        composed = compose_frame(&screen, &wallpaper, windows, SCENE_WINDOWS,
                                 &whole, band, 0, 0);
        check(screen_memory[0] == SCREEN_BEFORE && composed == 0,
              "a frame in bands of no rows writes nothing and says it "
              "composed none",
              0, 0);
        free(band);
        free(screen_memory);
        free(all);
        free(image);
    }
}

static void check_frames(const SCENE *scene)
{
    static const INT64 heights[] = {1, 7, 16, 33, SCENE_HEIGHT};
    COMPOSE_WINDOW windows[SCENE_WINDOWS];
    long tile_rows = 0;
    size_t f, h;

    windows_of(scene, windows);
    if (!read_const("TILE_ROWS", &tile_rows) || tile_rows < 1)
        check(0, "the description has TILE_ROWS", tile_rows, 0);
    for (f = 0; f < FORMATS; f++) {
        UINT32 *image = kernel_image(scene->canonical, SCENE_WIDTH,
                                     SCENE_HEIGHT, formats[f]);
        COMPOSE_WALLPAPER wallpaper = {image, SCENE_WIDTH, formats[f]};
        RECT whole = {0, 0, SCENE_WIDTH, SCENE_HEIGHT};
        UINT32 *all = compose_rect(&wallpaper, windows, &whole);
        INT64 x, y;
        INT64 bad = 0;

        for (y = 0; y < SCENE_HEIGHT; y++)
            for (x = 0; x < SCENE_WIDTH; x++)
                bad += all[y * SCENE_WIDTH + x] != reference(scene, x, y);
        check(bad == 0, "a frame is D8's pixels", bad, (long long)formats[f]);

        /* Bands of every height, TILE_ROWS and others, make the frame
           the whole composes. */
        for (h = 0; h <= sizeof heights / sizeof heights[0]; h++) {
            INT64 rows =
                h < sizeof heights / sizeof heights[0] ? heights[h] : tile_rows;
            INT64 top, wrong = 0;

            for (top = 0; top < SCENE_HEIGHT; top += rows) {
                RECT band = {0, top, SCENE_WIDTH,
                             top + rows < SCENE_HEIGHT ? top + rows
                                                       : SCENE_HEIGHT};
                UINT32 *part = compose_rect(&wallpaper, windows, &band);

                wrong += memcmp(part, all + top * SCENE_WIDTH,
                                (size_t)((band.bottom - top) * SCENE_WIDTH) *
                                    sizeof(UINT32)) != 0;
                free(part);
            }
            check(wrong == 0, "composing in bands equals composing whole", rows,
                  wrong);
        }

        /* A rectangle that touches no edge of the frame, and one that
           lies partly outside every window. */
        {
            RECT inner = {13, 9, 131, 71};
            UINT32 *part = compose_rect(&wallpaper, windows, &inner);
            INT64 wrong = 0;

            for (y = inner.top; y < inner.bottom; y++)
                for (x = inner.left; x < inner.right; x++)
                    wrong += part[(y - inner.top) * (inner.right - inner.left) +
                                  (x - inner.left)] != all[y * SCENE_WIDTH + x];
            check(wrong == 0, "a damage rectangle composes as the frame does",
                  wrong, 0);
            free(part);
        }
        free(all);
        free(image);
    }
}

/* A cover of 255 is copied, 0 is the material's lookups, and between is a
   mix of decoded light, whatever the wallpaper and the colour. */
static void check_covers(void)
{
    static const UINT32 covers[] = {0, 1, 2, 63, 127, 128, 200, 253, 254, 255};
    COMPOSE_MATERIAL material;
    UINT32 tint = 0x2A5B94, amount = 77;
    INT64 side = 16, x, y;
    UINT32 *wall = malloc((size_t)(side * side) * sizeof(UINT32));
    UINT32 *pixels = malloc((size_t)(side * side) * sizeof(UINT32));
    UINT32 *out = malloc((size_t)(side * side) * sizeof(UINT32));
    COMPOSE_WALLPAPER wallpaper = {wall, side, PIXEL_BGR};
    COMPOSE_WINDOW window;
    COMPOSE_BAND band = {out, side, {0, 0, side, side}};
    size_t c;

    compose_material(&material, tint, amount);
    window.pixels = pixels;
    window.stride = side;
    window.columns = side;
    window.rows = side;
    window.place = band.rect;
    window.material = &material;
    window.corners = 0;
    for (c = 0; c < sizeof covers / sizeof covers[0]; c++) {
        INT64 wrong_copy = 0, wrong_material = 0, wrong_mix = 0;

        for (y = 0; y < side; y++)
            for (x = 0; x < side; x++) {
                wall[y * side + x] = (UINT32)random_below(1 << 24);
                pixels[y * side + x] = covers[c] << COMPOSE_COVER_SHIFT |
                                       (UINT32)random_below(1 << 24);
            }
        compose_band(&band, &wallpaper, &window, 1);
        for (y = 0; y < side; y++)
            for (x = 0; x < side; x++) {
                UINT32 under = wall[y * side + x] & 0xFFFFFFu;
                UINT32 colour = pixels[y * side + x] & 0xFFFFFFu;
                UINT32 got = out[y * side + x];
                int k;

                if (covers[c] == 255) {
                    wrong_copy += got != colour;
                    continue;
                }
                for (k = 0; k < 3; k++) {
                    int behind = mixed((int)channel(under, k),
                                       (int)channel(tint, k), amount, 100);

                    if (covers[c] == 0)
                        wrong_material +=
                            channel(got, k) !=
                                material.code[k][channel(under, k)] ||
                            !near_code((int)channel(got, k), behind,
                                       MIX_TOLERANCE);
                    else
                        wrong_mix +=
                            !near_code((int)channel(got, k),
                                       mixed(behind, (int)channel(colour, k),
                                             covers[c], 255),
                                       2 * MIX_TOLERANCE);
                }
            }
        check(wrong_copy == 0, "a cover of 255 is copied", covers[c],
              wrong_copy);
        check(wrong_material == 0, "a cover of 0 is the material's lookups",
              covers[c], wrong_material);
        check(wrong_mix == 0, "a cover between mixes in linear light",
              covers[c], wrong_mix);
    }
    free(wall);
    free(pixels);
    free(out);
}

/* ------------------------------------------------------------------------
   A buffer that is not its place's size (display.md D7 rule 4) */

#define PAD_WALL_WIDTH 64
#define PAD_WALL_HEIGHT 48
#define PAD_LEFT 7
#define PAD_TOP 5
#define PAD_WIDTH 36
#define PAD_HEIGHT 26
#define PAD_RADIUS 1664 /* 6.5 pixels */
#define PAD_STRIDE_EXTRA 4
#define PAD_ROWS_AFTER 2
#define PAD_TINT 0x2A5B94
#define PAD_AMOUNT 77

/* A pixel no window may show: a cover of 255 over a colour that is no
   material's, so a composer that reads past the buffer shows it. */
#define POISON 0xFFFF00FFu

/* The buffers the window is given: its place's size; narrower; shorter;
   both; larger, which the place cuts; wider and shorter; narrower and
   taller; one pixel; none. */
static const struct {
    INT64 columns, rows;
} pad_buffers[] = {{PAD_WIDTH, PAD_HEIGHT},
                   {PAD_WIDTH - 7, PAD_HEIGHT},
                   {PAD_WIDTH, PAD_HEIGHT - 9},
                   {PAD_WIDTH - 13, PAD_HEIGHT - 15},
                   {PAD_WIDTH + 8, PAD_HEIGHT + 6},
                   {PAD_WIDTH + 8, PAD_HEIGHT - 9},
                   {PAD_WIDTH - 7, PAD_HEIGHT + 6},
                   {1, 1},
                   {0, 0}};

/* Each buffer, with a stride equal to its columns and above them, with and
   without corners: the buffer's own pixels hold random covers and every
   other pixel of its memory, the stride's padding and the rows after, is
   POISON. The frame is D8's pixels, which show the material past the
   buffer. */
static void check_buffer_sizes(void)
{
    static TEST_WINDOW w;
    UINT32 codes[PAD_WALL_WIDTH * PAD_WALL_HEIGHT];
    UINT32 *image, *out;
    COMPOSE_WALLPAPER wallpaper;
    size_t b;
    int rounded, extra;

    limb_codes(PAD_WALL_WIDTH, PAD_WALL_HEIGHT, codes);
    image = kernel_image(codes, PAD_WALL_WIDTH, PAD_WALL_HEIGHT, PIXEL_BGR);
    out = malloc((size_t)(PAD_WALL_WIDTH * PAD_WALL_HEIGHT) * sizeof(UINT32));
    wallpaper.pixels = image;
    wallpaper.stride = PAD_WALL_WIDTH;
    wallpaper.format = PIXEL_BGR;
    for (b = 0; b < sizeof pad_buffers / sizeof pad_buffers[0]; b++)
        for (rounded = 0; rounded < 2; rounded++)
            for (extra = 0; extra < 2; extra++) {
                COMPOSE_WINDOW window;
                COMPOSE_BAND band = {out,
                                     PAD_WALL_WIDTH,
                                     {0, 0, PAD_WALL_WIDTH, PAD_WALL_HEIGHT}};
                INT64 x, y, bad = 0, cells;

                memset(&w, 0, sizeof w);
                w.left = PAD_LEFT;
                w.top = PAD_TOP;
                w.width = PAD_WIDTH;
                w.height = PAD_HEIGHT;
                w.radius = rounded ? PAD_RADIUS : NO_CORNERS;
                w.columns = pad_buffers[b].columns;
                w.rows = pad_buffers[b].rows;
                w.stride = w.columns + extra * PAD_STRIDE_EXTRA;
                w.tint = PAD_TINT;
                w.amount = PAD_AMOUNT;
                cells = w.stride * (w.rows + PAD_ROWS_AFTER);
                w.buffer = malloc((size_t)(cells > 0 ? cells : 1) *
                                  sizeof(UINT32));
                for (x = 0; x < (cells > 0 ? cells : 1); x++)
                    w.buffer[x] = POISON;
                for (y = 0; y < w.rows; y++)
                    for (x = 0; x < w.columns; x++)
                        w.buffer[y * w.stride + x] =
                            random_cover() << COMPOSE_COVER_SHIFT |
                            (UINT32)random_below(1 << 24);
                if (rounded)
                    check(compose_corners(&work, &w.corners, w.radius) == 1,
                          "a window's corners are made", w.radius, 0);
                make_shape(&w);
                compose_material(&w.material, w.tint, w.amount);
                window.pixels = w.buffer;
                window.stride = w.stride;
                window.columns = w.columns;
                window.rows = w.rows;
                window.place.left = w.left;
                window.place.top = w.top;
                window.place.right = w.left + w.width;
                window.place.bottom = w.top + w.height;
                window.material = &w.material;
                window.corners = rounded ? &w.corners : 0;
                compose_band(&band, &wallpaper, &window, 1);
                for (y = 0; y < PAD_WALL_HEIGHT; y++)
                    for (x = 0; x < PAD_WALL_WIDTH; x++) {
                        UINT32 under = codes[y * PAD_WALL_WIDTH + x];

                        bad += out[y * PAD_WALL_WIDTH + x] !=
                               compose_over(&w, under, under, x, y);
                    }
                check(bad == 0,
                      "a window shows its material past a smaller buffer and "
                      "is cut by its place",
                      w.columns * 1000 + w.rows, bad);
                free(w.buffer);
                free(w.shape);
            }
    free(image);
    free(out);
}

/* ------------------------------------------------------------------------
   Windows at the greatest coordinates */

#define LIMIT_WIDTH 16
#define LIMIT_HEIGHT 8
#define LIMIT_WINDOWS 6
#define LIMIT_RADIUS 1280 /* 5 pixels */
#define LIMIT_FAR 100     /* a twin's far edge, in pixels past the band */

/* Windows whose places reach COMPOSE_COORDINATE_MAX, each with a twin that
   has the same pixels in the band and edges a few hundred pixels away at
   most: the composer's pixels in the band are D8's for the twin, whatever
   the distance of an edge that no pixel of the band lies near. The places
   are: from the far upper left to just past the band's corner; from a
   buffer at (3, 2) to the far lower right; wide and short; everywhere; and
   two that miss the band, at the far right and the far upper left. */
static const struct {
    RECT place, twin;
    INT64 radius;
} limit_places[LIMIT_WINDOWS] = {
    {{-COMPOSE_COORDINATE_MAX, -COMPOSE_COORDINATE_MAX, 5, 4},
     {-LIMIT_FAR, -LIMIT_FAR, 5, 4},
     LIMIT_RADIUS},
    {{3, 2, COMPOSE_COORDINATE_MAX, COMPOSE_COORDINATE_MAX},
     {3, 2, 200, 150},
     LIMIT_RADIUS},
    {{-COMPOSE_COORDINATE_MAX, 2, COMPOSE_COORDINATE_MAX, 6},
     {-LIMIT_FAR, 2, 200, 6},
     NO_CORNERS},
    {{-COMPOSE_COORDINATE_MAX, -COMPOSE_COORDINATE_MAX, COMPOSE_COORDINATE_MAX,
      COMPOSE_COORDINATE_MAX},
     {-LIMIT_FAR, -LIMIT_FAR, 200, 150},
     LIMIT_RADIUS},
    {{COMPOSE_COORDINATE_MAX - 6, 1, COMPOSE_COORDINATE_MAX, 5},
     {200, 1, 206, 5},
     NO_CORNERS},
    {{-COMPOSE_COORDINATE_MAX, -COMPOSE_COORDINATE_MAX,
      -COMPOSE_COORDINATE_MAX + 9, -COMPOSE_COORDINATE_MAX + 9},
     {-200, -200, -191, -191},
     NO_CORNERS}};

static void check_band_limits(void)
{
    static TEST_WINDOW twins[LIMIT_WINDOWS];
    UINT32 codes[LIMIT_WIDTH * LIMIT_HEIGHT];
    COMPOSE_WINDOW windows[LIMIT_WINDOWS];
    UINT32 *image, *memory;
    COMPOSE_WALLPAPER wallpaper;
    INT64 stride = LIMIT_WIDTH + PADDING, x, y, bad = 0, pad = 0;
    COMPOSE_BAND band;
    int i;

    for (i = 0; i < LIMIT_WIDTH * LIMIT_HEIGHT; i++)
        codes[i] = (UINT32)random_below(1 << 24);
    image = kernel_image(codes, LIMIT_WIDTH, LIMIT_HEIGHT, PIXEL_BGR);
    wallpaper.pixels = image;
    wallpaper.stride = LIMIT_WIDTH;
    wallpaper.format = PIXEL_BGR;
    for (i = 0; i < LIMIT_WINDOWS; i++) {
        TEST_WINDOW *t = &twins[i];
        INT64 cells;

        memset(t, 0, sizeof *t);
        t->left = limit_places[i].twin.left;
        t->top = limit_places[i].twin.top;
        t->width = limit_places[i].twin.right - limit_places[i].twin.left;
        t->height = limit_places[i].twin.bottom - limit_places[i].twin.top;
        t->radius = limit_places[i].radius;
        t->columns = 20;
        t->rows = 10;
        t->stride = t->columns + 1;
        t->tint = (UINT32)random_below(1 << 24);
        t->amount = 90 + (UINT32)random_below(COMPOSE_PERCENT - 90 + 1);
        cells = t->stride * t->rows;
        t->buffer = malloc((size_t)cells * sizeof(UINT32));
        for (x = 0; x < cells; x++)
            t->buffer[x] = random_cover() << COMPOSE_COVER_SHIFT |
                           (UINT32)random_below(1 << 24);
        if (t->radius != NO_CORNERS)
            check(compose_corners(&work, &t->corners, t->radius) == 1,
                  "a window's corners are made", t->radius, i);
        make_shape(t);
        compose_material(&t->material, t->tint, t->amount);
        windows[i].pixels = t->buffer;
        windows[i].stride = t->stride;
        windows[i].columns = t->columns;
        windows[i].rows = t->rows;
        windows[i].place = limit_places[i].place;
        windows[i].material = &t->material;
        windows[i].corners = t->radius == NO_CORNERS ? 0 : &t->corners;
    }
    /* Where a window's left or top is far, its buffer lies out of the band
       and the band shows its material, as its twin's does. */
    memory = malloc((size_t)(stride * LIMIT_HEIGHT) * sizeof(UINT32));
    for (x = 0; x < stride * LIMIT_HEIGHT; x++)
        memory[x] = SENTINEL;
    band.pixels = memory;
    band.stride = stride;
    band.rect.left = 0;
    band.rect.top = 0;
    band.rect.right = LIMIT_WIDTH;
    band.rect.bottom = LIMIT_HEIGHT;
    compose_band(&band, &wallpaper, windows, LIMIT_WINDOWS);
    for (y = 0; y < LIMIT_HEIGHT; y++) {
        for (x = 0; x < LIMIT_WIDTH; x++) {
            UINT32 under = codes[y * LIMIT_WIDTH + x], want = under;

            for (i = 0; i < LIMIT_WINDOWS; i++)
                want = compose_over(&twins[i], under, want, x, y);
            bad += memory[y * stride + x] != want;
        }
        for (x = LIMIT_WIDTH; x < stride; x++)
            pad += memory[y * stride + x] != SENTINEL;
    }
    check(bad == 0,
          "windows that reach the greatest coordinates compose as their twins "
          "do",
          bad, 0);
    check(pad == 0, "and leave the band's padding alone", pad, 0);
    for (i = 0; i < LIMIT_WINDOWS; i++) {
        free(twins[i].buffer);
        free(twins[i].shape);
    }
    free(memory);
    free(image);
}

/* ------------------------------------------------------------------------
   Corners */

/* The largest screen's scale is 5.5 (display.md D7 rule 5: 4320 / 800,
   rounded to the nearest quarter). There a window's radius of 12 points
   (look.md V6) is 66 pixels and a raised surface's 16 points are 88, the
   most the composer makes. */
#define LARGEST_WINDOW_RADIUS (66 * OUTLINE_ONE)
#define LARGEST_RAISED_RADIUS (88 * OUTLINE_ONE)

/* Radii in 256ths of a pixel: under a pixel, a pixel and a little over,
   either side of a whole pixel, halves and quarters, look.md V4's 12 and
   16, five quarters of the cells the screens give, exactly (8.75, 10, 20
   and 22.5 pixels, from cells 7, 8, 16 and 18 wide), which a theme's
   radius can be where the console's panel rounds it down, a few more odd
   ones, and the two radii of the largest screen. */
static const INT64 corner_radii[] = {
    1,    128,  256,  257,  1408, 1793, 2240, 2303, 2560, 3072, 3520,
    4096, 4160, 5120, 5760, 8064, 11520, LARGEST_WINDOW_RADIUS,
    LARGEST_RAISED_RADIUS};
#define CORNER_RADII (sizeof corner_radii / sizeof corner_radii[0])

/* Where the window lies in the plane the oracle fills: pixels from the
   plane's origin, whole ones, as a window is placed. */
static const struct {
    INT64 left, top;
} corner_places[] = {{0, 0}, {7, 3}, {130, 66}};
#define CORNER_PLACES (sizeof corner_places / sizeof corner_places[0])

/* The pixels around the window in the plane, which the oracle fills as
   it does the window's. */
#define CORNER_MARGIN 3

/* The pixels of a window of width x height at (left, top), of corners
   made for radius, where the corners' coverage is not what
   outline_coverage gives the window's own rounded rectangle there. The
   rectangle is the one outline_rounded_rect draws for the window itself,
   in a plane that holds the window with a margin, so it neither shares
   the corners' square nor starts where it does. */
static INT64 corner_mismatches(const COMPOSE_CORNERS *corners, INT64 radius,
                               INT64 width, INT64 height, INT64 left,
                               INT64 top)
{
    INT64 plane_width = left + width + CORNER_MARGIN;
    INT64 plane_height = top + height + CORNER_MARGIN;
    UINT8 *plane = malloc((size_t)(plane_width * plane_height));
    INT64 x, y, wrong = 0;

    outline_start(&work);
    outline_rounded_rect(&work, left * OUTLINE_ONE, top * OUTLINE_ONE,
                         (left + width) * OUTLINE_ONE,
                         (top + height) * OUTLINE_ONE, radius);
    check(outline_coverage(&work, plane, plane_width, plane_height) == 1,
          "the window's rounded rectangle gives its coverage", radius, width);
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++)
            wrong += compose_corner(corners, width, height, x, y) !=
                     plane[(top + y) * plane_width + left + x];
    free(plane);
    return wrong;
}

/* Every pixel of windows of many sizes, at several places, equals the
   coverage of the window's own rounded rectangle: the windows the
   corners are for, from the least they hold up to the console's, which
   panel_layout gives for each of the screens the boots run, with the
   radius the spec gives each. */
static void check_corner_pixels(void)
{
    static COMPOSE_CORNERS corners;
    size_t r, i, p;

    for (r = 0; r < CORNER_RADII; r++) {
        INT64 radius = corner_radii[r];
        INT64 reach = (radius + OUTLINE_ONE - 1) / OUTLINE_ONE;
        struct {
            INT64 width, height;
        } shapes[6 + SIZES] = {{2 * reach, 2 * reach},
                               {2 * reach + 1, 2 * reach},
                               {2 * reach, 2 * reach + 1},
                               {2 * reach + 3, 2 * reach + 7},
                               {2 * reach, 2 * reach + 150},
                               {257, 2 * reach}};
        size_t count = 6;

        check(compose_corners(&work, &corners, radius) == 1 &&
                  corners.reach == reach,
              "corners are made for a radius", radius, reach);
        for (i = 0; i < SIZES; i++) {
            LAYOUT layout;

            if (!panel_layout(&layout, sizes[i].width, sizes[i].height)) {
                check(0, "the panel lays out", sizes[i].width, 0);
                continue;
            }
            shapes[count].width = layout.box.right - layout.box.left;
            shapes[count].height = layout.box.bottom - layout.box.top;
            count++;
        }
        for (i = 0; i < count; i++)
            for (p = 0; p < CORNER_PLACES; p++) {
                INT64 wrong = corner_mismatches(
                    &corners, radius, shapes[i].width, shapes[i].height,
                    corner_places[p].left, corner_places[p].top);

                check(wrong == 0,
                      "a window's corners are its rounded rectangle's coverage",
                      radius, wrong);
                if (wrong && failures <= FAILURES_SHOWN)
                    printf("  %lld x %lld at %lld, %lld\n",
                           (long long)shapes[i].width,
                           (long long)shapes[i].height,
                           (long long)corner_places[p].left,
                           (long long)corner_places[p].top);
            }
    }
}

/* The corners the spec gives each screen the boots run: the console's
   window, at the radius panel_layout gives, five quarters of the cell
   rounded down to whole pixels (display.md D19, phase 3 arrangement,
   rule 4). */
static void check_console_corners(void)
{
    static COMPOSE_CORNERS corners;
    size_t i;

    for (i = 0; i < SIZES; i++) {
        LAYOUT layout;
        INT64 radius, width, height;

        if (!panel_layout(&layout, sizes[i].width, sizes[i].height))
            continue;
        radius = layout.radius * OUTLINE_ONE;
        width = layout.box.right - layout.box.left;
        height = layout.box.bottom - layout.box.top;
        check(compose_corners(&work, &corners, radius) == 1,
              "the console's corners are made", sizes[i].width, radius);
        check(corner_mismatches(&corners, radius, width, height,
                                layout.box.left, layout.box.top) == 0,
              "the console's corners are its window's rounded rectangle",
              sizes[i].width, radius);
    }
}

/* Two pixels under twice the reach the corners overlap, and no plane of
   corners has the window's pixels, for each of these radii: the
   composer's precondition is not slack. (A pixel under can still agree
   by chance, as at 10 pixels, so that is not the width to show it by.) */
static void check_corners_too_small(void)
{
    static const INT64 radii[] = {2240, 2560, 4160, 5760};
    static COMPOSE_CORNERS corners;
    size_t r;

    for (r = 0; r < sizeof radii / sizeof radii[0]; r++) {
        INT64 reach = (radii[r] + OUTLINE_ONE - 1) / OUTLINE_ONE;
        INT64 small = 2 * reach - 2;

        check(compose_corners(&work, &corners, radii[r]) == 1,
              "corners are made for a radius", radii[r], reach);
        check(corner_mismatches(&corners, radii[r], small, 60, 0, 0) != 0,
              "a window two under twice the reach is not its corners'",
              radii[r], small);
        check(corner_mismatches(&corners, radii[r], 60, small, 0, 0) != 0,
              "a window two under twice the reach is not its corners'",
              radii[r], small);
    }
}

/* A window smaller than that still reads inside the plane: the bytes
   after the plane in memory are a value the plane does not hold, and no
   pixel of any smaller window gives it. */
static void check_corner_reads(INT64 radius)
{
    static COMPOSE_CORNERS corners;
    INT64 reach = (radius + OUTLINE_ONE - 1) / OUTLINE_ONE;
    INT64 used = 4 * reach * reach, w, h, x, y, bad = 0;
    unsigned char present[CODES] = {0};
    int absent = -1, v;

    memset(&corners, 0xC3, sizeof corners);
    check(compose_corners(&work, &corners, radius) == 1,
          "corners are made for a radius", radius, reach);
    for (x = 0; x < used; x++)
        present[corners.plane[x]] = 1;
    for (v = 0; v < CODES && absent < 0; v++)
        absent = present[v] ? -1 : v;
    check(absent >= 0, "the plane leaves a code unused", radius, reach);
    if (absent < 0)
        return;
    for (x = used; x < (INT64)sizeof corners.plane; x++)
        check(corners.plane[x] == 0xC3, "the corners write only their plane",
              radius, x);
    memset(corners.plane + used, absent,
           sizeof corners.plane - (size_t)used);
    for (w = 1; w < 2 * reach; w++)
        for (h = 1; h < 2 * reach; h++)
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++)
                    bad += compose_corner(&corners, w, h, x, y) ==
                           (UINT32)absent;
    check(bad == 0, "a small window reads inside the plane", radius, bad);
}

/* Radii refused, and square corners. */
static void check_corner_refusals(void)
{
    static COMPOSE_CORNERS corners, before;
    INT64 dx, dy;
    INT64 bad = 0;

    memset(&corners, 0x5A, sizeof corners);
    corners.reach = 77;
    before = corners;
    check(compose_corners(&work, &corners, -1) == 0 &&
              memcmp(&corners, &before, sizeof corners) == 0,
          "a negative radius is refused and changes nothing", -1, 0);
    check(compose_corners(&work, &corners, LARGEST_RAISED_RADIUS + 1) == 0 &&
              memcmp(&corners, &before, sizeof corners) == 0,
          "a radius past the reach is refused and changes nothing",
          LARGEST_RAISED_RADIUS + 1, 0);
    check(compose_corners(&work, &corners, LARGEST_RAISED_RADIUS) == 1 &&
              corners.reach == 88,
          "the largest screen's greatest radius is made", LARGEST_RAISED_RADIUS,
          corners.reach);
    check(compose_corners(&work, &corners, 5 * OUTLINE_ONE) == 1 &&
              corners.reach == 5,
          "corners are made again", 5, corners.reach);
    check(compose_corners(&work, &corners, 0) == 1 && corners.reach == 0,
          "a radius of 0 makes square corners", 0, corners.reach);
    for (dy = 0; dy < 9; dy++)
        for (dx = 0; dx < 9; dx++)
            bad += compose_corner(&corners, 9, 9, dx, dy) != COMPOSE_COVER_MAX ||
                   compose_corner(0, 9, 9, dx, dy) != COMPOSE_COVER_MAX;
    check(bad == 0, "square corners and no corners leave every pixel whole",
          9, bad);
}

static void check_corners(void)
{
    check_corner_pixels();
    check_console_corners();
    check_corners_too_small();
    check_corner_reads(OUTLINE_ONE);
    check_corner_reads(10 * OUTLINE_ONE);
    check_corner_reads(5760);
    check_corner_refusals();
}

/* ------------------------------------------------------------------------
   The wallpaper alone, and the console's panel */

static void check_wallpaper_alone(INT64 width, INT64 height,
                                  const UINT32 *codes)
{
    long tile_rows = 16;
    size_t f;

    read_const("TILE_ROWS", &tile_rows);
    for (f = 0; f < FORMATS; f++) {
        UINT32 *image = kernel_image(codes, width, height, formats[f]);
        UINT32 *screen_memory = aligned_alloc(
            OUTPUT_BLOCK_BYTES,
            (size_t)((width * height * 4 + OUTPUT_BLOCK_BYTES - 1) /
                     OUTPUT_BLOCK_BYTES * OUTPUT_BLOCK_BYTES));
        UINT32 *band_memory =
            malloc((size_t)(width * (height + 1)) * sizeof(UINT32));
        SCREEN screen = {screen_memory, width, height, width, formats[f]};
        COMPOSE_WALLPAPER wallpaper = {image, width, formats[f]};
        /* The bands of TILE_ROWS the compositor draws in, a band of one
           row, a height that leaves a short last band, one band for the
           screen, and one taller than it. */
        const INT64 rows_of[] = {tile_rows, 1, 7, height, height + 1};
        INT64 i;
        size_t r;

        for (r = 0; r < sizeof rows_of / sizeof rows_of[0]; r++) {
            INT64 bad = 0, unused = 0;

            for (i = 0; i < width * height; i++)
                screen_memory[i] = SCREEN_BEFORE;
            compose_wallpaper_frame(&screen, &wallpaper, band_memory,
                                    rows_of[r]);
            for (i = 0; i < width * height; i++) {
                bad += (screen_memory[i] & 0xFFFFFFu) !=
                       (image[i] & 0xFFFFFFu);
                unused += screen_memory[i] >> 24 != 0;
            }
            check(bad == 0,
                  "a frame of the wallpaper alone writes the wallpaper's "
                  "pixels over a screen that held others",
                  width * 10000 + height, bad);
            check(unused == 0, "an output pixel's unused byte is 0", width,
                  unused);
        }
        for (i = 0; i < width * height; i++)
            screen_memory[i] = SCREEN_BEFORE;
        compose_wallpaper_frame(&screen, &wallpaper, band_memory, 0);
        check(screen_memory[0] == SCREEN_BEFORE &&
                  screen_memory[width * height - 1] == SCREEN_BEFORE,
              "a frame in bands of no rows writes nothing", width, 0);
        free(image);
        free(screen_memory);
        free(band_memory);
    }
}

/* Whether background, as sections sections hold it, is refused as the
   wallpaper on a display of display_width x display_height for reason, or
   taken where reason is 0. */
static int refused_for(UINT32 background, UINT32 width, UINT32 height,
                       UINT32 format, UINT32 sections, UINT32 display_width,
                       UINT32 display_height, const char *reason)
{
    BOOTSTRAP_BACKGROUND label = {background, width, height, format};
    BOOTSTRAP_DISPLAY display = {0, display_width, display_height,
                                 display_width, PIXEL_BGR};
    const char *refusal = compose_wallpaper_refusal(&label, &display, sections);

    if (!reason)
        return refusal == 0;
    return refusal && strcmp(refusal, reason) == 0;
}

/* The kernel's background is the wallpaper only where it is Limb, in a
   format the composer reads, at the display's size, in the sections an
   image that size takes (docs/spec/display.md D2); each refusal names
   its cause. */
static void check_wallpaper_refusals(void)
{
    const UINT32 w = 1280, h = 800;
    const UINT32 sections = (UINT32)bootstrap_background_sections(w, h);
    const UINT32 small = LIMB_MIN_SIDE - 1, large = LIMB_MAX_SIDE + 1;

    check(refused_for(BOOTSTRAP_BACKGROUND_LIMB, w, h, PIXEL_BGR, sections, w,
                      h, 0),
          "Limb at the display's size is the wallpaper", w, h);
    check(refused_for(BOOTSTRAP_BACKGROUND_LIMB, w, h, PIXEL_RGB, sections, w,
                      h, 0),
          "Limb in the other format is the wallpaper", w, h);
    check(refused_for(BOOTSTRAP_BACKGROUND_NONE, 0, 0, 0, 0, w, h,
                      "no image"),
          "no image is refused", w, h);
    check(refused_for(BOOTSTRAP_BACKGROUND_LIMB + 1, w, h, PIXEL_BGR, sections,
                      w, h, "not limb"),
          "another background is refused", w, h);
    check(refused_for(BOOTSTRAP_BACKGROUND_LIMB, w, h, PIXEL_BGR + 1, sections,
                      w, h, "unknown format"),
          "an unknown format is refused", w, h);
    check(refused_for(BOOTSTRAP_BACKGROUND_LIMB, w, h, PIXEL_BGR, sections,
                      w + 1, h, "another size"),
          "a display a column wider is refused", w, h);
    check(refused_for(BOOTSTRAP_BACKGROUND_LIMB, w, h, PIXEL_BGR, sections, w,
                      h + 1, "another size"),
          "a display a row taller is refused", w, h);
    check(refused_for(BOOTSTRAP_BACKGROUND_LIMB, small, small, PIXEL_BGR, 1,
                      small, small, "no size"),
          "a side Limb does not take is refused", small, small);
    check(refused_for(BOOTSTRAP_BACKGROUND_LIMB, large, h, PIXEL_BGR,
                      (UINT32)bootstrap_background_sections(large, h), large,
                      h, "no size"),
          "a side past Limb's largest is refused", large, h);
    check(refused_for(BOOTSTRAP_BACKGROUND_LIMB, w, h, PIXEL_BGR, sections - 1,
                      w, h, "wrong sections"),
          "an image in a section too few is refused", w, h);
    check(refused_for(BOOTSTRAP_BACKGROUND_LIMB, w, h, PIXEL_BGR, sections + 1,
                      w, h, "wrong sections"),
          "an image in a section too many is refused", w, h);
}

/* The console's panel over Limb, computed by console/panel.c, against the
   PANEL material of the composer over the same wallpaper, in the panel's
   place: wherever a corner does not cut the panel they are the same
   pixels. */
static void check_panel(INT64 width, INT64 height, const UINT32 *codes)
{
    static COMPOSE_MATERIAL material;
    UINT32 tint, amount;
    LAYOUT layout;
    INT64 w, h, x, y, wrong = 0;
    UINT32 *backdrop, *zeros, *out;
    UINT32 *image = kernel_image(codes, width, height, PIXEL_BGR);
    COMPOSE_WALLPAPER wallpaper = {image, width, PIXEL_BGR};
    COMPOSE_WINDOW window;
    COMPOSE_BAND band;

    if (!read_material("theme.material.panel", &tint, &amount) ||
        !panel_layout(&layout, width, height)) {
        check(0, "the panel lays out", width, height);
        free(image);
        return;
    }
    w = layout.box.right - layout.box.left;
    h = layout.box.bottom - layout.box.top;
    backdrop = malloc((size_t)panel_backdrop_size(&layout));
    zeros = calloc((size_t)(w * h), sizeof(UINT32));
    out = malloc((size_t)(w * h) * sizeof(UINT32));
    panel_backdrop(&layout, width, height, backdrop);
    compose_material(&material, tint, amount);
    window.pixels = zeros;
    window.stride = w;
    window.columns = w;
    window.rows = h;
    window.place = layout.box;
    window.material = &material;
    window.corners = 0;
    band.pixels = out;
    band.stride = w;
    band.rect = layout.box;
    compose_band(&band, &wallpaper, &window, 1);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            int corner = (x < layout.radius || x >= w - layout.radius) &&
                         (y < layout.radius || y >= h - layout.radius);

            wrong += !corner && out[y * w + x] != backdrop[y * w + x];
        }
    check(wrong == 0, "the PANEL material is the console panel's backdrop",
          width * 10000 + height, wrong);
    free(backdrop);
    free(zeros);
    free(out);
    free(image);
}

/* ------------------------------------------------------------------------
   The surfaces over Limb's brightest pixel (look.md V4) */

/* Each surface's tint and weight, and what it makes of white, which is the
   colour of Limb's brightest pixel at each screen size measured
   (limb_pixel at 1280 x 800, 1920 x 1080, 2560 x 1600, 2880 x 1800, 3840 x
   2160 and 7680 x 4320; check_peak shows it at the sizes this file runs).
   The first two are V4's table; the last two are the weights its "Why"
   gives for the window. Each colour is the mix in linear light, rounded
   to the nearest code. */
static const struct {
    const char *name;
    UINT32 tint, amount, peak;
} peaks[] = {{"window", 0x151A24, 99, 0x24272E},
             {"raised", 0x1B2130, 99, 0x282C38},
             {"window at 98 percent", 0x151A24, 98, 0x2E3036},
             {"window at 96 percent", 0x151A24, 96, 0x3D3F43}};
#define PEAKS (sizeof peaks / sizeof peaks[0])

static void check_peak_materials(void)
{
    static COMPOSE_MATERIAL material;
    size_t i;

    for (i = 0; i < PEAKS; i++) {
        UINT32 got;

        check(compose_material(&material, peaks[i].tint, peaks[i].amount) == 1,
              "a surface's material is made", peaks[i].tint, peaks[i].amount);
        got = (UINT32)material.code[0][CODES - 1] << 16 |
              (UINT32)material.code[1][CODES - 1] << 8 |
              material.code[2][CODES - 1];
        check(got == peaks[i].peak,
              "a surface over white is the colour V4 gives over Limb's peak",
              peaks[i].tint, got);
    }
}

/* Limb's brightest pixel at this size is white, and the description's
   window and raised surfaces make V4's colours of it, through the
   composer. */
static void check_peak(INT64 width, INT64 height, const UINT32 *codes)
{
    static const char *const names[] = {"theme.material.window",
                                        "theme.material.raised"};
    UINT32 *image = kernel_image(codes, width, height, PIXEL_BGR);
    COMPOSE_WALLPAPER wallpaper = {image, width, PIXEL_BGR};
    INT64 i, at = 0;
    unsigned best = 0;
    size_t m;

    for (i = 0; i < width * height; i++) {
        unsigned sum = channel(codes[i], 0) + channel(codes[i], 1) +
                       channel(codes[i], 2);

        if (sum > best) {
            best = sum;
            at = i;
        }
    }
    check(codes[at] == 0xFFFFFF, "Limb's brightest pixel is white", width,
          codes[at]);
    for (m = 0; m < sizeof names / sizeof names[0]; m++) {
        static COMPOSE_MATERIAL material;
        UINT32 tint, amount, covered = 0, out = 0;
        COMPOSE_WINDOW window;
        COMPOSE_BAND band;

        if (!read_material(names[m], &tint, &amount)) {
            check(0, "the description has the material", (long long)m, 0);
            continue;
        }
        compose_material(&material, tint, amount);
        window.pixels = &covered;
        window.stride = 1;
        window.columns = 1;
        window.rows = 1;
        window.place.left = at % width;
        window.place.top = at / width;
        window.place.right = window.place.left + 1;
        window.place.bottom = window.place.top + 1;
        window.material = &material;
        window.corners = 0;
        band.pixels = &out;
        band.stride = 1;
        band.rect = window.place;
        compose_band(&band, &wallpaper, &window, 1);
        check(out == peaks[m].peak,
              "the description's surface over Limb's peak is V4's colour",
              width * 10000 + height, out);
    }
    free(image);
}

/* ------------------------------------------------------------------------
   The output's blocks */

#define BLOCK_PIXELS (OUTPUT_BLOCK_BYTES / sizeof(UINT32))
#define OUTPUT_WIDTH 70
#define OUTPUT_HEIGHT 12
#define OUTPUT_ROUNDS 5
#define OUTPUT_MEMORY_BYTES (64 * 1024)

/* The block of the framebuffer that holds row y's pixel x. */
static UINT64 block_of(const SCREEN *screen, INT64 x, INT64 y)
{
    return (UINT64)(UINTN)(screen->pixels + y * screen->stride + x) /
           OUTPUT_BLOCK_BYTES;
}

/* Write a random rectangle of a random band to a framebuffer whose first
   pixel lies offset pixels past a block's start, and compare every pixel
   of it with what the rule gives: the pixels of the blocks from the one
   holding the rectangle's left edge to the one holding its last column,
   in each of its rows, written from the band as gfx_pixel gives them, and no
   other pixel touched. The rule is applied here pixel by pixel from each
   pixel's address. */
static void check_one_output(UINT32 *memory, size_t offset, INT64 stride,
                             UINT32 format)
{
    SCREEN screen = {memory + offset, OUTPUT_WIDTH, OUTPUT_HEIGHT, stride,
                     format};
    RECT rect, wide;
    COMPOSE_BAND band;
    UINT32 *band_memory;
    INT64 x, y, band_width, wrong = 0, runs = 0, outside = 0;

    rect.left = random_below(OUTPUT_WIDTH - 1);
    rect.right = rect.left + 1 + random_below(OUTPUT_WIDTH - 1 - rect.left);
    rect.top = random_below(OUTPUT_HEIGHT - 1);
    rect.bottom = rect.top + 1 + random_below(OUTPUT_HEIGHT - 1 - rect.top);
    wide = compose_blocks(&screen, &rect);
    band_width = wide.right - wide.left;
    band_memory = malloc((size_t)(band_width * OUTPUT_HEIGHT) * sizeof(UINT32));
    for (x = 0; x < band_width * OUTPUT_HEIGHT; x++)
        band_memory[x] = (UINT32)(next_random() & COMPOSE_COLOUR_MASK);
    for (x = 0; x < stride * OUTPUT_HEIGHT; x++)
        memory[offset + (size_t)x] = SENTINEL;
    band.pixels = band_memory;
    band.stride = band_width;
    band.rect.left = wide.left;
    band.rect.right = wide.right;
    band.rect.top = 0;
    band.rect.bottom = OUTPUT_HEIGHT;
    compose_write(&band, &screen, &rect);

    for (y = 0; y < OUTPUT_HEIGHT; y++) {
        UINT64 low = block_of(&screen, rect.left, y);
        UINT64 high = block_of(&screen, rect.right - 1, y);
        INT64 first = -1, last = -1;

        for (x = 0; x < OUTPUT_WIDTH; x++) {
            UINT64 here = block_of(&screen, x, y);
            int rows = y >= rect.top && y < rect.bottom;
            int expected = rows && here >= low && here <= high;
            UINT32 got = memory[offset + (size_t)(y * stride + x)];

            if (expected && (x < wide.left || x >= wide.right)) {
                outside++;
            } else if (expected) {
                UINT32 source = band_memory[y * band_width + (x - wide.left)];

                wrong +=
                    got != gfx_pixel(&screen, channel(source, 0),
                                     channel(source, 1), channel(source, 2));
            } else {
                wrong += got != SENTINEL;
            }
            if (got != SENTINEL) {
                first = first < 0 ? x : first;
                last = x;
            }
        }
        for (x = OUTPUT_WIDTH; x < stride; x++)
            wrong += memory[offset + (size_t)(y * stride + x)] != SENTINEL;
        /* A run that does not reach the row's end starts and stops on a
           block's edge, so the processor fills whole buffers. */
        if (first > 0)
            runs +=
                block_of(&screen, first, y) == block_of(&screen, first - 1, y);
        if (last >= 0 && last < OUTPUT_WIDTH - 1)
            runs +=
                block_of(&screen, last, y) == block_of(&screen, last + 1, y);
    }
    check(
        wrong == 0,
        "the output writes the blocks that hold the rectangle and nothing else",
        (long long)stride * 100 + (long long)offset, wrong);
    check(outside == 0, "the widened rectangle holds every row's blocks",
          (long long)stride * 100 + (long long)offset, outside);
    check(runs == 0, "the output's runs start and stop on a block's edge",
          (long long)stride * 100 + (long long)offset, runs);
    check(wide.left <= rect.left && wide.right >= rect.right &&
              wide.top == rect.top && wide.bottom == rect.bottom,
          "the widened rectangle holds the rectangle", rect.left, rect.right);
    free(band_memory);
}

static void check_blocks(void)
{
    /* Rows of a whole number of blocks, and rows that are not. */
    static const INT64 strides[] = {80, 77, 71};
    UINT32 *memory = aligned_alloc(OUTPUT_BLOCK_BYTES, OUTPUT_MEMORY_BYTES);
    size_t s, offset;
    int round;

    for (s = 0; s < sizeof strides / sizeof strides[0]; s++)
        for (offset = 0; offset < BLOCK_PIXELS; offset++)
            for (round = 0; round < OUTPUT_ROUNDS; round++)
                check_one_output(memory, offset, strides[s],
                                 round % 2 ? PIXEL_RGB : PIXEL_BGR);
    free(memory);
}

/* The rectangle compose_blocks must give for rect on a screen of
   OUTPUT_WIDTH x OUTPUT_HEIGHT, written out from D8's rule: rect cut to the
   screen; then in each of its rows from the first pixel of the block that
   holds its left edge to the last of the block that holds its last column,
   cut to the row; and the union of the rows. *active says whether any of
   rect is on the screen. */
static RECT expected_blocks(const SCREEN *screen, const RECT *rect, int *active)
{
    RECT cut, wide;
    INT64 x, y;

    cut.left = rect->left < 0 ? 0 : rect->left;
    cut.top = rect->top < 0 ? 0 : rect->top;
    cut.right = rect->right > OUTPUT_WIDTH ? OUTPUT_WIDTH : rect->right;
    cut.bottom = rect->bottom > OUTPUT_HEIGHT ? OUTPUT_HEIGHT : rect->bottom;
    *active = cut.left < cut.right && cut.top < cut.bottom;
    if (!*active)
        return cut;
    wide.left = OUTPUT_WIDTH;
    wide.right = 0;
    wide.top = cut.top;
    wide.bottom = cut.bottom;
    for (y = cut.top; y < cut.bottom; y++) {
        UINT64 low = block_of(screen, cut.left, y);
        UINT64 high = block_of(screen, cut.right - 1, y);

        for (x = 0; x < OUTPUT_WIDTH; x++) {
            UINT64 here = block_of(screen, x, y);

            if (here >= low && here <= high) {
                wide.left = x < wide.left ? x : wide.left;
                wide.right = x + 1 > wide.right ? x + 1 : wide.right;
            }
        }
    }
    return wide;
}

#define CUT_STRIDE_EXTRA 5
#define CUT_LESS 3

/* The rectangles written: on the screen, whole and in one corner; off it
   at each side, and around it; wholly off it at each side; and empty. */
static const RECT cut_rects[] = {
    {9, 3, 41, 8},
    {0, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT},
    {OUTPUT_WIDTH - 1, OUTPUT_HEIGHT - 1, OUTPUT_WIDTH, OUTPUT_HEIGHT},
    {-12, 2, 20, 6},
    {50, 2, 90, 6},
    {10, -5, 40, 4},
    {10, 7, 40, 20},
    {-9, -9, 90, 30},
    {-30, 2, -5, 6},
    {75, 2, 99, 6},
    {10, -9, 40, -1},
    {10, OUTPUT_HEIGHT, 40, 20},
    {10, 3, 10, 8}};
#define CUT_RECTS (sizeof cut_rects / sizeof cut_rects[0])

/* The greatest coordinates the composer takes. A composer that cuts a
   rectangle to the screen takes time in proportion to the screen on
   these; one that does not takes time in proportion to the coordinates, so
   they are run only once the cuts above pass. */
static const RECT cut_extremes[] = {
    {-COMPOSE_COORDINATE_MAX, -COMPOSE_COORDINATE_MAX, COMPOSE_COORDINATE_MAX,
     COMPOSE_COORDINATE_MAX},
    {COMPOSE_COORDINATE_MAX - 1, 0, COMPOSE_COORDINATE_MAX, OUTPUT_HEIGHT},
    {-COMPOSE_COORDINATE_MAX, 2, -COMPOSE_COORDINATE_MAX + 1, 6},
    {10, -COMPOSE_COORDINATE_MAX, 40, COMPOSE_COORDINATE_MAX}};
#define CUT_EXTREMES (sizeof cut_extremes / sizeof cut_extremes[0])

/* What the band holds, from the blocks of the rectangle, wide, on the
   rows from cut.top to cut.bottom: all of them; the whole screen; and less
   than all of them at each edge in turn, and at all four. */
enum {
    HOLDS_EXACT,
    HOLDS_MORE,
    HOLDS_LESS_LEFT,
    HOLDS_LESS_RIGHT,
    HOLDS_LESS_TOP,
    HOLDS_LESS_BOTTOM,
    HOLDS_LESS_ALL,
    HOLDS_KINDS
};

/* Write rect from a band of the given kind to a framebuffer whose first
   pixel lies offset pixels past a block's start. The pixels written are
   those of the blocks that hold rect on the screen and that the band
   holds, from the band's own pixel; no other pixel is touched; and
   compose_blocks is the rectangle expected_blocks gives. Returns 0 if the
   kind does not apply to this rectangle. */
static int check_one_cut(UINT32 *memory, size_t offset, INT64 stride,
                         UINT32 format, const RECT *rect, int kind)
{
    SCREEN screen = {memory + offset, OUTPUT_WIDTH, OUTPUT_HEIGHT, stride,
                     format};
    int active;
    RECT wide = expected_blocks(&screen, rect, &active);
    RECT cut = wide, got = compose_blocks(&screen, rect), band_rect, whole;
    COMPOSE_BAND band;
    UINT32 *band_memory;
    INT64 x, y, band_width, band_height, band_stride, wrong = 0;
    int less_left = kind == HOLDS_LESS_LEFT || kind == HOLDS_LESS_ALL;
    int less_right = kind == HOLDS_LESS_RIGHT || kind == HOLDS_LESS_ALL;
    int less_top = kind == HOLDS_LESS_TOP || kind == HOLDS_LESS_ALL;
    int less_bottom = kind == HOLDS_LESS_BOTTOM || kind == HOLDS_LESS_ALL;

    whole.left = 0;
    whole.top = 0;
    whole.right = OUTPUT_WIDTH;
    whole.bottom = OUTPUT_HEIGHT;
    if (!active && kind != HOLDS_MORE)
        return 0;
    if (active && cut.right - cut.left <= 2 * CUT_LESS &&
        (less_left || less_right))
        return 0;
    if (active && cut.bottom - cut.top <= 2 && (less_top || less_bottom))
        return 0;
    check(active ? got.left == wide.left && got.right == wide.right &&
                       got.top == wide.top && got.bottom == wide.bottom
                 : got.left >= got.right || got.top >= got.bottom,
          "compose_blocks is the blocks of the rectangle cut to the screen",
          rect->left, (long long)offset);
    band_rect = kind == HOLDS_MORE || !active ? whole : wide;
    band_rect.left += less_left ? CUT_LESS : 0;
    band_rect.right -= less_right ? CUT_LESS : 0;
    band_rect.top += less_top ? 1 : 0;
    band_rect.bottom -= less_bottom ? 1 : 0;
    band_width = band_rect.right - band_rect.left;
    band_height = band_rect.bottom - band_rect.top;
    band_stride = band_width + CUT_STRIDE_EXTRA;
    band_memory = malloc((size_t)(band_stride * band_height) * sizeof(UINT32));
    for (x = 0; x < band_stride * band_height; x++)
        band_memory[x] = (UINT32)(next_random() & COMPOSE_COLOUR_MASK);
    for (x = 0; x < stride * OUTPUT_HEIGHT; x++)
        memory[offset + (size_t)x] = SENTINEL;
    band.pixels = band_memory;
    band.stride = band_stride;
    band.rect = band_rect;
    compose_write(&band, &screen, rect);

    for (y = 0; y < OUTPUT_HEIGHT; y++) {
        int row = active && y >= cut.top && y < cut.bottom;
        UINT64 low = row ? block_of(&screen, rect->left < 0 ? 0 : rect->left, y)
                         : 0;
        UINT64 high =
            row ? block_of(&screen, rect->right > OUTPUT_WIDTH
                                        ? OUTPUT_WIDTH - 1
                                        : rect->right - 1,
                           y)
                : 0;

        for (x = 0; x < OUTPUT_WIDTH; x++) {
            UINT64 here = block_of(&screen, x, y);
            int held = x >= band_rect.left && x < band_rect.right &&
                       y >= band_rect.top && y < band_rect.bottom;
            int expected = row && here >= low && here <= high && held;
            UINT32 got_pixel = memory[offset + (size_t)(y * stride + x)];

            if (expected) {
                UINT32 source = band_memory[(y - band_rect.top) * band_stride +
                                            (x - band_rect.left)];

                wrong += got_pixel != gfx_pixel(&screen, channel(source, 0),
                                                channel(source, 1),
                                                channel(source, 2));
            } else {
                wrong += got_pixel != SENTINEL;
            }
        }
        for (x = OUTPUT_WIDTH; x < stride; x++)
            wrong += memory[offset + (size_t)(y * stride + x)] != SENTINEL;
    }
    check(wrong == 0,
          "the output writes the blocks of the rectangle on the screen that "
          "the band holds, and nothing else",
          (long long)kind * 1000 + (long long)offset, wrong);
    free(band_memory);
    return 1;
}

static void check_write_cuts(void)
{
    static const INT64 strides[] = {80, 77, 71};
    UINT32 *memory = aligned_alloc(OUTPUT_BLOCK_BYTES, OUTPUT_MEMORY_BYTES);
    size_t s, offset, r;
    int kind, runs = 0;

    check(COMPOSE_BLOCK_BYTES == OUTPUT_BLOCK_BYTES,
          "the composer's block is D8's 64 bytes", COMPOSE_BLOCK_BYTES,
          OUTPUT_BLOCK_BYTES);
    for (s = 0; s < sizeof strides / sizeof strides[0]; s++)
        for (offset = 0; offset < BLOCK_PIXELS; offset++)
            for (r = 0; r < CUT_RECTS; r++)
                for (kind = 0; kind < HOLDS_KINDS; kind++)
                    runs += check_one_cut(memory, offset, strides[s],
                                          (r + (size_t)kind) % 2 ? PIXEL_RGB
                                                                 : PIXEL_BGR,
                                          &cut_rects[r], kind);
    check(runs > 0, "cuts are run", runs, 0);
    for (s = 0; failures == 0 && s < sizeof strides / sizeof strides[0]; s++)
        for (offset = 0; offset < BLOCK_PIXELS; offset++)
            for (r = 0; r < CUT_EXTREMES; r++)
                for (kind = 0; kind < HOLDS_KINDS; kind++)
                    check_one_cut(memory, offset, strides[s],
                                  (r + (size_t)kind) % 2 ? PIXEL_RGB
                                                         : PIXEL_BGR,
                                  &cut_extremes[r], kind);
    free(memory);
}

/* ------------------------------------------------------------------------
   The pointer */

#define ARROW_X 40
#define ARROW_Y 60
#define BAND_LEFT 30
#define BAND_TOP 44
#define BAND_WIDTH 70
#define BAND_HEIGHT 80
#define BACKGROUND 0x203040
#define POINT_UNITS 256
#define CURVE_STEPS 64
#define POLY_MAX 4096
#define MARGIN 3
#define EDGE_PERCENT_TEST 85

typedef struct {
    double x, y;
} POINT;

static INT64 floor_div(INT64 a, INT64 b)
{
    INT64 q = a / b;

    return q * b > a ? q - 1 : q;
}

static INT64 ceil_div(INT64 a, INT64 b) { return -floor_div(-a, b); }

/* v in 256ths of a point, in pixels at quarters / 4: rounded once to the
   nearest 256th of a pixel, halves up (look.md V2, "rounded once";
   compose.h), then a pixel position. */
static double pixels_of(INT32 v, UINT32 quarters)
{
    return floor((double)v * quarters / COMPOSE_QUARTERS + 0.5) / POINT_UNITS;
}

/* Contour 0 of the shape as a polygon in pixels, hotspot at the origin,
   each curve cut into CURVE_STEPS steps of its parameter. A quadratic is
   raised to the cubic with the same curve. */
static int polygon_of(const SHAPE_STORE *s, UINT32 quarters, POINT *poly)
{
    UINT32 i = 1;
    int n = 0;

#define P(k, axis) pixels_of(s->points[2 * (k) + (axis)], quarters)
    poly[n].x = P(0, 0);
    poly[n++].y = P(0, 1);
    while (i < s->ends[0]) {
        if (s->flags[i] == COMPOSE_POINT_ON_CURVE) {
            poly[n].x = P(i, 0);
            poly[n++].y = P(i, 1);
            i += 1;
        } else {
            POINT p0 = poly[n - 1], c1, c2, end;
            int step;

            if (s->flags[i] == COMPOSE_POINT_CUBIC) {
                c1.x = P(i, 0);
                c1.y = P(i, 1);
                c2.x = P(i + 1, 0);
                c2.y = P(i + 1, 1);
                end.x = P(i + 2, 0);
                end.y = P(i + 2, 1);
                i += 3;
            } else {
                POINT control;

                control.x = P(i, 0);
                control.y = P(i, 1);
                end.x = P(i + 1, 0);
                end.y = P(i + 1, 1);
                c1.x = p0.x + 2 * (control.x - p0.x) / 3;
                c1.y = p0.y + 2 * (control.y - p0.y) / 3;
                c2.x = end.x + 2 * (control.x - end.x) / 3;
                c2.y = end.y + 2 * (control.y - end.y) / 3;
                i += 2;
            }
            for (step = 1; step <= CURVE_STEPS; step++) {
                double t = (double)step / CURVE_STEPS, u = 1 - t;

                poly[n].x = u * u * u * p0.x + 3 * u * u * t * c1.x +
                            3 * u * t * t * c2.x + t * t * t * end.x;
                poly[n++].y = u * u * u * p0.y + 3 * u * u * t * c1.y +
                              3 * u * t * t * c2.y + t * t * t * end.y;
            }
        }
    }
#undef P
    return n;
}

/* The area of the polygon inside the pixel square at (px, py): the polygon
   clipped against the square's four sides, one after another, and the
   shoelace sum of what remains. */
static double square_area(const POINT *poly, int n, double px, double py)
{
    static POINT a[POLY_MAX], b[POLY_MAX];
    POINT *in = a, *out = b, *swap;
    int count = n, side, i;
    double area = 0;

    memcpy(in, poly, (size_t)n * sizeof *poly);
    for (side = 0; side < 4 && count > 0; side++) {
        int kept = 0;

        for (i = 0; i < count; i++) {
            POINT p = in[i], q = in[(i + 1) % count];
            double dp, dq, t;

            switch (side) {
            case 0:
                dp = p.x - px;
                dq = q.x - px;
                break;
            case 1:
                dp = px + 1 - p.x;
                dq = px + 1 - q.x;
                break;
            case 2:
                dp = p.y - py;
                dq = q.y - py;
                break;
            default:
                dp = py + 1 - p.y;
                dq = py + 1 - q.y;
                break;
            }
            if (dp >= 0)
                out[kept++] = p;
            if ((dp >= 0) != (dq >= 0)) {
                t = dp / (dp - dq);
                out[kept].x = p.x + t * (q.x - p.x);
                out[kept++].y = p.y + t * (q.y - p.y);
            }
        }
        swap = in;
        in = out;
        out = swap;
        count = kept;
    }
    for (i = 0; i < count; i++) {
        POINT p = in[i], q = in[(i + 1) % count];

        area += p.x * q.y - q.x * p.y;
    }
    return fabs(area) / 2;
}

static int cover_of(double area)
{
    int cover = (int)floor(area * COMPOSE_COVER_MAX + 0.5);

    return cover < 0                   ? 0
           : cover > COMPOSE_COVER_MAX ? COMPOSE_COVER_MAX
                                       : cover;
}

typedef struct {
    SHAPE_STORE body, edge;
    UINT32 body_ink, edge_ink, edge_percent;
} THEME_POINTER;

static COMPOSE_POINTER pointer_of(const THEME_POINTER *theme)
{
    COMPOSE_POINTER pointer;

    pointer.body = view(&theme->body);
    pointer.edge = view(&theme->edge);
    pointer.body_ink = theme->body_ink;
    pointer.edge_ink = theme->edge_ink;
    pointer.edge_percent = theme->edge_percent;
    return pointer;
}

static int read_pointer(THEME_POINTER *theme)
{
    return read_shape("theme.pointer.body", &theme->body) &&
           read_shape("theme.pointer.edge", &theme->edge) &&
           read_default("theme.pointer.body-ink", &theme->body_ink, 16) &&
           read_default("theme.pointer.edge-ink", &theme->edge_ink, 16) &&
           read_default("theme.pointer.edge-opacity", &theme->edge_percent, 10);
}

/* A band of width x height pixels at (left, top), all of one colour. */
static UINT32 *flat_band(INT64 width, INT64 height, UINT32 colour)
{
    UINT32 *pixels = malloc((size_t)(width * height) * sizeof(UINT32));
    INT64 i;

    for (i = 0; i < width * height; i++)
        pixels[i] = colour;
    return pixels;
}

/* The arrow's box, in pixels from the hotspot, that its geometry gives:
   the edge is the body grown by one point, so it reaches a point past the
   body's extreme vertices, and the pixels it covers are those from the
   one holding its least coordinate to the one holding its greatest. */
static void geometric_box(const SHAPE_STORE *body, UINT32 quarters, RECT *box)
{
    INT64 xmin = body->points[0], xmax = xmin, ymin = body->points[1],
          ymax = ymin;
    UINT32 i;
    INT64 denominator = (INT64)COMPOSE_QUARTERS * POINT_UNITS;

    for (i = 0; i < body->point_count; i++) {
        INT64 x = body->points[2 * i], y = body->points[2 * i + 1];

        xmin = x < xmin ? x : xmin;
        xmax = x > xmax ? x : xmax;
        ymin = y < ymin ? y : ymin;
        ymax = y > ymax ? y : ymax;
    }
    box->left = floor_div((xmin - POINT_UNITS) * quarters, denominator);
    box->top = floor_div((ymin - POINT_UNITS) * quarters, denominator);
    box->right = ceil_div((xmax + POINT_UNITS) * quarters, denominator);
    box->bottom = ceil_div((ymax + POINT_UNITS) * quarters, denominator);
}

/* Draw the pointer with its hotspot at (x, y) on a flat band of the
   rectangle; returns the band's pixels. */
static UINT32 *draw(const COMPOSE_POINTER *pointer, const RECT *rect, INT64 x,
                    INT64 y, UINT32 quarters, UINT32 background, int *ok)
{
    INT64 width = rect->right - rect->left, height = rect->bottom - rect->top;
    UINT32 *pixels = flat_band(width, height, background);
    COMPOSE_BAND band = {pixels, width, *rect};

    *ok = compose_pointer(&work, pointer, &band, x, y, quarters);
    return pixels;
}

static void check_arrow_size(const THEME_POINTER *theme)
{
    COMPOSE_POINTER pointer = pointer_of(theme);
    RECT rect = {BAND_LEFT, BAND_TOP, BAND_LEFT + BAND_WIDTH,
                 BAND_TOP + BAND_HEIGHT};
    size_t t;

    for (t = 0; t < ARROW_SCALES; t++) {
        UINT32 q = arrow_table[t].quarters;
        RECT box, expected, drawn = {BAND_WIDTH, BAND_HEIGHT, 0, 0};
        INT64 x, y, outside = 0;
        int ok;
        UINT32 *pixels;

        check(compose_pointer_box(&work, &pointer, ARROW_X, ARROW_Y, q, &box),
              "the pointer's box is made", q, 0);
        geometric_box(&theme->body, q, &expected);
        check(box.right - box.left == arrow_table[t].width &&
                  box.bottom - box.top == arrow_table[t].height,
              "the pointer's box is the pixels V15's table gives", q,
              (box.right - box.left) * 1000 + (box.bottom - box.top));
        check(box.left == ARROW_X + expected.left &&
                  box.top == ARROW_Y + expected.top &&
                  box.right == ARROW_X + expected.right &&
                  box.bottom == ARROW_Y + expected.bottom,
              "the pointer's box is the one the arrow's geometry gives", q,
              box.left);

        pixels = draw(&pointer, &rect, ARROW_X, ARROW_Y, q, BACKGROUND, &ok);
        check(ok, "the pointer is drawn", q, 0);
        for (y = 0; y < BAND_HEIGHT; y++)
            for (x = 0; x < BAND_WIDTH; x++)
                if (pixels[y * BAND_WIDTH + x] != BACKGROUND) {
                    INT64 sx = BAND_LEFT + x, sy = BAND_TOP + y;

                    drawn.left = x < drawn.left ? x : drawn.left;
                    drawn.top = y < drawn.top ? y : drawn.top;
                    drawn.right = x + 1 > drawn.right ? x + 1 : drawn.right;
                    drawn.bottom = y + 1 > drawn.bottom ? y + 1 : drawn.bottom;
                    outside += sx < box.left || sx >= box.right ||
                               sy < box.top || sy >= box.bottom;
                }
        check(outside == 0, "the pointer changes no pixel outside its box", q,
              outside);
        check(drawn.right - drawn.left <= arrow_table[t].width &&
                  drawn.bottom - drawn.top <= arrow_table[t].height,
              "the pointer covers no more pixels than V15's table", q,
              (drawn.right - drawn.left) * 1000 + (drawn.bottom - drawn.top));
        printf("pointer: scale %u/4 covers %lld x %lld pixels, table %lld x "
               "%lld\n",
               q, (long long)(drawn.right - drawn.left),
               (long long)(drawn.bottom - drawn.top),
               (long long)arrow_table[t].width,
               (long long)arrow_table[t].height);
        free(pixels);
    }
}

/* The arrow's pixels are the same wherever its hotspot is, which lies on a
   pixel's top left corner, and a band that cuts the arrow shows the same
   pixels of it as a band that does not. */
static void check_arrow_translation(const THEME_POINTER *theme)
{
    COMPOSE_POINTER pointer = pointer_of(theme);
    RECT whole = {0, 0, 2 * BAND_WIDTH, 2 * BAND_HEIGHT};
    RECT small = {BAND_LEFT, BAND_TOP, BAND_LEFT + BAND_WIDTH,
                  BAND_TOP + BAND_HEIGHT};
    static const INT64 hotspots[][2] = {
        {ARROW_X, ARROW_Y},
        {BAND_LEFT + 2, BAND_TOP + 2},
        {BAND_LEFT + BAND_WIDTH - 5, BAND_TOP + BAND_HEIGHT - 7},
        {BAND_LEFT - 9, BAND_TOP + 11},
        {ARROW_X + 17, ARROW_Y - 29}};
    size_t t, h;

    for (t = 0; t < ARROW_SCALES; t++) {
        UINT32 q = arrow_table[t].quarters;
        int ok;
        UINT32 *base =
            draw(&pointer, &whole, ARROW_X, ARROW_Y, q, BACKGROUND, &ok);

        check(ok, "the pointer is drawn", q, 1);
        for (h = 0; h < sizeof hotspots / sizeof hotspots[0]; h++) {
            INT64 hx = hotspots[h][0], hy = hotspots[h][1], x, y, wrong = 0;
            UINT32 *moved = draw(&pointer, &whole, hx, hy, q, BACKGROUND, &ok);
            UINT32 *cut = draw(&pointer, &small, hx, hy, q, BACKGROUND, &ok);

            for (y = 0; y < whole.bottom; y++)
                for (x = 0; x < whole.right; x++) {
                    INT64 bx = x - (hx - ARROW_X), by = y - (hy - ARROW_Y);
                    UINT32 want = bx >= 0 && by >= 0 && bx < whole.right &&
                                          by < whole.bottom
                                      ? base[by * whole.right + bx]
                                      : BACKGROUND;

                    wrong += moved[y * whole.right + x] != want && bx >= 0 &&
                             by >= 0 && bx < whole.right && by < whole.bottom;
                }
            check(wrong == 0,
                  "the arrow's pixels are the same wherever the hotspot is", q,
                  wrong);
            wrong = 0;
            for (y = small.top; y < small.bottom; y++)
                for (x = small.left; x < small.right; x++)
                    wrong +=
                        cut[(y - small.top) * BAND_WIDTH + (x - small.left)] !=
                        moved[y * whole.right + x];
            check(wrong == 0,
                  "a band that cuts the arrow shows the arrow's pixels", q,
                  wrong);
            free(moved);
            free(cut);
        }
        free(base);
    }
}

/* The body with no edge, against the body's polygon clipped to each pixel
   in floating point: where the hotspot lies, which pixels the arrow
   covers, in what share, and that the mix is of the ink's light. */
static void check_arrow_body(const THEME_POINTER *theme)
{
    THEME_POINTER body_only = *theme;
    COMPOSE_POINTER pointer;
    RECT rect = {BAND_LEFT, BAND_TOP, BAND_LEFT + BAND_WIDTH,
                 BAND_TOP + BAND_HEIGHT};
    static POINT poly[POLY_MAX];
    size_t t;

    body_only.edge_percent = 0;
    pointer = pointer_of(&body_only);
    for (t = 0; t < ARROW_SCALES; t++) {
        UINT32 q = arrow_table[t].quarters;
        int ok, n = polygon_of(&theme->body, q, poly);
        UINT32 *pixels =
            draw(&pointer, &rect, ARROW_X, ARROW_Y, q, BACKGROUND, &ok);
        INT64 x, y, wrong = 0, covered = 0;

        check(ok, "the body is drawn", q, 0);
        for (y = rect.top; y < rect.bottom; y++)
            for (x = rect.left; x < rect.right; x++) {
                int cover = cover_of(square_area(poly, n, (double)(x - ARROW_X),
                                                 (double)(y - ARROW_Y)));
                UINT32 got =
                    pixels[(y - rect.top) * BAND_WIDTH + (x - rect.left)];
                int c, near = 0;

                covered += cover > 0;
                for (c = cover - BODY_COVER_TOLERANCE;
                     c <= cover + BODY_COVER_TOLERANCE; c++) {
                    int clamped = c < 0 ? 0 : c > 255 ? 255 : c;

                    near |=
                        got == fade(BACKGROUND, theme->body_ink, clamped, 255);
                }
                wrong += !near;
            }
        check(wrong == 0, "the body is the arrow's polygon at each pixel", q,
              wrong);
        check(covered > 0, "the body covers pixels", q, covered);
        free(pixels);
    }
}

/* The edge alone, white on black at full opacity, against the edge's
   outline cut into fine steps and clipped to each pixel: the curves'
   flattening, a sixteenth of a pixel, bounds the difference. */
static void check_arrow_edge(const THEME_POINTER *theme)
{
    THEME_POINTER edge_only = *theme;
    COMPOSE_POINTER pointer;
    RECT rect = {BAND_LEFT, BAND_TOP, BAND_LEFT + BAND_WIDTH,
                 BAND_TOP + BAND_HEIGHT};
    static POINT poly[POLY_MAX];
    size_t t;

    memset(&edge_only.body, 0, sizeof edge_only.body);
    edge_only.edge_ink = 0xFFFFFF;
    edge_only.edge_percent = COMPOSE_PERCENT;
    pointer = pointer_of(&edge_only);
    for (t = 0; t < ARROW_SCALES; t++) {
        UINT32 q = arrow_table[t].quarters;
        int ok, n = polygon_of(&theme->edge, q, poly);
        UINT32 *pixels = draw(&pointer, &rect, ARROW_X, ARROW_Y, q, 0, &ok);
        INT64 x, y, wrong = 0;

        check(ok, "the edge is drawn", q, 0);
        for (y = rect.top; y < rect.bottom; y++)
            for (x = rect.left; x < rect.right; x++) {
                int cover = cover_of(square_area(poly, n, (double)(x - ARROW_X),
                                                 (double)(y - ARROW_Y)));
                int low = cover - EDGE_COVER_TOLERANCE;
                int high = cover + EDGE_COVER_TOLERANCE;
                UINT32 got =
                    pixels[(y - rect.top) * BAND_WIDTH + (x - rect.left)];
                UINT32 least = fade(0, 0xFFFFFF, low < 0 ? 0 : low, 255);
                UINT32 most = fade(0, 0xFFFFFF, high > 255 ? 255 : high, 255);
                int k;

                for (k = 0; k < 3; k++)
                    wrong += channel(got, k) < channel(least, k) ||
                             channel(got, k) > channel(most, k);
            }
        check(wrong == 0, "the edge is the arrow's outline at each pixel", q,
              wrong);
        free(pixels);
    }
}

/* The body is drawn over the edge, and the edge at its opacity over what
   lies beneath. */
static void check_arrow_layers(const THEME_POINTER *theme)
{
    COMPOSE_POINTER pointer = pointer_of(theme);
    RECT rect = {BAND_LEFT, BAND_TOP, BAND_LEFT + BAND_WIDTH,
                 BAND_TOP + BAND_HEIGHT};
    size_t t;

    for (t = 0; t < ARROW_SCALES; t++) {
        UINT32 q = arrow_table[t].quarters;
        double scale = q / (double)COMPOSE_QUARTERS;
        int ok;
        UINT32 *pixels =
            draw(&pointer, &rect, ARROW_X, ARROW_Y, q, BACKGROUND, &ok);
        /* A pixel well inside the body, at (2.5, 8.5) points, and one in
           the ring the edge makes left of the body, at 8.5 points down. */
        INT64 inside_x = ARROW_X + (INT64)(2.5 * scale);
        INT64 row = ARROW_Y + (INT64)(8.5 * scale);
        UINT32 body =
            pixels[(row - rect.top) * BAND_WIDTH + (inside_x - rect.left)];
        UINT32 ring =
            pixels[(row - rect.top) * BAND_WIDTH + (ARROW_X - 1 - rect.left)];
        UINT32 want = fade(BACKGROUND, theme->edge_ink, theme->edge_percent,
                           COMPOSE_PERCENT);
        int k, near = 1;

        check(body == theme->body_ink,
              "the body is drawn over the edge in its own ink", q, body);
        for (k = 0; k < 3; k++)
            near &= near_code((int)channel(ring, k), (int)channel(want, k),
                              MIX_TOLERANCE);
        check(near,
              "the edge mixes its ink at its opacity over what is beneath", q,
              ring);
        free(pixels);
    }
}

/* A shape that cannot be drawn leaves nothing drawn, not even the other
   shape, and its box is refused. */
static void check_arrow_refusals(const THEME_POINTER *theme)
{
    RECT rect = {BAND_LEFT, BAND_TOP, BAND_LEFT + BAND_WIDTH,
                 BAND_TOP + BAND_HEIGHT};
    UINT32 first_cubic = 0, i;
    size_t m;
    enum {
        BAD_START,
        BAD_SHORT,
        BAD_END,
        BAD_CUBIC,
        BAD_LAST,
        BAD_RANGE,
        BAD_BODY,
        BAD_SCALE,
        BAD_HUGE,
        BAD_OVERFLOW,
        BAD_PERCENT,
        BAD_CASES
    };

    for (i = 0; i < theme->edge.point_count; i++)
        if (theme->edge.flags[i] == COMPOSE_POINT_CUBIC) {
            first_cubic = i;
            break;
        }
    for (m = 0; m < BAD_CASES; m++) {
        THEME_POINTER bad = *theme;
        UINT32 q = 9;
        RECT box = {7, 8, 9, 10}, kept = box;
        int ok_draw, ok_box;
        COMPOSE_POINTER pointer;
        UINT32 *pixels;
        INT64 changed = 0, j;

        switch (m) {
        case BAD_START:
            bad.edge.flags[0] = COMPOSE_POINT_QUADRATIC;
            break;
        case BAD_SHORT:
            bad.edge.ends[0] = 1;
            break;
        case BAD_END:
            bad.edge.ends[0] = bad.edge.point_count + 1;
            break;
        case BAD_CUBIC:
            bad.edge.flags[first_cubic + 1] = COMPOSE_POINT_ON_CURVE;
            break;
        case BAD_LAST:
            bad.edge.flags[bad.edge.ends[0] - 1] = COMPOSE_POINT_CUBIC;
            break;
        case BAD_RANGE:
            bad.edge.points[0] = OUTLINE_COORDINATE_MAX + 1;
            break;
        case BAD_BODY:
            bad.body.flags[0] = COMPOSE_POINT_CUBIC;
            break;
        case BAD_SCALE:
            q = 0;
            break;
        case BAD_HUGE:
            q = 1u << 20;
            break;
        case BAD_OVERFLOW:
            /* With the body empty, nothing but the coordinate's own scaling
               stands between this edge and a product past an INT64. */
            bad.body.contours = 0;
            bad.body.point_count = 0;
            bad.edge.points[0] = 2147483647;
            q = 4294967295u;
            break;
        default:
            bad.edge_percent = COMPOSE_PERCENT + 1;
            break;
        }
        pointer = pointer_of(&bad);
        pixels =
            draw(&pointer, &rect, ARROW_X, ARROW_Y, q, BACKGROUND, &ok_draw);
        ok_box =
            compose_pointer_box(&work, &pointer, ARROW_X, ARROW_Y, q, &box);
        for (j = 0; j < BAND_WIDTH * BAND_HEIGHT; j++)
            changed += pixels[j] != BACKGROUND;
        check(!ok_draw && changed == 0,
              "a pointer that cannot be drawn draws nothing", (long long)m,
              changed);
        check(!ok_box && box.left == kept.left && box.top == kept.top &&
                  box.right == kept.right && box.bottom == kept.bottom,
              "its box is refused and left alone", (long long)m, 0);
        free(pixels);
    }
}

/* The band's memory around its pixels is not the pointer's to change: the
   rows after the last and the stride's padding hold SENTINEL, and the
   arrow, cut by the band at its right and bottom edges, leaves them be. */
static void check_pointer_sentinels(const THEME_POINTER *theme)
{
    COMPOSE_POINTER pointer = pointer_of(theme);
    static const INT64 hotspots[][2] = {
        {ARROW_X, ARROW_Y},
        {BAND_LEFT + BAND_WIDTH - 5, BAND_TOP + BAND_HEIGHT - 7},
        {BAND_LEFT - 9, BAND_TOP - 6},
        {BAND_LEFT + 20, BAND_TOP + BAND_HEIGHT - 3}};
    INT64 stride = BAND_WIDTH + PADDING, rows = BAND_HEIGHT + PADDING;
    size_t t, h;

    for (t = 0; t < ARROW_SCALES; t++)
        for (h = 0; h < sizeof hotspots / sizeof hotspots[0]; h++) {
            UINT32 *memory = malloc((size_t)(stride * rows) * sizeof(UINT32));
            COMPOSE_BAND band = {
                memory, stride,
                {BAND_LEFT, BAND_TOP, BAND_LEFT + BAND_WIDTH,
                 BAND_TOP + BAND_HEIGHT}};
            INT64 x, y, disturbed = 0, drawn = 0;
            int ok;

            for (y = 0; y < rows; y++)
                for (x = 0; x < stride; x++)
                    memory[y * stride + x] =
                        x < BAND_WIDTH && y < BAND_HEIGHT ? BACKGROUND
                                                          : SENTINEL;
            ok = compose_pointer(&work, &pointer, &band, hotspots[h][0],
                                 hotspots[h][1], arrow_table[t].quarters);
            for (y = 0; y < rows; y++)
                for (x = 0; x < stride; x++) {
                    UINT32 pixel = memory[y * stride + x];

                    if (x < BAND_WIDTH && y < BAND_HEIGHT)
                        drawn += pixel != BACKGROUND;
                    else
                        disturbed += pixel != SENTINEL;
                }
            check(ok && drawn > 0,
                  "the pointer is drawn in a band with padding",
                  arrow_table[t].quarters, (long long)h);
            check(disturbed == 0,
                  "the pointer changes no memory outside the band's pixels",
                  arrow_table[t].quarters, disturbed);
            free(memory);
        }
}

/* The edge's opacity is 85 percent of OUTLINE_OPACITY_ONE rounded to the
   nearest (look.md V15 gives the percent; compose.h the rounding). Over
   every grey, at a coverage of 255 where the edge alone lies, the result
   is the mix of the ink at that weight, to the code: the other rounding
   gives another code for some greys, which the check shows. */
static void check_edge_opacity(void)
{
    static const INT32 strip[] = {0,
                                  0,
                                  CODES * POINT_UNITS,
                                  0,
                                  CODES * POINT_UNITS,
                                  2 * POINT_UNITS,
                                  0,
                                  2 * POINT_UNITS};
    static const UINT8 flags[] = {1, 1, 1, 1};
    static const UINT32 ends[] = {4};
    static const UINT32 inks[] = {0x000000, 0x0A0A0A, 0xFFFFFF};
    COMPOSE_SHAPE nothing = {0, 0, 0, 0, 0};
    COMPOSE_SHAPE edge = {strip, flags, ends, 4, 1};
    INT64 nearest = (INT64)floor((double)EDGE_PERCENT_TEST *
                                     OUTLINE_OPACITY_ONE / COMPOSE_PERCENT +
                                 0.5);
    INT64 lower = (INT64)EDGE_PERCENT_TEST * OUTLINE_OPACITY_ONE /
                  COMPOSE_PERCENT;
    RECT rect = {0, 0, CODES, 2};
    size_t i;

    for (i = 0; i < sizeof inks / sizeof inks[0]; i++) {
        COMPOSE_POINTER pointer = {nothing, edge, 0, inks[i],
                                   EDGE_PERCENT_TEST};
        UINT32 *pixels = malloc((size_t)(CODES * 2) * sizeof(UINT32));
        COMPOSE_BAND band = {pixels, CODES, rect};
        INT64 x, y, wrong = 0, apart = 0;
        int k;

        for (y = 0; y < 2; y++)
            for (x = 0; x < CODES; x++)
                pixels[y * CODES + x] = (UINT32)x * 0x010101u;
        check(compose_pointer(&work, &pointer, &band, 0, 0, COMPOSE_QUARTERS) ==
                  1,
              "the opacity strip is drawn", (long long)inks[i], 0);
        for (y = 0; y < 2; y++)
            for (x = 0; x < CODES; x++)
                for (k = 0; k < 3; k++) {
                    INT64 under = x, ink = channel(inks[i], k);

                    wrong += channel(pixels[y * CODES + x], k) !=
                             (UINT32)gfx_cross_fade(under, ink, nearest,
                                                    OUTLINE_OPACITY_ONE);
                    apart += y == 0 &&
                             gfx_cross_fade(under, ink, lower,
                                            OUTLINE_OPACITY_ONE) !=
                                 gfx_cross_fade(under, ink, nearest,
                                                OUTLINE_OPACITY_ONE);
                }
        check(wrong == 0,
              "the edge mixes at its opacity rounded to the nearest, to the "
              "code",
              (long long)inks[i], wrong);
        if (inks[i] == 0x000000)
            check(apart > 0, "rounding the opacity down would change a grey",
                  apart, 0);
        free(pixels);
    }
}

/* A hotspot farther than COMPOSE_COORDINATE_MAX from the origin is
   refused, nothing is drawn and the box is left alone, however far: the
   greatest coordinates themselves lie past what lib/outline.c holds, and
   are refused with nothing drawn too. */
static void check_pointer_limits(const THEME_POINTER *theme)
{
    COMPOSE_POINTER pointer = pointer_of(theme);
    RECT rect = {BAND_LEFT, BAND_TOP, BAND_LEFT + BAND_WIDTH,
                 BAND_TOP + BAND_HEIGHT};
    static const INT64 hotspots[] = {
        COMPOSE_COORDINATE_MAX,      -COMPOSE_COORDINATE_MAX,
        COMPOSE_COORDINATE_MAX + 1,  -COMPOSE_COORDINATE_MAX - 1,
        (INT64)1 << 60,              -((INT64)1 << 60)};
    size_t h;
    int axis;

    for (h = 0; h < sizeof hotspots / sizeof hotspots[0]; h++)
        for (axis = 0; axis < 2; axis++) {
            INT64 x = axis ? ARROW_X : hotspots[h];
            INT64 y = axis ? hotspots[h] : ARROW_Y;
            RECT box = {7, 8, 9, 10}, kept = box;
            INT64 changed = 0, j;
            int ok_draw, ok_box;
            UINT32 *pixels =
                draw(&pointer, &rect, x, y, COMPOSE_QUARTERS, BACKGROUND,
                     &ok_draw);

            ok_box = compose_pointer_box(&work, &pointer, x, y,
                                         COMPOSE_QUARTERS, &box);
            for (j = 0; j < BAND_WIDTH * BAND_HEIGHT; j++)
                changed += pixels[j] != BACKGROUND;
            check(!ok_draw && changed == 0,
                  "a hotspot at or past the greatest coordinate draws nothing",
                  (long long)hotspots[h], axis);
            check(!ok_box && box.left == kept.left && box.top == kept.top &&
                      box.right == kept.right && box.bottom == kept.bottom,
                  "its box is refused and left alone", (long long)hotspots[h],
                  axis);
            free(pixels);
        }
}

/* The scales a screen the compositor draws on can give: its height is at
   most LIMB_MAX_SIDE, the greatest side limb_init takes, which
   mapping_display accepts a display by, and the scale is the height over
   WINDOW_SCALE_HEIGHT, to the nearest quarter, halves up (D7, rule 5). The
   arrow is traced at the greatest, with its hotspot at the screen's last
   pixel, and at ten times it, so that the compositor, which traces it at
   the screen's scale and never meets a refusal, has room to spare. */
#define ARROW_MARGIN 10
static void check_arrow_greatest_scale(const THEME_POINTER *theme)
{
    COMPOSE_POINTER pointer = pointer_of(theme);
    UINT32 greatest =
        (UINT32)(((INT64)LIMB_MAX_SIDE * DISPLAY_SCALE_ONE +
                  WINDOW_SCALE_HEIGHT / 2) /
                 WINDOW_SCALE_HEIGHT);
    UINT32 quarters[] = {greatest, greatest * ARROW_MARGIN};
    size_t i;

    for (i = 0; i < sizeof quarters / sizeof quarters[0]; i++) {
        RECT box;

        check(compose_pointer_box(&work, &pointer, 0, 0, quarters[i], &box) &&
                  compose_pointer_box(&work, &pointer, LIMB_MAX_SIDE - 1,
                                      LIMB_MAX_SIDE - 1, quarters[i], &box),
              "the arrow is traced at the greatest scale a screen gives, and "
              "far past it, at the screen's corners",
              quarters[i], 0);
    }
}

/* The lines lib/outline.c is given, as integers: a shape's coordinates
   scaled and rounded once to the nearest 256th of a pixel, halves up, and
   moved to the hotspot. Pixel tolerances cannot see a 256th, so each line's
   ends are compared exactly. */
static void check_coordinates(void)
{
    static const INT32 triangle[] = {-2, -2, 6, -6, 2, 10};
    static const INT32 square[] = {0, 0, 1024, 0, 1024, 1024, 0, 1024};
    static const UINT8 flags[] = {1, 1, 1, 1};
    static const UINT32 ends_three[] = {3}, ends_four[] = {4};
    static const UINT32 scales[] = {4, 8, 9, 13};
    COMPOSE_SHAPE nothing = {0, 0, 0, 0, 0};
    size_t s, i;

    for (s = 0; s < sizeof scales / sizeof scales[0]; s++) {
        UINT32 q = scales[s];
        COMPOSE_SHAPE shape = {triangle, flags, ends_three, 3, 1};
        COMPOSE_POINTER pointer = {nothing, shape, 0, 0, 0};
        INT64 x = 3, y = -2, want[3][2];
        RECT box, expected;
        int ok = compose_pointer_box(&work, &pointer, x, y, q, &box);
        INT64 least_x, least_y, most_x, most_y;

        check(ok && work.count == 3, "a triangle builds three lines", q,
              work.count);
        for (i = 0; i < 3; i++) {
            want[i][0] = x * POINT_UNITS +
                         (INT64)floor((double)triangle[2 * i] * q / 4 + 0.5);
            want[i][1] =
                y * POINT_UNITS +
                (INT64)floor((double)triangle[2 * i + 1] * q / 4 + 0.5);
        }
        for (i = 0; ok && i < 3; i++) {
            const OUTLINE_EDGE *e = &work.edges[i];

            check(e->x0 == want[i][0] && e->y0 == want[i][1] &&
                      e->x1 == want[(i + 1) % 3][0] &&
                      e->y1 == want[(i + 1) % 3][1],
                  "a shape's coordinates are scaled, rounded once and moved to "
                  "the hotspot",
                  q, (long long)i);
        }
        least_x = most_x = want[0][0];
        least_y = most_y = want[0][1];
        for (i = 1; i < 3; i++) {
            least_x = want[i][0] < least_x ? want[i][0] : least_x;
            most_x = want[i][0] > most_x ? want[i][0] : most_x;
            least_y = want[i][1] < least_y ? want[i][1] : least_y;
            most_y = want[i][1] > most_y ? want[i][1] : most_y;
        }
        expected.left = floor_div(least_x, POINT_UNITS);
        expected.top = floor_div(least_y, POINT_UNITS);
        expected.right = ceil_div(most_x, POINT_UNITS);
        expected.bottom = ceil_div(most_y, POINT_UNITS);
        check(box.left == expected.left && box.top == expected.top &&
                  box.right == expected.right && box.bottom == expected.bottom,
              "a triangle's box is the pixels its lines cross", q, box.left);
    }
    {
        /* A square on pixel boundaries covers exactly its own pixels: a
           line's far end on a pixel's edge belongs to the next pixel. */
        COMPOSE_SHAPE shape = {square, flags, ends_four, 4, 1};
        COMPOSE_POINTER pointer = {nothing, shape, 0, 0, 0};
        RECT box;

        check(compose_pointer_box(&work, &pointer, 10, 20, COMPOSE_QUARTERS,
                                  &box) &&
                  box.left == 10 && box.top == 20 && box.right == 14 &&
                  box.bottom == 24,
              "a square on pixel boundaries has a box of its own pixels",
              box.right, box.bottom);
    }
    {
        /* A quadratic from (0, 0) to (4, 0) pixels with its control point
           2 pixels up and 2 across: the curve peaks half way to the
           control point, 1 pixel up, so its pixels are the row above the
           hotspot's row and the 4 across. A reading of the control point
           as a line's end, or as the first of a cubic's two, gives
           another box or none. */
        static const INT32 bow[] = {0, 0, 512, -512, 1024, 0};
        static const UINT8 bow_flags[] = {COMPOSE_POINT_ON_CURVE,
                                          COMPOSE_POINT_QUADRATIC,
                                          COMPOSE_POINT_ON_CURVE};
        COMPOSE_SHAPE shape = {bow, bow_flags, ends_three, 3, 1};
        COMPOSE_POINTER pointer = {nothing, shape, 0, 0, 0};
        RECT box;

        check(compose_pointer_box(&work, &pointer, 10, 20, COMPOSE_QUARTERS,
                                  &box) &&
                  box.left == 10 && box.top == 19 && box.right == 14 &&
                  box.bottom == 20,
              "a quadratic's curve is cut toward its control point", box.top,
              box.bottom);
    }
}

static void check_arrow(void)
{
    static THEME_POINTER theme;

    if (!read_pointer(&theme)) {
        check(0, "the description has the pointer's settings", 0, 0);
        return;
    }
    check_coordinates();
    check_edge_opacity();
    check_arrow_size(&theme);
    check_arrow_translation(&theme);
    check_arrow_body(&theme);
    check_arrow_edge(&theme);
    check_arrow_layers(&theme);
    check_arrow_refusals(&theme);
    check_pointer_sentinels(&theme);
    check_pointer_limits(&theme);
    check_arrow_greatest_scale(&theme);
}

/* ------------------------------------------------------------------------
   The pointer as a frame draws it (display.md D19 rule 7), against the
   scene: the whole frame composed, with the arrow drawn over it as one band
   of the whole screen. Where the arrow is drawn is checked above
   (check_arrow); here the frames and the damage of a move must agree with
   it. */

#define CURSOR_STEPS 90
#define CURSOR_MOVES_MAX 3    /* moves between two frames, at most */
#define CURSOR_NEAR 4         /* a move to a neighbour, in pixels, at most */
#define CURSOR_OFF_SCREEN 30  /* how far past the screen a move may go */
#define CURSOR_MOVE_KINDS 4

/* Hotspots, in pixels: the screen's corners and middle, each edge cut
   through, and well off the screen. */
static const struct {
    INT64 x, y;
} cursor_places[] = {{0, 0},
                     {30, 20},
                     {80, 50},
                     {SCENE_WIDTH - 1, SCENE_HEIGHT - 1},
                     {150, 90},
                     {-5, -8},
                     {SCENE_WIDTH - 3, -4},
                     {-3, SCENE_HEIGHT - 6},
                     {SCENE_WIDTH + CURSOR_OFF_SCREEN,
                      SCENE_HEIGHT + CURSOR_OFF_SCREEN},
                     {65, 41},
                     {17, 3}};
#define CURSOR_PLACES (sizeof cursor_places / sizeof cursor_places[0])

typedef struct {
    SCREEN screen;
    COMPOSE_WALLPAPER wallpaper;
    COMPOSE_WINDOW windows[SCENE_WINDOWS];
    COMPOSE_POINTER image;
    UINT32 quarters;
    UINT32 *all; /* the frame composed whole, 0x00RRGGBB, without the arrow */
} CURSOR_RIG;

/* The whole frame with the arrow's hotspot on the top left of pixel (x, y),
   drawn over it. The caller frees it. */
static UINT32 *with_arrow(const CURSOR_RIG *rig, INT64 x, INT64 y)
{
    size_t bytes = (size_t)SCENE_WIDTH * SCENE_HEIGHT * sizeof(UINT32);
    UINT32 *drawn = malloc(bytes);
    RECT whole = {0, 0, SCENE_WIDTH, SCENE_HEIGHT};
    COMPOSE_BAND band;

    memcpy(drawn, rig->all, bytes);
    band.pixels = drawn;
    band.stride = SCENE_WIDTH;
    band.rect = whole;
    compose_pointer(&work, &rig->image, &band, x, y, rig->quarters);
    return drawn;
}

/* A frame over each rectangle, in bands of each height, with the hotspot
   placed at each of the places, is the whole frame with the arrow drawn
   over it in the blocks the rectangle holds, and writes nothing else. */
static void check_cursor_frames(const CURSOR_RIG *rig, UINT32 *screen_memory)
{
    size_t p, r, h;

    for (p = 0; p < CURSOR_PLACES; p++) {
        COMPOSE_CURSOR cursor = {&rig->image, rig->quarters, &work, 1,
                                 cursor_places[p].x, cursor_places[p].y};
        UINT32 *drawn = with_arrow(rig, cursor.x, cursor.y);

        for (r = 0; r < FRAME_RECTS; r++)
            for (h = 0; h < FRAME_ROWS; h++) {
                INT64 wrong, stray, miscount;

                frame_difference(&rig->screen, screen_memory, &rig->wallpaper,
                                 rig->windows, &frame_rects[r], frame_rows[h],
                                 &cursor, drawn, &wrong, &stray, &miscount);
                check(wrong == 0,
                      "a frame with the pointer placed is the whole frame "
                      "with the arrow over it, in the blocks it writes",
                      (long long)(p * FRAME_RECTS + r), wrong);
                check(stray == 0, "a frame with the pointer placed writes "
                                  "nothing else",
                      (long long)(p * FRAME_RECTS + r), stray);
                check(miscount == 0,
                      "a frame with the pointer placed says it composed the "
                      "pixels of the blocks it writes",
                      (long long)(p * FRAME_RECTS + r), miscount);
            }
        free(drawn);
    }
}

/* A pointer with no place is drawn nowhere, whatever its x and y hold, and
   one whose scale the image is refused at is drawn nowhere. */
static void check_cursor_not_drawn(const CURSOR_RIG *rig, UINT32 *screen_memory)
{
    COMPOSE_CURSOR unplaced = {&rig->image, rig->quarters, &work, 0, 40, 40};
    COMPOSE_CURSOR refused = {&rig->image, 0, &work, 1, 40, 40};
    const COMPOSE_CURSOR *cursors[] = {&unplaced, &refused};
    size_t c, r, h;

    for (c = 0; c < sizeof cursors / sizeof cursors[0]; c++)
        for (r = 0; r < FRAME_RECTS; r++)
            for (h = 0; h < FRAME_ROWS; h++) {
                INT64 wrong, stray, miscount;

                frame_difference(&rig->screen, screen_memory, &rig->wallpaper,
                                 rig->windows, &frame_rects[r], frame_rows[h],
                                 cursors[c], rig->all, &wrong, &stray,
                                 &miscount);
                check(wrong == 0 && stray == 0 && miscount == 0,
                      "a pointer with no place, or one the image is refused "
                      "for, is drawn nowhere",
                      (long long)c, wrong + stray);
            }
}

/* The smallest rectangle holding both, which D19's note calls their join. */
static void join_rects(RECT *total, const RECT *rect)
{
    if (rect->left >= rect->right || rect->top >= rect->bottom)
        return;
    if (total->left >= total->right || total->top >= total->bottom) {
        *total = *rect;
        return;
    }
    total->left = rect->left < total->left ? rect->left : total->left;
    total->top = rect->top < total->top ? rect->top : total->top;
    total->right = rect->right > total->right ? rect->right : total->right;
    total->bottom = rect->bottom > total->bottom ? rect->bottom : total->bottom;
}

static INT64 block_down(INT64 pixels)
{
    return pixels / (INT64)BLOCK_PIXELS * (INT64)BLOCK_PIXELS;
}

static INT64 block_up(INT64 pixels)
{
    return block_down(pixels + (INT64)BLOCK_PIXELS - 1);
}

static int has_pixels(const RECT *rect)
{
    return rect->left < rect->right && rect->top < rect->bottom;
}

static int same_rect(const RECT *a, const RECT *b)
{
    return a->left == b->left && a->top == b->top && a->right == b->right &&
           a->bottom == b->bottom;
}

static INT64 pixels_in(const RECT *rect)
{
    return has_pixels(rect)
               ? (rect->right - rect->left) * (rect->bottom - rect->top)
               : 0;
}

/* What the pointer's hotspot at pixel (x, y) damages, from the arrow's
   geometry (geometric_box) and the output's blocks, which start every row
   of this scene: the arrow's box cut by the screen, with the columns widened
   to whole blocks. Empty where the box is off the screen. */
static RECT damage_of(const THEME_POINTER *theme, UINT32 quarters, INT64 x,
                      INT64 y)
{
    RECT box, none = {0, 0, 0, 0};
    INT64 left, right;

    geometric_box(&theme->body, quarters, &box);
    left = x + box.left < 0 ? 0 : x + box.left;
    right = x + box.right > SCENE_WIDTH ? SCENE_WIDTH : x + box.right;
    box.top = y + box.top < 0 ? 0 : y + box.top;
    box.bottom = y + box.bottom > SCENE_HEIGHT ? SCENE_HEIGHT : y + box.bottom;
    if (left >= right || box.top >= box.bottom)
        return none;
    box.left = block_down(left);
    box.right = block_up(right);
    return box;
}

/* Whether got is the damage wanted: the same rectangle, or no pixels where
   wanted has none. */
static int is_damage(const RECT *got, const RECT *wanted)
{
    if (!has_pixels(wanted))
        return !has_pixels(got);
    return same_rect(got, wanted);
}

static int holds(const RECT *rect, INT64 x, INT64 y)
{
    return x >= rect->left && x < rect->right && y >= rect->top &&
           y < rect->bottom;
}

/* Whether two rectangles within the scene share a pixel, found by trying
   every pixel of the scene and not by comparing their edges, as the
   composer does. */
static int share_pixel(const RECT *a, const RECT *b)
{
    INT64 x, y;

    for (y = 0; y < SCENE_HEIGHT; y++)
        for (x = 0; x < SCENE_WIDTH; x++)
            if (holds(a, x, y) && holds(b, x, y))
                return 1;
    return 0;
}

/* What D19's note on the pointer's two boxes has a frame compose for the
   image's old box and its new, each within the scene: each that has a
   pixel, the old first, where they share none, and their join alone where
   they share one. Returns how many. */
static UINT32 wanted_rects(const RECT *old_box, const RECT *new_box,
                           RECT wanted[COMPOSE_CURSOR_RECTS_MAX])
{
    UINT32 count = 0;

    if (share_pixel(old_box, new_box)) {
        wanted[0] = *old_box;
        join_rects(&wanted[0], new_box);
        return 1;
    }
    if (has_pixels(old_box))
        wanted[count++] = *old_box;
    if (has_pixels(new_box))
        wanted[count++] = *new_box;
    return count;
}

/* Whether the count rectangles got are the wanted_count wanted, in order. */
static int are_rects(const RECT *got, UINT32 count, const RECT *wanted,
                     UINT32 wanted_count)
{
    UINT32 i;

    if (count != wanted_count)
        return 0;
    for (i = 0; i < count; i++)
        if (!same_rect(&got[i], &wanted[i]))
            return 0;
    return 1;
}

/* The pixel a step of a walk moves the hotspot to: where it is, a
   neighbour, anywhere on the screen, or past its edges. */
static void walk_place(const COMPOSE_CURSOR *cursor, INT64 *x, INT64 *y)
{
    INT64 span = 2 * CURSOR_NEAR + 1;

    switch (random_below(CURSOR_MOVE_KINDS)) {
    case 0:
        *x = cursor->x;
        *y = cursor->y;
        break;
    case 1:
        *x = cursor->x + random_below(span) - CURSOR_NEAR;
        *y = cursor->y + random_below(span) - CURSOR_NEAR;
        break;
    case 2:
        *x = random_below(SCENE_WIDTH);
        *y = random_below(SCENE_HEIGHT);
        break;
    default:
        *x = random_below(SCENE_WIDTH + 2 * CURSOR_OFF_SCREEN) -
             CURSOR_OFF_SCREEN;
        *y = random_below(SCENE_HEIGHT + 2 * CURSOR_OFF_SCREEN) -
             CURSOR_OFF_SCREEN;
        break;
    }
}

/* The walk's own account of the screen, kept apart from the cursor and
   its damage: whether the last frame drew the image and with its hotspot
   on which pixel, and whether a move has come since that frame. */
typedef struct {
    int drawn;
    INT64 x, y;
    int moved;
} WALK_SHOWN;

typedef struct {
    INT64 state;  /* moves that left the pointer other than where moved to */
    INT64 boxes;  /* moves that recorded boxes other than the geometry's */
    INT64 still;  /* moves to the pixel the pointer is on that changed a box */
    INT64 rects;  /* frames whose rectangles are not those D19's note gives */
} WALK_FAULTS;

static int same_damage(const COMPOSE_CURSOR_DAMAGE *a,
                       const COMPOSE_CURSOR_DAMAGE *b)
{
    return a->moved == b->moved && same_rect(&a->old, &b->old) &&
           same_rect(&a->now, &b->now);
}

/* The image's box where the last frame drew it, from the geometry; no
   pixel where it drew none. */
static RECT shown_box(const CURSOR_RIG *rig, const THEME_POINTER *theme,
                      const WALK_SHOWN *shown)
{
    RECT none = {0, 0, 0, 0};

    return shown->drawn ? damage_of(theme, rig->quarters, shown->x, shown->y)
                        : none;
}

/* One move of the walk: the pointer it leaves is where it moved to, and
   the damage it records is what the header says: the old box is the
   image's where the last frame drew it, however many moves have come
   since, and the new box is the image's at the place moved to. A move to
   the pixel the pointer is on changes nothing. */
static void walk_move(const CURSOR_RIG *rig, const THEME_POINTER *theme,
                      COMPOSE_CURSOR *cursor, COMPOSE_CURSOR_DAMAGE *damage,
                      WALK_SHOWN *shown, WALK_FAULTS *faults)
{
    COMPOSE_CURSOR_DAMAGE before = *damage;
    RECT old_box, new_box;
    INT64 x, y;
    int same;

    walk_place(cursor, &x, &y);
    same = cursor->placed != 0 && x == cursor->x && y == cursor->y;
    compose_cursor_move(cursor, &rig->screen, x, y, damage);
    faults->state += cursor->placed == 0 || cursor->x != x || cursor->y != y;
    if (same) {
        faults->still += !same_damage(damage, &before);
        return;
    }
    shown->moved = 1;
    old_box = shown_box(rig, theme, shown);
    new_box = damage_of(theme, rig->quarters, x, y);
    faults->boxes += damage->moved == 0 || !is_damage(&damage->old, &old_box) ||
                     !is_damage(&damage->now, &new_box);
}

/* The pixels of the screen's memory that are not colours (0x00RRGGBB) in
   the screen's order. */
static INT64 screen_wrong(const CURSOR_RIG *rig, const UINT32 *screen_memory,
                          const UINT32 *colours)
{
    INT64 i, wrong = 0;

    for (i = 0; i < SCENE_WIDTH * SCENE_HEIGHT; i++)
        wrong += screen_memory[i] != gfx_pixel(&rig->screen,
                                               colours[i] >> 16 & CHANNEL_MASK,
                                               colours[i] >> 8 & CHANNEL_MASK,
                                               colours[i] & CHANNEL_MASK);
    return wrong;
}

/* A pointer that moves, and a frame composed over only the rectangles its
   moves give, leave the screen as the whole frame composed with the arrow
   where the pointer is: after every step of CURSOR_STEPS, each of one to
   CURSOR_MOVES_MAX moves between two frames, to the pixel it is on, a
   neighbour, anywhere and off the screen. The moves record what the
   geometry says, and the rectangles are those D19's note gives for the
   box where the last frame drew the image and the box where the pointer
   is, whatever the places between. Where there is nothing to compose the
   compositor composes no frame for the pointer, and the damage is kept or,
   as a frame for other damage would, cleared. */
static void check_cursor_walk(const CURSOR_RIG *rig, const THEME_POINTER *theme,
                              UINT32 *screen_memory)
{
    static const COMPOSE_CURSOR_DAMAGE no_damage = {0, {0, 0, 0, 0},
                                                    {0, 0, 0, 0}};
    COMPOSE_CURSOR cursor = {&rig->image, rig->quarters, &work, 0, 0, 0};
    COMPOSE_CURSOR_DAMAGE damage = no_damage;
    WALK_SHOWN shown = {0, 0, 0, 0};
    RECT whole = {0, 0, SCENE_WIDTH, SCENE_HEIGHT};
    UINT32 *memory = malloc((size_t)SCENE_WIDTH * (SCENE_HEIGHT + 1) *
                            sizeof(UINT32));
    WALK_FAULTS faults = {0, 0, 0, 0};
    INT64 step, i;

    for (i = 0; i < SCENE_WIDTH * SCENE_HEIGHT; i++)
        screen_memory[i] = SCREEN_BEFORE;
    compose_frame(&rig->screen, &rig->wallpaper, rig->windows, SCENE_WINDOWS,
                  &whole, memory, SCENE_HEIGHT, &cursor);
    check(screen_wrong(rig, screen_memory, rig->all) == 0,
          "a screen composed before the pointer has a place shows no "
          "pointer",
          rig->quarters, 0);
    for (step = 0; step < CURSOR_STEPS; step++) {
        RECT rects[COMPOSE_CURSOR_RECTS_MAX], wanted[COMPOSE_CURSOR_RECTS_MAX];
        RECT none = {0, 0, 0, 0}, old_box = none, new_box = none;
        INT64 moves = 1 + random_below(CURSOR_MOVES_MAX), m;
        UINT32 count, wanted_count, r;
        UINT32 *drawn;

        for (m = 0; m < moves; m++)
            walk_move(rig, theme, &cursor, &damage, &shown, &faults);
        if (shown.moved) {
            old_box = shown_box(rig, theme, &shown);
            new_box = damage_of(theme, rig->quarters, cursor.x, cursor.y);
        }
        wanted_count = wanted_rects(&old_box, &new_box, wanted);
        count = compose_cursor_rects(&damage, rects);
        faults.rects += !are_rects(rects, count, wanted, wanted_count);
        for (r = 0; r < count && r < COMPOSE_CURSOR_RECTS_MAX; r++)
            compose_frame(&rig->screen, &rig->wallpaper, rig->windows,
                          SCENE_WINDOWS, &rects[r], memory,
                          frame_rows[step % (INT64)FRAME_ROWS], &cursor);
        drawn = with_arrow(rig, cursor.x, cursor.y);
        check(screen_wrong(rig, screen_memory, drawn) == 0,
              "frames over what the pointer's moves damage leave the screen "
              "as the whole frame with the arrow where it is",
              rig->quarters, step);
        free(drawn);
        if (count != 0 || random_below(2)) {
            damage = no_damage;
            shown.drawn = 1;
            shown.x = cursor.x;
            shown.y = cursor.y;
            shown.moved = 0;
        }
    }
    check(faults.state == 0, "a move leaves the pointer on the pixel moved to",
          rig->quarters, faults.state);
    check(faults.boxes == 0,
          "a move records the arrow's box where the last frame drew it and "
          "at the new place, cut by the screen and in whole blocks",
          rig->quarters, faults.boxes);
    check(faults.still == 0,
          "a move to the pixel the pointer is on records nothing",
          rig->quarters, faults.still);
    check(faults.rects == 0,
          "a frame composes the pointer's old box and its new apart, or "
          "their join where they share a pixel",
          rig->quarters, faults.rects);
    free(memory);
}

/* ------------------------------------------------------------------------
   What a frame composes for the pointer's moves (display.md D9's row for
   the pointer and D19's note on the pointer's two boxes). */

#define CURSOR_PAIRS 60 /* moves between two places anywhere, each checked */

typedef struct {
    INT64 x, y;
} PLACE;

/* What a frame after some moves composed, as moves_compose found it. */
typedef struct {
    UINT32 count;  /* the rectangles compose_cursor_rects gave */
    RECT old_box;  /* the image's box before the moves, from the geometry */
    RECT new_box;  /* its box after them */
    INT64 written; /* the pixels of the screen the frame wrote */
    INT64 counted; /* the pixels the frame said it composed */
    int right;     /* nonzero if everything checked here held */
} COMPOSED;

/* A pointer the last frame drew at from, or with no place where from is 0,
   moves to each of the count places of to in turn, and a frame is then
   composed over the rectangles compose_cursor_rects gives, in bands of rows
   rows, on a screen that held SCREEN_BEFORE. The rectangles must be those
   D19's note gives for the image's box at from and its box at the last
   place, whatever the places between; every pixel in one of them must be
   the whole frame's with the arrow at the last place; every other pixel
   must be untouched, the pixels between two boxes and those at the places
   between among them; and the frame must say it composed as many pixels
   as it wrote. */
static COMPOSED moves_compose(const CURSOR_RIG *rig, const THEME_POINTER *theme,
                              UINT32 *screen_memory, const PLACE *from,
                              const PLACE *to, UINT32 count, INT64 rows)
{
    COMPOSE_CURSOR cursor = {&rig->image, rig->quarters, &work, from != 0,
                             from ? from->x : 0, from ? from->y : 0};
    COMPOSE_CURSOR_DAMAGE damage = {0, {0, 0, 0, 0}, {0, 0, 0, 0}};
    RECT rects[COMPOSE_CURSOR_RECTS_MAX], wanted[COMPOSE_CURSOR_RECTS_MAX];
    RECT none = {0, 0, 0, 0};
    UINT32 *memory = malloc((size_t)SCENE_WIDTH * (SCENE_HEIGHT + 1) *
                            sizeof(UINT32));
    UINT32 *drawn = with_arrow(rig, to[count - 1].x, to[count - 1].y);
    UINT32 wanted_count, i;
    INT64 x, y, wrong = 0, stray = 0;
    COMPOSED composed;

    for (i = 0; i < count; i++)
        compose_cursor_move(&cursor, &rig->screen, to[i].x, to[i].y, &damage);
    composed.old_box =
        from ? damage_of(theme, rig->quarters, from->x, from->y) : none;
    composed.new_box =
        damage_of(theme, rig->quarters, to[count - 1].x, to[count - 1].y);
    wanted_count = wanted_rects(&composed.old_box, &composed.new_box, wanted);
    composed.count = compose_cursor_rects(&damage, rects);
    for (x = 0; x < SCENE_WIDTH * SCENE_HEIGHT; x++)
        screen_memory[x] = SCREEN_BEFORE;
    composed.counted = 0;
    for (i = 0; i < composed.count && i < COMPOSE_CURSOR_RECTS_MAX; i++)
        composed.counted += (INT64)compose_frame(
            &rig->screen, &rig->wallpaper, rig->windows, SCENE_WINDOWS,
            &rects[i], memory, rows, &cursor);
    composed.written = 0;
    for (y = 0; y < SCENE_HEIGHT; y++)
        for (x = 0; x < SCENE_WIDTH; x++) {
            UINT32 got = screen_memory[y * SCENE_WIDTH + x];
            UINT32 colour = drawn[y * SCENE_WIDTH + x];
            int inside = 0;

            for (i = 0; i < wanted_count; i++)
                inside |= holds(&wanted[i], x, y);
            composed.written += got != SCREEN_BEFORE;
            if (inside)
                wrong += got != gfx_pixel(&rig->screen,
                                          colour >> 16 & CHANNEL_MASK,
                                          colour >> 8 & CHANNEL_MASK,
                                          colour & CHANNEL_MASK);
            else
                stray += got != SCREEN_BEFORE;
        }
    composed.right = are_rects(rects, composed.count, wanted, wanted_count) &&
                     wrong == 0 && stray == 0 &&
                     composed.counted == composed.written;
    free(drawn);
    free(memory);
    return composed;
}

/* A move across the screen, longer than the image, composes the image's
   old box and its new and nothing between them (D19's note), and the
   pixels composed are D9's row: two images of the size V15's table gives
   at the scale, each widened to whole blocks. At the scale of a
   2880 x 1800 screen the frame says it composed the pixels of rule 8's
   example line: both of this move's boxes start a pixel into a block, as
   two boxes on that screen can. */
static void check_cursor_long_move(const CURSOR_RIG *rig,
                                   const THEME_POINTER *theme,
                                   UINT32 *screen_memory, INT64 image_width,
                                   INT64 image_height)
{
    RECT box, between;
    PLACE from, to;
    COMPOSED composed;
    INT64 two_boxes, widest = image_width + 2 * ((INT64)BLOCK_PIXELS - 1);
    size_t h;

    /* The image lies whole on the screen at both places: a pixel in from
       the top left corner and a pixel in from the bottom right. */
    geometric_box(&theme->body, rig->quarters, &box);
    from.x = 1 - box.left;
    from.y = 1 - box.top;
    to.x = SCENE_WIDTH - 1 - box.right;
    to.y = SCENE_HEIGHT - 1 - box.bottom;
    for (h = 0; h < FRAME_ROWS; h++) {
        composed = moves_compose(rig, theme, screen_memory, &from, &to, 1,
                                 frame_rows[h]);
        two_boxes = pixels_in(&composed.old_box) +
                    pixels_in(&composed.new_box);
        between = composed.old_box;
        join_rects(&between, &composed.new_box);
        check(composed.right && composed.count == 2,
              "a move across the screen composes the image's old box and "
              "its new, each apart",
              rig->quarters, composed.count);
        check(composed.written == two_boxes &&
                  two_boxes < pixels_in(&between),
              "a move across the screen composes nothing between the "
              "image's two boxes",
              composed.written, pixels_in(&between));
        check(two_boxes >= 2 * image_width * image_height &&
                  two_boxes <= 2 * widest * image_height &&
                  composed.old_box.bottom - composed.old_box.top ==
                      image_height &&
                  composed.new_box.bottom - composed.new_box.top ==
                      image_height,
              "a move across the screen composes two images, each in whole "
              "blocks, as D9's row counts",
              two_boxes, 2 * image_width * image_height);
        check(rig->quarters != EXAMPLE_QUARTERS ||
                  composed.counted == example_pixels,
              "a move across the screen at the scale of 2880 x 1800 says it "
              "composed the pixels of rule 8's example line",
              composed.counted, example_pixels);
    }
}

/* Moves along a row from one place: every one whose box shares a pixel
   with the first's composes the join of the two, a rectangle that holds
   both, and every one past them composes the two apart, the first of
   those, whose box touches the first's and shares no pixel with it,
   among them. */
static void check_cursor_near_moves(const CURSOR_RIG *rig,
                                    const THEME_POINTER *theme,
                                    UINT32 *screen_memory)
{
    static const PLACE from = {20, 30};
    INT64 joined = 0, apart = 0, wrong = 0, touching = 0;
    PLACE to = from;

    for (to.x = from.x + 1; to.x < SCENE_WIDTH - (INT64)BLOCK_PIXELS; to.x++) {
        COMPOSED composed = moves_compose(rig, theme, screen_memory, &from,
                                          &to, 1, frame_rows[2]);
        RECT join = composed.old_box;

        join_rects(&join, &composed.new_box);
        if (share_pixel(&composed.old_box, &composed.new_box)) {
            joined++;
            wrong += !composed.right || composed.count != 1 ||
                     composed.written != pixels_in(&join);
        } else {
            apart++;
            touching += composed.old_box.right == composed.new_box.left;
            wrong += !composed.right || composed.count != 2 ||
                     composed.written != pixels_in(&composed.old_box) +
                                             pixels_in(&composed.new_box);
        }
    }
    check(joined > 0 && apart > 0 && touching > 0,
          "the moves along a row reach boxes that share pixels, boxes that "
          "touch and boxes apart",
          joined, apart);
    check(wrong == 0,
          "boxes that share a pixel compose their join, and boxes that "
          "share none, touching or not, compose apart",
          rig->quarters, wrong);
}

/* A move down and across, shorter than the image, composes the join of
   its two boxes: one rectangle, which holds pixels of neither box. */
static void check_cursor_diagonal_move(const CURSOR_RIG *rig,
                                       const THEME_POINTER *theme,
                                       UINT32 *screen_memory)
{
    static const PLACE from = {40, 20};
    PLACE to;
    COMPOSED composed;
    RECT join;

    to.x = from.x + (INT64)BLOCK_PIXELS;
    to.y = from.y + 9;
    composed = moves_compose(rig, theme, screen_memory, &from, &to, 1,
                             frame_rows[2]);
    join = composed.old_box;
    join_rects(&join, &composed.new_box);
    check(composed.right && composed.count == 1 &&
              composed.written == pixels_in(&join),
          "a move whose boxes share a pixel composes their join",
          rig->quarters, composed.count);
    /* The pixel in the new box's last column and the old box's first row. */
    check(holds(&join, composed.new_box.right - 1, composed.old_box.top) &&
              !holds(&composed.old_box, composed.new_box.right - 1,
                     composed.old_box.top) &&
              !holds(&composed.new_box, composed.new_box.right - 1,
                     composed.old_box.top),
          "the join of a move down and across holds a pixel of neither box",
          pixels_in(&join), pixels_in(&composed.old_box));
}

/* Moves between two frames compose the image's box where the last frame
   drew it and its box where the pointer ends, and nothing at the places
   between, which no frame drew it at; a pointer given its place composes
   its one box; and one that ends where it began composes that box alone. */
static void check_cursor_moves_between(const CURSOR_RIG *rig,
                                       const THEME_POINTER *theme,
                                       UINT32 *screen_memory)
{
    static const PLACE from = {8, 6};
    static const PLACE through[] = {{70, 45}, {130, 8}};
    static const PLACE back[] = {{130, 8}, {8, 6}};
    COMPOSED composed;
    RECT between;

    composed = moves_compose(rig, theme, screen_memory, &from, through, 2,
                             frame_rows[2]);
    between = damage_of(theme, rig->quarters, through[0].x, through[0].y);
    check(composed.right && composed.count == 2 &&
              composed.written == pixels_in(&composed.old_box) +
                                      pixels_in(&composed.new_box) &&
              !share_pixel(&between, &composed.old_box) &&
              !share_pixel(&between, &composed.new_box),
          "two moves between two frames compose the box the screen shows "
          "and the last, and nothing at the place between",
          rig->quarters, composed.count);
    composed = moves_compose(rig, theme, screen_memory, 0, through, 1,
                             frame_rows[2]);
    check(composed.right && composed.count == 1 &&
              composed.written == pixels_in(&composed.new_box),
          "a pointer given its first place composes the image's one box",
          rig->quarters, composed.count);
    composed = moves_compose(rig, theme, screen_memory, 0, through, 2,
                             frame_rows[2]);
    check(composed.right && composed.count == 1 &&
              composed.written == pixels_in(&composed.new_box),
          "a pointer moved again before its first frame composes the box "
          "at its last place alone",
          rig->quarters, composed.count);
    composed = moves_compose(rig, theme, screen_memory, &from, back, 2,
                             frame_rows[2]);
    check(composed.right && composed.count == 1 &&
              composed.written == pixels_in(&composed.old_box),
          "a pointer that ends where the last frame drew it composes that "
          "box alone",
          rig->quarters, composed.count);
}

/* Moves between two places anywhere, on the screen and past its edges,
   where a box is cut by the screen or has no pixel: the rectangles and the
   pixels a frame writes are those D19's note gives. */
static void check_cursor_pairs(const CURSOR_RIG *rig,
                               const THEME_POINTER *theme,
                               UINT32 *screen_memory)
{
    INT64 pair, wrong = 0, none = 0;

    for (pair = 0; pair < CURSOR_PAIRS; pair++) {
        PLACE from, to;
        COMPOSED composed;

        from.x = random_below(SCENE_WIDTH + 2 * CURSOR_OFF_SCREEN) -
                 CURSOR_OFF_SCREEN;
        from.y = random_below(SCENE_HEIGHT + 2 * CURSOR_OFF_SCREEN) -
                 CURSOR_OFF_SCREEN;
        to.x = random_below(SCENE_WIDTH + 2 * CURSOR_OFF_SCREEN) -
               CURSOR_OFF_SCREEN;
        to.y = random_below(SCENE_HEIGHT + 2 * CURSOR_OFF_SCREEN) -
               CURSOR_OFF_SCREEN;
        composed = moves_compose(rig, theme, screen_memory, &from, &to, 1,
                                 frame_rows[pair % (INT64)FRAME_ROWS]);
        wrong += !composed.right;
        none += composed.count == 0;
    }
    check(wrong == 0,
          "a move between any two places composes the rectangles its boxes "
          "give and nothing else",
          rig->quarters, wrong);
    check(none < CURSOR_PAIRS, "the moves between places compose something",
          rig->quarters, none);
}

/* A move the image is refused at records nothing and leaves the pointer
   where it was. */
static void check_cursor_refusals(const CURSOR_RIG *rig,
                                  const THEME_POINTER *theme)
{
    static const COMPOSE_CURSOR_DAMAGE untouched = {0, {7, 8, 9, 10},
                                                    {7, 8, 9, 10}};
    COMPOSE_POINTER bad = pointer_of(theme);
    COMPOSE_CURSOR cursor = {&rig->image, rig->quarters, &work, 1, 30, 20};
    COMPOSE_CURSOR no_scale = {&rig->image, 0, &work, 1, 30, 20};
    COMPOSE_CURSOR no_image = {&bad, rig->quarters, &work, 0, 0, 0};
    COMPOSE_CURSOR_DAMAGE damage = untouched;

    compose_cursor_move(&cursor, &rig->screen, COMPOSE_COORDINATE_MAX, 5,
                        &damage);
    check(cursor.placed && cursor.x == 30 && cursor.y == 20 &&
              same_damage(&damage, &untouched),
          "a move to a hotspot the arrow cannot be traced at is refused and "
          "leaves the pointer",
          rig->quarters, 0);
    compose_cursor_move(&no_scale, &rig->screen, 31, 20, &damage);
    check(no_scale.x == 30 && no_scale.y == 20 &&
              same_damage(&damage, &untouched),
          "a move at a scale of 0 is refused and leaves the pointer",
          rig->quarters, 0);
    bad.edge_percent = COMPOSE_PERCENT + 1;
    compose_cursor_move(&no_image, &rig->screen, 31, 20, &damage);
    check(no_image.placed == 0 && same_damage(&damage, &untouched),
          "a move of an image that is refused leaves the pointer with no "
          "place",
          rig->quarters, 0);
}

/* The frames and the moves of the pointer, in each pixel format and at each
   of V15's scales. */
static void check_cursor(const SCENE *scene)
{
    static THEME_POINTER theme;
    size_t f, q;

    if (!read_pointer(&theme)) {
        check(0, "the description has the pointer's settings", 0, 0);
        return;
    }
    for (f = 0; f < FORMATS; f++) {
        UINT32 *image = kernel_image(scene->canonical, SCENE_WIDTH,
                                     SCENE_HEIGHT, formats[f]);
        UINT32 *screen_memory = aligned_alloc(
            OUTPUT_BLOCK_BYTES,
            (size_t)SCENE_WIDTH * SCENE_HEIGHT * sizeof(UINT32));

        for (q = 0; q < ARROW_SCALES; q++) {
            static CURSOR_RIG rig;
            RECT whole = {0, 0, SCENE_WIDTH, SCENE_HEIGHT};

            windows_of(scene, rig.windows);
            rig.screen.pixels = screen_memory;
            rig.screen.width = SCENE_WIDTH;
            rig.screen.height = SCENE_HEIGHT;
            rig.screen.stride = SCENE_WIDTH;
            rig.screen.format = formats[f];
            rig.wallpaper.pixels = image;
            rig.wallpaper.stride = SCENE_WIDTH;
            rig.wallpaper.format = formats[f];
            rig.image = pointer_of(&theme);
            rig.quarters = arrow_table[q].quarters;
            rig.all = compose_rect(&rig.wallpaper, rig.windows, &whole);
            check_cursor_frames(&rig, screen_memory);
            check_cursor_not_drawn(&rig, screen_memory);
            check_cursor_walk(&rig, &theme, screen_memory);
            check_cursor_long_move(&rig, &theme, screen_memory,
                                   arrow_table[q].width,
                                   arrow_table[q].height);
            check_cursor_near_moves(&rig, &theme, screen_memory);
            check_cursor_diagonal_move(&rig, &theme, screen_memory);
            check_cursor_moves_between(&rig, &theme, screen_memory);
            check_cursor_pairs(&rig, &theme, screen_memory);
            check_cursor_refusals(&rig, &theme);
            free(rig.all);
        }
        free(screen_memory);
        free(image);
    }
}

/* ------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    size_t i;

    if (!read_description(argc > 1 ? argv[1] : "interfaces/display.desc")) {
        printf("cannot read the interface description\n");
        return 2;
    }
    if (!read_example(argc > 2 ? argv[2] : "docs/spec/display.md")) {
        printf("cannot read rule 8's example line from the specification\n");
        return 2;
    }
    make_light();
    check_mix();
    check_materials();
    check_covers();
    check_peak_materials();
    check_buffer_sizes();
    check_band_limits();
    check_corners();
    {
        static SCENE scene;

        make_scene(&scene);
        check_frames(&scene);
        check_frame_rects(&scene);
        check_cursor(&scene);
        free_scene(&scene);
    }
    check_blocks();
    check_write_cuts();
    check_wallpaper_refusals();
    {
        /* A width whose last column starts a block of 16 pixels. */
        static const INT64 narrow_width = 33, narrow_height = 20;
        UINT32 codes[33 * 20];

        limb_codes(narrow_width, narrow_height, codes);
        check_wallpaper_alone(narrow_width, narrow_height, codes);
    }
    for (i = 0; i < SIZES; i++) {
        UINT32 *codes =
            malloc((size_t)(sizes[i].width * sizes[i].height) * sizeof(UINT32));

        limb_codes(sizes[i].width, sizes[i].height, codes);
        check_wallpaper_alone(sizes[i].width, sizes[i].height, codes);
        check_panel(sizes[i].width, sizes[i].height, codes);
        check_peak(sizes[i].width, sizes[i].height, codes);
        free(codes);
    }
    check_arrow();
    printf("%llu checks, %u failed\n", checks, failures);
    free(desc_text);
    return failures != 0;
}
