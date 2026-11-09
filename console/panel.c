/* The panel. Its backdrop is made once, Limb's pixels, read from the
   kernel's background or computed, mixed with the tint in linear light,
   so that the framebuffer, which is mapped write-combining and slow to
   read, is only written; a cell is drawn by mixing its masks' coverage
   into the backdrop through tables. */

#include "panel.h"
#include "font.h"
#include "limb.h"

/* The padding around the text, in quarters of a cell's width: the side
   padding is wider by the strokes' bearing, so that the text's edges
   stand as far from the panel's on every side. */
#define SIDE_PADDING 5
#define END_PADDING 4

/* The panel's centre lies this far down the screen, so that it sits
   clear above the planet's edge, which Limb puts at 78 percent. */
#define CENTRE_PERCENT 40
#define PERCENT 100

/* The tint, mixed into the background at PANEL_OPACITY percent; the
   text's ink, a little softer than the wordmark's; and the caret's, the
   warm light of Limb's sunrise. */
#define PANEL_OPACITY 68
static const UINT8 tint[PANEL_CHANNELS] = {18, 22, 32};
static const UINT8 ink[PANEL_INKS][PANEL_CHANNELS] = {
    [PANEL_TEXT] = {232, 234, 240},
    [PANEL_CARET] = {236, 170, 96},
};

#define CHANNEL_BITS 8
#define CHANNEL_MASK 0xFFu
#define BLANK ' '

static INT64 smaller(INT64 a, INT64 b) { return a < b ? a : b; }

static INT64 larger(INT64 a, INT64 b) { return a > b ? a : b; }

int panel_layout(LAYOUT *layout, INT64 width, INT64 height)
{
    INT64 cell = font_cell_width(height);
    INT64 side = cell * SIDE_PADDING / CELL_QUARTERS;
    INT64 end = cell * END_PADDING / CELL_QUARTERS;
    INT64 line = font_height(cell);
    INT64 columns = smaller(TEXT_COLUMNS_MAX, (width - 2 * side) / cell);
    INT64 rows = smaller(PANEL_ROWS, (height - 2 * end) / line);
    INT64 panel_width, panel_height;

    if (columns < 1 || rows < 1)
        return 0;
    panel_width = columns * cell + 2 * side;
    panel_height = rows * line + 2 * end;
    layout->cell_width = cell;
    layout->cell_height = line;
    layout->columns = (UINT32)columns;
    layout->rows = (UINT32)rows;
    layout->box.left = (width - panel_width) / 2;
    layout->box.top =
        larger(0, smaller(height - panel_height,
                          height * CENTRE_PERCENT / PERCENT -
                              panel_height / 2));
    layout->box.right = layout->box.left + panel_width;
    layout->box.bottom = layout->box.top + panel_height;
    layout->radius = font_panel_radius(cell);
    layout->text_left = layout->box.left + side;
    layout->text_top = layout->box.top + end;
    return 1;
}

static INT64 box_width(const LAYOUT *layout)
{
    return layout->box.right - layout->box.left;
}

static INT64 box_height(const LAYOUT *layout)
{
    return layout->box.bottom - layout->box.top;
}

UINT64 panel_backdrop_size(const LAYOUT *layout)
{
    return (UINT64)(box_width(layout) * box_height(layout)) * sizeof(UINT32);
}

/* ------------------------------------------------------------------------
   The backdrop. */

static INT64 clamp(INT64 v, INT64 low, INT64 high)
{
    return v < low ? low : v > high ? high : v;
}

/* How many of the samples of panel pixel (x, y) lie inside the panel's
   rounded rectangle, at the rasterizer's sample points: within the
   radius of the nearest point of the rectangle the corners' centres
   span. */
static INT64 corner_coverage(const LAYOUT *layout, INT64 x, INT64 y)
{
    INT64 r = layout->radius * SUBPIXEL;
    INT64 right = box_width(layout) * SUBPIXEL - r;
    INT64 bottom = box_height(layout) * SUBPIXEL - r;
    INT64 sx, sy, hits = 0;

    for (sy = 0; sy < SAMPLES; sy++)
        for (sx = 0; sx < SAMPLES; sx++) {
            INT64 px = x * SUBPIXEL + sx * (SUBPIXEL / SAMPLES) + 1;
            INT64 py = y * SUBPIXEL + sy * (SUBPIXEL / SAMPLES) + 1;
            INT64 dx = px - clamp(px, r, right);
            INT64 dy = py - clamp(py, r, bottom);

            if (dx * dx + dy * dy <= r * r)
                hits++;
        }
    return hits;
}

/* The tint over code at coverage of the panel. */
static UINT32 tinted(UINT32 rgb, INT64 coverage)
{
    UINT32 out = 0;
    int k;

    for (k = 0; k < PANEL_CHANNELS; k++) {
        INT64 shift = (PANEL_CHANNELS - 1 - k) * CHANNEL_BITS;

        out |= (UINT32)gfx_cross_fade(rgb >> shift & CHANNEL_MASK, tint[k],
                                      PANEL_OPACITY * coverage,
                                      PERCENT * COVERAGE_MAX)
               << shift;
    }
    return out;
}

/* Mix the tint into backdrop, Limb's codes under the panel row by row
   as 0x00RRGGBB, in place: through tables where the panel covers a
   pixel whole, and by its rounded rectangle's coverage at the corners. */
static void tint_backdrop(const LAYOUT *layout, UINT32 *backdrop)
{
    UINT8 over[PANEL_CHANNELS][PANEL_CODES];
    INT64 w = box_width(layout), h = box_height(layout), x, y;
    INT64 r = layout->radius;
    int k, code;

    for (k = 0; k < PANEL_CHANNELS; k++)
        for (code = 0; code < PANEL_CODES; code++)
            over[k][code] = (UINT8)gfx_cross_fade(code, tint[k], PANEL_OPACITY,
                                                  PERCENT);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            UINT32 rgb = backdrop[y * w + x];
            int corner = (x < r || x >= w - r) && (y < r || y >= h - r);

            backdrop[y * w + x] =
                corner ? tinted(rgb, corner_coverage(layout, x, y))
                       : (UINT32)over[0][rgb >> 2 * CHANNEL_BITS &
                                         CHANNEL_MASK]
                                 << 2 * CHANNEL_BITS |
                             (UINT32)over[1][rgb >> CHANNEL_BITS &
                                             CHANNEL_MASK]
                                 << CHANNEL_BITS |
                             over[2][rgb & CHANNEL_MASK];
        }
}

int panel_backdrop(const LAYOUT *layout, INT64 width, INT64 height,
                   UINT32 *backdrop)
{
    INT64 w = box_width(layout), h = box_height(layout), x, y;
    LIMB limb;

    if (!limb_init(&limb, width, height))
        return 0;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            backdrop[y * w + x] =
                limb_pixel(&limb, layout->box.left + x, layout->box.top + y);
    tint_backdrop(layout, backdrop);
    return 1;
}

const char *panel_background_refusal(const BOOTSTRAP_BACKGROUND *background,
                                     const BOOTSTRAP_DISPLAY *display)
{
    LAYOUT layout;

    if (background->background == BOOTSTRAP_BACKGROUND_NONE)
        return "no image";
    if (background->background != BOOTSTRAP_BACKGROUND_LIMB)
        return "not limb";
    if (background->format != PIXEL_RGB && background->format != PIXEL_BGR)
        return "unknown format";
    if (background->width != display->width ||
        background->height != display->height)
        return "another size";
    if (background->width < LIMB_MIN_SIDE ||
        background->height < LIMB_MIN_SIDE ||
        background->width > LIMB_MAX_SIDE ||
        background->height > LIMB_MAX_SIDE)
        return "no size";
    if (!panel_layout(&layout, background->width, background->height))
        return "no room for a cell";
    return 0;
}

/* The image's pixels go into the backdrop with their codes in its
   order, red from bit 16 and blue from bit 0, gfx_pixel's order undone:
   a PIXEL_BGR pixel as it is, a PIXEL_RGB one with red and blue
   swapped. The tint reads only those 24 bits. The format is tested once
   a row, since TinyCC inlines no call. */
void panel_backdrop_image(const LAYOUT *layout, const UINT32 *image,
                          INT64 width, UINT32 format, UINT32 *backdrop)
{
    INT64 w = box_width(layout), h = box_height(layout), x, y;

    for (y = 0; y < h; y++) {
        const UINT32 *row =
            image + (layout->box.top + y) * width + layout->box.left;
        UINT32 *out = backdrop + y * w;

        if (format == PIXEL_BGR)
            for (x = 0; x < w; x++)
                out[x] = row[x];
        else
            for (x = 0; x < w; x++)
                out[x] = (row[x] & CHANNEL_MASK) << 2 * CHANNEL_BITS |
                         (row[x] & CHANNEL_MASK << CHANNEL_BITS) |
                         (row[x] >> 2 * CHANNEL_BITS & CHANNEL_MASK);
    }
    tint_backdrop(layout, backdrop);
}

void panel_inks(INKS *inks)
{
    int i, k, coverage, code;

    for (i = 0; i < PANEL_INKS; i++)
        for (k = 0; k < PANEL_CHANNELS; k++)
            for (coverage = 0; coverage <= COVERAGE_MAX; coverage++)
                for (code = 0; code < PANEL_CODES; code++)
                    inks->mix[i][k][coverage][code] = (UINT8)gfx_cross_fade(
                        code, ink[i][k], coverage, COVERAGE_MAX);
}

/* ------------------------------------------------------------------------
   Drawing. */

static UINT32 screen_pixel(const SCREEN *screen, UINT32 rgb)
{
    return gfx_pixel(screen, rgb >> 2 * CHANNEL_BITS & CHANNEL_MASK,
                     rgb >> CHANNEL_BITS & CHANNEL_MASK, rgb & CHANNEL_MASK);
}

int panel_limb(const SCREEN *screen)
{
    LIMB limb;
    INT64 x, y;

    if (!limb_init(&limb, screen->width, screen->height))
        return 0;
    for (y = 0; y < screen->height; y++)
        for (x = 0; x < screen->width; x++)
            screen->pixels[y * screen->stride + x] =
                screen_pixel(screen, limb_pixel(&limb, x, y));
    return 1;
}

void panel_show(const SCREEN *screen, const LAYOUT *layout,
                const UINT32 *backdrop, PANEL_SHOWN *shown)
{
    INT64 w = box_width(layout), h = box_height(layout), x, y;
    UINT32 row, column;

    for (y = 0; y < h; y++) {
        UINT32 *pixels = screen->pixels +
                         (layout->box.top + y) * screen->stride +
                         layout->box.left;

        for (x = 0; x < w; x++)
            pixels[x] = screen_pixel(screen, backdrop[y * w + x]);
    }
    for (row = 0; row < layout->rows; row++)
        for (column = 0; column < layout->columns; column++)
            shown->cells[row][column] = BLANK;
    shown->caret.lit = 0;
}

/* rgb with ink mixed in at coverage. */
static UINT32 under(const INKS *inks, int which, UINT32 rgb, UINT8 coverage)
{
    UINT32 out = 0;
    int k;

    for (k = 0; k < PANEL_CHANNELS; k++) {
        INT64 shift = (PANEL_CHANNELS - 1 - k) * CHANNEL_BITS;

        out |= (UINT32)inks->mix[which][k][coverage][rgb >> shift &
                                                     CHANNEL_MASK]
               << shift;
    }
    return out;
}

/* Draw the cell at row and column, the column up to the layout's
   columns, the last in the padding, with character's glyph, and the
   caret over it if caret is nonzero. */
static void cell(const PANEL *panel, UINT64 row, UINT32 column,
                 UINT8 character, int caret)
{
    const LAYOUT *layout = panel->layout;
    const SCREEN *screen = panel->screen;
    INT64 w = box_width(layout), cw = layout->cell_width;
    INT64 left = layout->text_left + column * cw;
    INT64 top = layout->text_top + (INT64)row * layout->cell_height;
    const UINT8 *glyph = font_mask(panel->masks, cw, character);
    const UINT8 *caret_glyph =
        caret ? font_mask(panel->masks, cw, FONT_CARET) : 0;
    INT64 x, y;

    for (y = 0; y < layout->cell_height; y++) {
        const UINT32 *from = panel->backdrop +
                             (top + y - layout->box.top) * w +
                             (left - layout->box.left);
        UINT32 *to = screen->pixels + (top + y) * screen->stride + left;

        for (x = 0; x < cw; x++) {
            UINT32 rgb = from[x];
            UINT8 a = glyph[y * cw + x];

            if (a)
                rgb = under(panel->inks, PANEL_TEXT, rgb, a);
            if (caret_glyph && caret_glyph[y * cw + x])
                rgb = under(panel->inks, PANEL_CARET, rgb,
                            caret_glyph[y * cw + x]);
            to[x] = screen_pixel(screen, rgb);
        }
    }
}

static int at(const VIEW_CARET *caret, UINT64 row, UINT32 column)
{
    return caret->lit && caret->row == row && caret->column == column;
}

void panel_text(const PANEL *panel, PANEL_SHOWN *shown, const TEXT *text,
                UINT64 top, const VIEW_CARET *caret)
{
    const LAYOUT *layout = panel->layout;
    VIEW_CARET was = shown->caret;
    int moved = caret->lit != was.lit || caret->row != was.row ||
                caret->column != was.column;
    UINT64 row;
    UINT32 column;

    for (row = 0; row < layout->rows; row++) {
        UINT64 line = top + row;
        const UINT8 *cells = line < text->lines ? text_line(text, line) : 0;

        for (column = 0; column < layout->columns; column++) {
            UINT8 character = cells ? cells[column] : (UINT8)BLANK;

            if (character != shown->cells[row][column] ||
                (moved && (at(caret, row, column) || at(&was, row, column))))
                cell(panel, row, column, character, at(caret, row, column));
            shown->cells[row][column] = character;
        }
    }
    if (moved && at(&was, was.row, layout->columns))
        cell(panel, was.row, layout->columns, BLANK, 0);
    if (moved && at(caret, caret->row, layout->columns))
        cell(panel, caret->row, layout->columns, BLANK, 1);
    shown->caret = *caret;
}

/* ------------------------------------------------------------------------
   A window's pixels. */

/* A pixel's cover is its top byte and runs to COVER_MAX; a mask's
   coverage becomes a cover by rounding half up. */
#define COVER_SHIFT 24
#define COVER_MAX 255
#define COVER_ROUND (COVERAGE_MAX / 2)

/* A cell that holds nothing drawn: no character the text holds is 0. */
#define UNDRAWN 0

static UINT32 cover_of(UINT8 coverage)
{
    return ((UINT32)coverage * COVER_MAX + COVER_ROUND) / COVERAGE_MAX;
}

UINT32 panel_window_pixel(UINT8 glyph, UINT8 caret)
{
    UINT32 g = cover_of(glyph), c = cover_of(caret), cover, rgb = 0;
    int k;

    cover = g + c - g * c / COVER_MAX;
    if (cover == 0)
        return 0;
    for (k = 0; k < PANEL_CHANNELS; k++) {
        INT64 shift = (PANEL_CHANNELS - 1 - k) * CHANNEL_BITS;
        UINT32 code;

        if (c == 0)
            code = ink[PANEL_TEXT][k];
        else if (g == 0)
            code = ink[PANEL_CARET][k];
        else
            code = (UINT32)gfx_cross_fade(ink[PANEL_TEXT][k],
                                          ink[PANEL_CARET][k], c, cover);
        rgb |= code << shift;
    }
    return cover << COVER_SHIFT | rgb;
}

/* The character cell (row, column) of shown holds: the padding to the
   right of the last column holds none but the caret. */
static UINT8 character_of(const LAYOUT *layout, const PANEL_SHOWN *shown,
                          UINT64 row, UINT32 column)
{
    return column < layout->columns ? shown->cells[row][column]
                                    : (UINT8)BLANK;
}

static int differs(const LAYOUT *layout, const PANEL_SHOWN *was,
                   const PANEL_SHOWN *want, UINT64 row, UINT32 column)
{
    return character_of(layout, was, row, column) !=
               character_of(layout, want, row, column) ||
           at(&was->caret, row, column) != at(&want->caret, row, column);
}

/* The window's own coordinates of cell (row, column)'s left and top. */
static INT64 cell_left(const LAYOUT *layout, UINT32 column)
{
    return layout->text_left - layout->box.left + column * layout->cell_width;
}

static INT64 cell_top(const LAYOUT *layout, UINT64 row)
{
    return layout->text_top - layout->box.top +
           (INT64)row * layout->cell_height;
}

void panel_view(const LAYOUT *layout, const TEXT *text, UINT64 top,
                const VIEW_CARET *caret, PANEL_SHOWN *want)
{
    UINT64 row;
    UINT32 column;

    for (row = 0; row < layout->rows; row++) {
        UINT64 line = top + row;
        const UINT8 *cells = line < text->lines ? text_line(text, line) : 0;

        for (column = 0; column < layout->columns; column++)
            want->cells[row][column] = cells ? cells[column] : (UINT8)BLANK;
    }
    want->caret.row = caret->row;
    want->caret.column = caret->column;
    want->caret.lit = caret->lit;
}

void panel_window_forget(const LAYOUT *layout, PANEL_SHOWN *shown)
{
    UINT64 row;
    UINT32 column;

    for (row = 0; row < layout->rows; row++)
        for (column = 0; column < layout->columns; column++)
            shown->cells[row][column] = UNDRAWN;
    shown->caret.lit = 0;
}

int panel_window_damage(const LAYOUT *layout, const PANEL_SHOWN *was,
                        const PANEL_SHOWN *want, RECT *damage)
{
    int found = 0;
    UINT64 row;
    UINT32 column;

    for (row = 0; row < layout->rows; row++)
        for (column = 0; column <= layout->columns; column++) {
            INT64 left, top;

            if (!differs(layout, was, want, row, column))
                continue;
            left = cell_left(layout, column);
            top = cell_top(layout, row);
            if (!found || left < damage->left)
                damage->left = left;
            if (!found) /* the rows go top down: the first cell has the top */
                damage->top = top;
            if (!found || left + layout->cell_width > damage->right)
                damage->right = left + layout->cell_width;
            if (!found || top + layout->cell_height > damage->bottom)
                damage->bottom = top + layout->cell_height;
            found = 1;
        }
    return found;
}

int panel_window_buffer(const int *available, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        if (available[i])
            return (int)i;
    return PANEL_NO_BUFFER;
}

/* Draw the cell at row and column, the column up to the layout's columns,
   into pixels, with character's glyph, and the caret over it if caret is
   nonzero. */
static void window_cell(const LAYOUT *layout, const UINT8 *masks,
                        UINT32 *pixels, UINT32 stride, UINT64 row,
                        UINT32 column, UINT8 character, int caret)
{
    INT64 cw = layout->cell_width;
    INT64 left = cell_left(layout, column), top = cell_top(layout, row);
    const UINT8 *glyph = font_mask(masks, cw, character);
    const UINT8 *caret_glyph = caret ? font_mask(masks, cw, FONT_CARET) : 0;
    INT64 x, y;

    for (y = 0; y < layout->cell_height; y++) {
        UINT32 *to = pixels + (top + y) * stride + left;

        for (x = 0; x < cw; x++)
            to[x] = panel_window_pixel(
                glyph[y * cw + x], caret_glyph ? caret_glyph[y * cw + x] : 0);
    }
}

void panel_window_draw(const LAYOUT *layout, const UINT8 *masks,
                       UINT32 *pixels, UINT32 stride, PANEL_SHOWN *shown,
                       const PANEL_SHOWN *want)
{
    UINT64 row;
    UINT32 column;

    for (row = 0; row < layout->rows; row++)
        for (column = 0; column <= layout->columns; column++)
            if (differs(layout, shown, want, row, column))
                window_cell(layout, masks, pixels, stride, row, column,
                            character_of(layout, want, row, column),
                            at(&want->caret, row, column));
    for (row = 0; row < layout->rows; row++)
        for (column = 0; column < layout->columns; column++)
            shown->cells[row][column] = want->cells[row][column];
    shown->caret.row = want->caret.row;
    shown->caret.column = want->caret.column;
    shown->caret.lit = want->caret.lit;
}
