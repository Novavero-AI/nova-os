/* The composer (compose.h): display.md D7 rule 2, D8 and D10 rule 7, and
   D19's phase 3 arrangement, rules 1, 4 and 7.

   A band is composed row by row, and within a row each window in turn,
   which is each pixel's order from the wallpaper up, since no pixel
   reads another. Every mix is gfx_cross_fade, which decodes the codes to
   light, mixes the light and encodes the result, so that a mix of
   neighbours is the one the console's panel and the boot fade make
   (gfx/raster.c, console/panel.c). A window's colour of cover 255 is
   copied and a cover of 0 is the material's lookups, which are what a
   mix of those weights gives and cost no decode.

   A window's corners are the coverage of a square of twice the corner's
   reach, the radius rounded up, drawn as a rounded rectangle of that
   radius. Its four quadrants are the four corners of any window at
   least that big, each byte what outline_coverage gives that pixel of
   the window's own rounded rectangle (tools/check-compose.c checks
   it). A window's corner is the square's moved by whole pixels, and a
   curve's points are rounded sums of weights that add up to one, which
   a move by whole pixels shifts by exactly that. A pixel's coverage
   depends on every line crossing its row, as outline.c cuts a row at
   every line's ends, and a corner's rows hold, besides its own lines,
   the other corner on the same edge: in the square as in the window,
   and in no other row. A square of twice the radius itself would hold
   both arcs of a fractional radius in the column where they meet, where
   a larger window has the straight edge. Each square is made once per
   radius, and a window of another size reads it by offset.

   The pointer's image is drawn over each finished band that holds a pixel
   of its box, whole and cut by the band, so a screen composed in bands of
   any height is the screen composed whole with the image drawn over it.
   Moving the pointer damages the image's box where the last frame drew it
   and its box where the pointer is now, each in whole blocks, which is
   every pixel a frame must write for the image to be at its new place and
   at no other, since no frame drew it at the places of the moves between.
   A frame composes the two apart where they share no pixel, so that a
   move across the screen composes two images and nothing between them,
   and their join where they share one (tools/check-compose.c checks
   each). */

#include "compose.h"
#include "limb.h"
#include "raster.h"

#define CHANNEL_BITS 8
#define CHANNEL_MASK 0xFFu

/* A pixel's bits below channel k's, in the 0x00RRGGBB order. */
static int channel_shift(int k)
{
    return (COMPOSE_CHANNELS - 1 - k) * CHANNEL_BITS;
}

static UINT32 channel(UINT32 pixel, int k)
{
    return pixel >> channel_shift(k) & CHANNEL_MASK;
}

static INT64 smaller(INT64 a, INT64 b) { return a < b ? a : b; }

static INT64 larger(INT64 a, INT64 b) { return a > b ? a : b; }

/* a / b rounded toward minus infinity, for b > 0. */
static INT64 floor_div(INT64 a, INT64 b)
{
    INT64 q = a / b;

    return q * b > a ? q - 1 : q;
}

/* ------------------------------------------------------------------------
   Pixels */

/* The pixel of the wallpaper's image at (x, y) as 0x00RRGGBB. gfx_pixel
   puts red in the highest byte of a PIXEL_BGR pixel and in the lowest of
   a PIXEL_RGB one. */
static UINT32 wallpaper_colour(const COMPOSE_WALLPAPER *wallpaper, INT64 x,
                               INT64 y)
{
    UINT32 pixel = wallpaper->pixels[y * wallpaper->stride + x];

    if (wallpaper->format == PIXEL_RGB)
        return (pixel & CHANNEL_MASK) << channel_shift(0) |
               (pixel >> channel_shift(1) & CHANNEL_MASK)
                   << channel_shift(1) |
               (pixel >> channel_shift(0) & CHANNEL_MASK);
    return pixel & COMPOSE_COLOUR_MASK;
}

/* from mixed toward to, to weighted by weight of COMPOSE_COVER_MAX, in
   linear light, channel by channel. */
static UINT32 mix_pixels(UINT32 from, UINT32 to, INT64 weight)
{
    UINT32 out = 0;
    int k;

    for (k = 0; k < COMPOSE_CHANNELS; k++)
        out |= (UINT32)gfx_cross_fade(channel(from, k), channel(to, k),
                                      weight, COMPOSE_COVER_MAX)
               << channel_shift(k);
    return out;
}

/* What the material makes of the wallpaper's colour. */
static UINT32 through(const COMPOSE_MATERIAL *material, UINT32 colour)
{
    UINT32 out = 0;
    int k;

    for (k = 0; k < COMPOSE_CHANNELS; k++)
        out |= (UINT32)material->code[k][channel(colour, k)]
               << channel_shift(k);
    return out;
}

int compose_material(COMPOSE_MATERIAL *material, UINT32 tint, UINT32 amount)
{
    int k, code;

    if (amount > COMPOSE_PERCENT)
        return 0;
    for (k = 0; k < COMPOSE_CHANNELS; k++)
        for (code = 0; code < COMPOSE_CODES; code++)
            material->code[k][code] = (UINT8)gfx_cross_fade(
                code, channel(tint, k), amount, COMPOSE_PERCENT);
    return 1;
}

/* ------------------------------------------------------------------------
   Bands */

/* The window's pixel at (dx, dy) in it, as it shows where nothing is
   beneath but the wallpaper, whose colour there is under. Past the
   buffer the pixel is 0, a cover of 0, which is the material alone
   (display.md D7 rule 4). */
static UINT32 shown(const COMPOSE_WINDOW *window, INT64 dx, INT64 dy,
                    const COMPOSE_WALLPAPER *wallpaper, INT64 x, INT64 y)
{
    UINT32 pixel = 0;
    UINT32 cover, colour, behind;

    if (dx < window->columns && dy < window->rows)
        pixel = window->pixels[dy * window->stride + dx];
    cover = pixel >> COMPOSE_COVER_SHIFT;
    colour = pixel & COMPOSE_COLOUR_MASK;
    if (cover == COMPOSE_COVER_MAX)
        return colour;
    behind = through(window->material, wallpaper_colour(wallpaper, x, y));
    return cover == 0 ? behind : mix_pixels(behind, colour, cover);
}

/* Where along an edge size pixels long the pixel d lies in the corners'
   plane of 2 x reach pixels: its own place within reach of the near
   end, the far end's within reach of the far one, or -1 between, where
   no corner reaches. */
static INT64 plane_place(INT64 d, INT64 size, INT64 reach)
{
    if (d < reach)
        return d;
    if (d >= size - reach)
        return d - (size - 2 * reach);
    return -1;
}

/* The coverage at column dx of a row that lies at down in the corners'
   plane, or at -1 between the corners' rows. */
static UINT32 corner_cover(const COMPOSE_CORNERS *corners, INT64 width,
                           INT64 dx, INT64 down)
{
    INT64 across;

    if (down < 0)
        return COMPOSE_COVER_MAX;
    across = plane_place(dx, width, corners->reach);
    if (across < 0)
        return COMPOSE_COVER_MAX;
    return corners->plane[down * 2 * corners->reach + across];
}

UINT32 compose_corner(const COMPOSE_CORNERS *corners, INT64 width,
                      INT64 height, INT64 dx, INT64 dy)
{
    if (!corners)
        return COMPOSE_COVER_MAX;
    return corner_cover(corners, width, dx,
                        plane_place(dy, height, corners->reach));
}

int compose_corners(OUTLINE *work, COMPOSE_CORNERS *corners, INT64 radius)
{
    INT64 reach, side;

    if (radius < 0 ||
        radius > COMPOSE_REACH_MAX * (INT64)OUTLINE_ONE)
        return 0;
    reach = (radius + OUTLINE_ONE - 1) / OUTLINE_ONE;
    side = 2 * reach;
    outline_start(work);
    outline_rounded_rect(work, 0, 0, side * OUTLINE_ONE, side * OUTLINE_ONE,
                         radius);
    if (!outline_coverage(work, corners->plane, side, side))
        return 0;
    corners->reach = reach;
    return 1;
}

/* Compose the window into row y of the band, whose pixels in that row
   start at out. */
static void window_row(UINT32 *out, const COMPOSE_BAND *band, INT64 y,
                       const COMPOSE_WALLPAPER *wallpaper,
                       const COMPOSE_WINDOW *window)
{
    INT64 from = larger(band->rect.left, window->place.left);
    INT64 to = smaller(band->rect.right, window->place.right);
    INT64 dy = y - window->place.top;
    INT64 width = window->place.right - window->place.left;
    INT64 height = window->place.bottom - window->place.top;
    INT64 down, x;

    if (y < window->place.top || y >= window->place.bottom)
        return;
    /* Most rows lie between the corners' rows, where no pixel needs the
       plane. */
    down = window->corners
               ? plane_place(dy, height, window->corners->reach)
               : -1;
    for (x = from; x < to; x++) {
        INT64 dx = x - window->place.left;
        UINT32 cover = down < 0 ? COMPOSE_COVER_MAX
                                : corner_cover(window->corners, width, dx,
                                               down);
        UINT32 *pixel = out + (x - band->rect.left);

        if (cover == 0)
            continue;
        if (cover == COMPOSE_COVER_MAX)
            *pixel = shown(window, dx, dy, wallpaper, x, y);
        else
            *pixel = mix_pixels(*pixel,
                                shown(window, dx, dy, wallpaper, x, y), cover);
    }
}

void compose_band(const COMPOSE_BAND *band, const COMPOSE_WALLPAPER *wallpaper,
                  const COMPOSE_WINDOW *windows, UINT32 count)
{
    INT64 y, x;
    UINT32 i;

    for (y = band->rect.top; y < band->rect.bottom; y++) {
        UINT32 *row = band->pixels + (y - band->rect.top) * band->stride;

        for (x = band->rect.left; x < band->rect.right; x++)
            row[x - band->rect.left] = wallpaper_colour(wallpaper, x, y);
        for (i = 0; i < count; i++)
            window_row(row, band, y, wallpaper, &windows[i]);
    }
}

/* ------------------------------------------------------------------------
   Output */

/* rect cut to the screen. */
static RECT on_screen(const SCREEN *screen, const RECT *rect)
{
    RECT cut;

    cut.left = larger(rect->left, 0);
    cut.top = larger(rect->top, 0);
    cut.right = smaller(rect->right, screen->width);
    cut.bottom = smaller(rect->bottom, screen->height);
    return cut;
}

/* For row y of the screen, the pixels [*first, *last) in the blocks that
   hold pixels left to right - 1 of it, cut to the screen's row. A block
   is COMPOSE_BLOCK_BYTES from an address that is a multiple of it, and
   the framebuffer's pixels are 4-byte aligned, so every pixel lies in
   one block. The row is on the screen and left < right <= the width. */
static void block_span(const SCREEN *screen, INT64 y, INT64 left, INT64 right,
                       INT64 *first, INT64 *last)
{
    const UINT32 *row = screen->pixels + y * screen->stride;
    UINTN at_left = (UINTN)(row + left);
    UINTN at_last = (UINTN)(row + right - 1);
    UINTN block_start = at_left - at_left % COMPOSE_BLOCK_BYTES;
    UINTN block_end = at_last - at_last % COMPOSE_BLOCK_BYTES +
                      COMPOSE_BLOCK_BYTES;

    *first = larger(left - (INT64)((at_left - block_start) / sizeof(UINT32)),
                    0);
    *last = smaller(right + (INT64)((block_end - at_last - sizeof(UINT32)) /
                                    sizeof(UINT32)),
                    screen->width);
}

RECT compose_blocks(const SCREEN *screen, const RECT *rect)
{
    RECT cut = on_screen(screen, rect);
    RECT wide = cut;
    INT64 y;

    if (cut.left >= cut.right || cut.top >= cut.bottom)
        return cut;
    for (y = cut.top; y < cut.bottom; y++) {
        INT64 first, last;

        block_span(screen, y, cut.left, cut.right, &first, &last);
        wide.left = smaller(wide.left, first);
        wide.right = larger(wide.right, last);
    }
    return wide;
}

void compose_write(const COMPOSE_BAND *band, const SCREEN *screen,
                   const RECT *rect)
{
    RECT cut = on_screen(screen, rect);
    INT64 y, x;

    if (cut.left >= cut.right)
        return;
    cut.top = larger(cut.top, band->rect.top);
    cut.bottom = smaller(cut.bottom, band->rect.bottom);
    for (y = cut.top; y < cut.bottom; y++) {
        const UINT32 *from = band->pixels + (y - band->rect.top) * band->stride;
        UINT32 *to = screen->pixels + y * screen->stride;
        INT64 first, last;

        block_span(screen, y, cut.left, cut.right, &first, &last);
        first = larger(first, band->rect.left);
        last = smaller(last, band->rect.right);
        for (x = first; x < last; x++) {
            UINT32 pixel = from[x - band->rect.left];

            to[x] = gfx_pixel(screen, channel(pixel, 0), channel(pixel, 1),
                              channel(pixel, 2));
        }
    }
}

/* ------------------------------------------------------------------------
   The wallpaper */

const char *compose_wallpaper_refusal(const BOOTSTRAP_BACKGROUND *background,
                                      const BOOTSTRAP_DISPLAY *display,
                                      UINT32 sections)
{
    if (background->background == BOOTSTRAP_BACKGROUND_NONE)
        return "no image";
    if (background->background != BOOTSTRAP_BACKGROUND_LIMB)
        return "not limb";
    if (background->format != PIXEL_RGB && background->format != PIXEL_BGR)
        return "unknown format";
    if (background->width != display->width ||
        background->height != display->height)
        return "another size";
    if (background->width < LIMB_MIN_SIDE || background->height < LIMB_MIN_SIDE ||
        background->width > LIMB_MAX_SIDE || background->height > LIMB_MAX_SIDE)
        return "no size";
    if (sections !=
        bootstrap_background_sections(background->width, background->height))
        return "wrong sections";
    return 0;
}

/* Whether rect holds a pixel. */
static int has_pixels(const RECT *rect)
{
    return rect->left < rect->right && rect->top < rect->bottom;
}

/* Whether two rectangles that each hold a pixel share one. */
static int overlap(const RECT *a, const RECT *b)
{
    return a->left < b->right && b->left < a->right && a->top < b->bottom &&
           b->top < a->bottom;
}

/* Draw the pointer's image over band, if the pixels it touches (image) are
   any of the band's, since drawing it over a band it does not touch changes
   nothing and costs the tracing of its outlines. */
static void draw_cursor(const COMPOSE_CURSOR *cursor, const RECT *image,
                        const COMPOSE_BAND *band)
{
    if (overlap(image, &band->rect))
        compose_pointer(cursor->work, cursor->image, band, cursor->x,
                        cursor->y, cursor->quarters);
}

void compose_frame(const SCREEN *screen, const COMPOSE_WALLPAPER *wallpaper,
                   const COMPOSE_WINDOW *windows, UINT32 count,
                   const RECT *rect, UINT32 *memory, INT64 rows,
                   const COMPOSE_CURSOR *cursor)
{
    RECT image;
    int pointer = cursor != 0 && cursor->placed;
    INT64 top;

    /* A rectangle with no pixel returns before the image's box is found,
       which costs the tracing of its outlines. */
    if (rows < 1 || !has_pixels(rect))
        return;
    if (pointer)
        pointer = compose_pointer_box(cursor->work, cursor->image, cursor->x,
                                      cursor->y, cursor->quarters, &image);
    for (top = rect->top; top < rect->bottom; top += rows) {
        RECT written;
        COMPOSE_BAND band;

        written.left = rect->left;
        written.top = top;
        written.right = rect->right;
        written.bottom = smaller(top + rows, rect->bottom);
        band.pixels = memory;
        band.stride = screen->width;
        band.rect = compose_blocks(screen, &written);
        compose_band(&band, wallpaper, windows, count);
        if (pointer)
            draw_cursor(cursor, &image, &band);
        compose_write(&band, screen, &written);
    }
}

void compose_wallpaper_frame(const SCREEN *screen,
                             const COMPOSE_WALLPAPER *wallpaper,
                             UINT32 *memory, INT64 rows)
{
    RECT whole;

    whole.left = 0;
    whole.top = 0;
    whole.right = screen->width;
    whole.bottom = screen->height;
    compose_frame(screen, wallpaper, 0, 0, &whole, memory, rows, 0);
}

/* ------------------------------------------------------------------------
   The pointer */

/* Where a shape's origin lies on the canvas: the top left corner of pixel
   (x, y), at a scale of quarters / COMPOSE_QUARTERS. */
typedef struct {
    INT64 x, y;
    UINT32 quarters;
} ORIGIN;

/* v, in 256ths of a point, in 256ths of a pixel at the origin's scale,
   rounded once to the nearest, halves up, as outline.c rounds. */
static INT64 scaled(INT64 v, UINT32 quarters)
{
    return floor_div(2 * v * (INT64)quarters + COMPOSE_QUARTERS,
                     2 * COMPOSE_QUARTERS);
}

/* The canvas coordinate, in 256ths of a pixel, of coordinate index of
   shape's points: 2 * point for x and 2 * point + 1 for y. */
static INT64 coordinate(const COMPOSE_SHAPE *shape, UINT32 index,
                        INT64 origin, const ORIGIN *at)
{
    return origin * OUTLINE_ONE + scaled(shape->points[index], at->quarters);
}

/* How many points the curve that starts at a point with this flag takes,
   itself and the ones after it: a line 1, a quadratic 2, a cubic 3; or 0
   if the flag names none. */
static UINT32 span_of(UINT8 flag)
{
    switch (flag) {
    case COMPOSE_POINT_ON_CURVE:
        return 1;
    case COMPOSE_POINT_QUADRATIC:
        return 2;
    case COMPOSE_POINT_CUBIC:
        return 3;
    default:
        return 0;
    }
}

/* Whether the shape's contours are well formed: each starts on the curve,
   holds two points at least, and ends with a curve's last point, and no
   coordinate lies past OUTLINE_COORDINATE_MAX, which keeps scaling it
   within an INT64. */
static int well_formed(const COMPOSE_SHAPE *shape)
{
    UINT32 c, start = 0, i;

    for (i = 0; i < 2 * shape->point_count; i++)
        if (shape->points[i] > OUTLINE_COORDINATE_MAX ||
            shape->points[i] < -OUTLINE_COORDINATE_MAX)
            return 0;
    for (c = 0; c < shape->contours; c++) {
        UINT32 end = shape->ends[c];

        if (end > shape->point_count || end < start + 2 ||
            shape->flags[start] != COMPOSE_POINT_ON_CURVE)
            return 0;
        for (i = start + 1; i < end;) {
            UINT32 span = span_of(shape->flags[i]);

            if (span == 0 || i + span > end ||
                shape->flags[i + span - 1] != COMPOSE_POINT_ON_CURVE ||
                (span == 3 && shape->flags[i + 1] != COMPOSE_POINT_CUBIC))
                return 0;
            i += span;
        }
        start = end;
    }
    return 1;
}

/* Build a well formed shape in outline. Returns 0 if the outline could
   not hold it. */
static int trace(OUTLINE *outline, const COMPOSE_SHAPE *shape,
                 const ORIGIN *at)
{
    UINT32 c, start = 0, i;

#define X(point) coordinate(shape, 2 * (point), at->x, at)
#define Y(point) coordinate(shape, 2 * (point) + 1, at->y, at)
    outline_start(outline);
    for (c = 0; c < shape->contours; c++) {
        UINT32 end = shape->ends[c];

        outline_move(outline, X(start), Y(start));
        for (i = start + 1; i < end;) {
            if (shape->flags[i] == COMPOSE_POINT_ON_CURVE) {
                outline_line(outline, X(i), Y(i));
                i += 1;
            } else if (shape->flags[i] == COMPOSE_POINT_QUADRATIC) {
                outline_quad(outline, X(i), Y(i), X(i + 1), Y(i + 1));
                i += 2;
            } else {
                outline_cubic(outline, X(i), Y(i), X(i + 1), Y(i + 1),
                              X(i + 2), Y(i + 2));
                i += 3;
            }
        }
        outline_close(outline);
        start = end;
    }
#undef X
#undef Y
    return !outline->failed;
}

/* The opacity, of OUTLINE_OPACITY_ONE, that percent of the ink gives,
   rounded to the nearest, halves up. */
static UINT32 opacity_of(UINT32 percent)
{
    return (percent * OUTLINE_OPACITY_ONE + COMPOSE_PERCENT / 2) /
           COMPOSE_PERCENT;
}

/* Whether the pointer can be drawn at all, at this scale, with its
   hotspot at (x, y): a scale, an edge opacity that is a percent, two well
   formed shapes and a hotspot within COMPOSE_COORDINATE_MAX, which keeps
   the shapes' coordinates within an INT64. */
static int usable(const COMPOSE_POINTER *pointer, UINT32 quarters, INT64 x,
                  INT64 y)
{
    return quarters != 0 && pointer->edge_percent <= COMPOSE_PERCENT &&
           x >= -COMPOSE_COORDINATE_MAX && x <= COMPOSE_COORDINATE_MAX &&
           y >= -COMPOSE_COORDINATE_MAX && y <= COMPOSE_COORDINATE_MAX &&
           well_formed(&pointer->body) && well_formed(&pointer->edge);
}

int compose_pointer(OUTLINE *work, const COMPOSE_POINTER *pointer,
                    const COMPOSE_BAND *band, INT64 x, INT64 y,
                    UINT32 quarters)
{
    ORIGIN at;
    CANVAS canvas;

    if (!usable(pointer, quarters, x, y))
        return 0;
    at.x = x - band->rect.left;
    at.y = y - band->rect.top;
    at.quarters = quarters;
    canvas.pixels = band->pixels;
    canvas.width = band->rect.right - band->rect.left;
    canvas.height = band->rect.bottom - band->rect.top;
    canvas.stride = band->stride;
    /* Both shapes are built before either is filled, so that a shape the
       outline cannot hold leaves nothing drawn. */
    if (canvas.width > OUTLINE_WIDTH_MAX || !trace(work, &pointer->body, &at) ||
        !trace(work, &pointer->edge, &at))
        return 0;
    if (!outline_fill(work, &canvas, pointer->edge_ink,
                      opacity_of(pointer->edge_percent)))
        return 0;
    return trace(work, &pointer->body, &at) &&
           outline_fill(work, &canvas, pointer->body_ink,
                        OUTLINE_OPACITY_ONE);
}

/* Grow *box to hold the lines of outline, to the pixels that have
   coverage: from the pixel holding the least coordinate to the one
   holding the greatest, a coordinate on a pixel's far edge belonging to
   the next pixel. *any says whether *box holds a line yet. */
static void grow_to_lines(RECT *box, int *any, const OUTLINE *outline)
{
    UINT32 i;

    for (i = 0; i < outline->count; i++) {
        const OUTLINE_EDGE *e = &outline->edges[i];
        RECT line;

        line.left = floor_div(smaller(e->x0, e->x1), OUTLINE_ONE);
        line.top = floor_div(smaller(e->y0, e->y1), OUTLINE_ONE);
        line.right = floor_div(larger(e->x0, e->x1) - 1, OUTLINE_ONE) + 1;
        line.bottom = floor_div(larger(e->y0, e->y1) - 1, OUTLINE_ONE) + 1;
        if (*any) {
            box->left = smaller(box->left, line.left);
            box->top = smaller(box->top, line.top);
            box->right = larger(box->right, line.right);
            box->bottom = larger(box->bottom, line.bottom);
        } else {
            *box = line;
            *any = 1;
        }
    }
}

int compose_pointer_box(OUTLINE *work, const COMPOSE_POINTER *pointer,
                        INT64 x, INT64 y, UINT32 quarters, RECT *box)
{
    ORIGIN at;
    RECT found;
    int any = 0;

    if (!usable(pointer, quarters, x, y))
        return 0;
    /* Fields, not an initializer: TinyCC turns a local aggregate's
       initializer into a call to memset, and no program here links a C
       library (docs/spec/interface.md P2). */
    found.left = x;
    found.top = y;
    found.right = x;
    found.bottom = y;
    at.x = x;
    at.y = y;
    at.quarters = quarters;
    if (!trace(work, &pointer->body, &at))
        return 0;
    grow_to_lines(&found, &any, work);
    if (!trace(work, &pointer->edge, &at))
        return 0;
    grow_to_lines(&found, &any, work);
    *box = found;
    return 1;
}

void compose_cursor_move(COMPOSE_CURSOR *cursor, const SCREEN *screen,
                         INT64 x, INT64 y, COMPOSE_CURSOR_DAMAGE *damage)
{
    RECT before, after;

    if (cursor->placed && cursor->x == x && cursor->y == y)
        return;
    if (!compose_pointer_box(cursor->work, cursor->image, x, y,
                             cursor->quarters, &after))
        return;
    if (!damage->moved) {
        /* Fields, not an initializer, as in compose_pointer_box. A pointer
           with no place leaves no box. One with a place was accepted
           there, and the image and the scale have not changed, so its box
           is found again. */
        before.left = 0;
        before.top = 0;
        before.right = 0;
        before.bottom = 0;
        if (cursor->placed)
            compose_pointer_box(cursor->work, cursor->image, cursor->x,
                                cursor->y, cursor->quarters, &before);
        damage->old = compose_blocks(screen, &before);
        damage->moved = 1;
    }
    damage->now = compose_blocks(screen, &after);
    cursor->placed = 1;
    cursor->x = x;
    cursor->y = y;
}

UINT32 compose_cursor_rects(const COMPOSE_CURSOR_DAMAGE *damage,
                            RECT rects[COMPOSE_CURSOR_RECTS_MAX])
{
    int old = has_pixels(&damage->old), now = has_pixels(&damage->now);
    UINT32 count = 0;

    if (old && now && overlap(&damage->old, &damage->now)) {
        rects[0].left = smaller(damage->old.left, damage->now.left);
        rects[0].top = smaller(damage->old.top, damage->now.top);
        rects[0].right = larger(damage->old.right, damage->now.right);
        rects[0].bottom = larger(damage->old.bottom, damage->now.bottom);
        return 1;
    }
    if (old)
        rects[count++] = damage->old;
    if (now)
        rects[count++] = damage->now;
    return count;
}
