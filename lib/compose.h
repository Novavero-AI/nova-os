/** @file
    The compositor's composer: what it computes without a system call
    (docs/spec/display.md D7 rule 2, D8 and D10 rule 7, and D19's phase 3
    arrangement, rules 1, 4 and 7).

    A material is three tables of codes made from a tint and an amount. A
    band of the screen is composed from the wallpaper and the windows,
    bottom to top, as 0x00RRGGBB: a window's pixel is its colour mixed
    over its material by its cover, and a window's corners mix it over
    what is composed beneath, by the coverage of its rounded rectangle.
    A finished band is written to the
    framebuffer in the screen's order, row by row, in whole 64-byte
    blocks. The pointer's image is drawn over a band from the theme's
    outlines at the screen's scale, by lib/outline.c, last, so over every
    window; a frame draws it where a COMPOSE_CURSOR says the pointer is,
    compose_cursor_move says what a move of it damages, and
    compose_cursor_rects which rectangles a frame composes for its moves.

    Integer arithmetic only, with no machine dependence, so a host build
    composes the pixels nova-os does (tools/check-compose.c), and every
    mix is gfx/raster.c's, in linear light. */

#ifndef NOVA_COMPOSE_H
#define NOVA_COMPOSE_H

#include "gfx.h"
#include "native_abi.h"
#include "outline.h"
#include "types.h"

/** The colour channels of a pixel: red, green and blue, in that order
    in a band and in a window's buffer, red in the highest byte. */
#define COMPOSE_CHANNELS 3

/** The codes of one channel: sRGB codes, 0 to 255. */
#define COMPOSE_CODES 256

/** A cover, and a corner's coverage, when the colour holds the whole
    pixel: a cover is a byte (display.md D6). */
#define COMPOSE_COVER_MAX 255

/** The bit where a window's pixel holds its cover, above the 24 bits of
    its colour (display.md D6). */
#define COMPOSE_COVER_SHIFT 24

/** A window's pixel without its cover: the colour, as 0x00RRGGBB. */
#define COMPOSE_COLOUR_MASK 0x00FFFFFFu

/** What a material's amount and a pointer's edge opacity are percent
    of. */
#define COMPOSE_PERCENT 100

/** The bytes in one write-combining buffer, the unit the processor
    writes to a write-combining framebuffer in bursts (Intel 64 and
    IA-32 Architectures Software Developer's Manual, Volume 3A, 14.3.1,
    for the processors nova-os runs on). */
#define COMPOSE_BLOCK_BYTES 64

/** The scale is held in quarters: this many to a scale of 1
    (display.md D7, rule 5). */
#define COMPOSE_QUARTERS 4

/** The most pixels a window's corner can reach, which is the radius
    rounded up to a whole pixel: 16 points, the greatest radius the theme
    gives a surface (docs/spec/look.md V6), at a scale of 5.5, the
    largest screen's (display.md D7 rule 5). The corners' plane is held
    in memory of twice that on each side. */
#define COMPOSE_REACH_MAX 88

/** The greatest magnitude of a coordinate given here, a window's place,
    a band's rectangle or a pointer's hotspot, in pixels: far past a
    screen, which is 7680 across at most, and low enough that no sum or
    difference of two coordinates overflows. */
#define COMPOSE_COORDINATE_MAX ((INT64)1 << 30)

/** A point of a shape that lies on its curve. */
#define COMPOSE_POINT_ON_CURVE 1

/** A point of a shape that is a quadratic Bezier curve's control point,
    between two points on the curve. */
#define COMPOSE_POINT_QUADRATIC 0

/** A point of a shape that is one of a cubic Bezier curve's two control
    points, between two points on the curve. */
#define COMPOSE_POINT_CUBIC 2

/** A material: for each channel and each code of the wallpaper beneath,
    the code the wallpaper becomes under the material's tint, mixed in
    linear light. */
typedef struct {
    UINT8 code[COMPOSE_CHANNELS][COMPOSE_CODES]; /**< the three tables */
} COMPOSE_MATERIAL;

/** The wallpaper: an image the width of the screen, as the kernel hands
    one over or as the compositor computes it. */
typedef struct {
    const UINT32 *pixels; /**< the top-left pixel */
    INT64 stride;         /**< pixels from a row's start to the next's */
    UINT32 format;        /**< PIXEL_RGB or PIXEL_BGR, gfx.h; each
                               pixel's unused byte is ignored */
} COMPOSE_WALLPAPER;

/** The coverage of a window's corners at one radius, made by
    compose_corners and shared by every window of that radius. It holds
    the coverage of a square 2 x reach pixels on a side whose rounded
    rectangle has the radius, so that each of its four quadrants is a
    corner of any window at least that big, byte for byte. */
typedef struct {
    INT64 reach;  /**< the pixels a corner reaches along each edge: the
                       radius rounded up, 0 for square corners */
    UINT8 plane[4 * COMPOSE_REACH_MAX * COMPOSE_REACH_MAX]; /**< the square's
                       coverage, 2 x reach by 2 x reach, a row after the
                       row above it */
} COMPOSE_CORNERS;

/** A window, as the compositor shows it. Its buffer holds its pixels:
    the colour as 0x00RRGGBB and in the top byte its cover. The buffer
    holds rows rows of stride pixels, the first columns of each in use,
    and columns is at most stride. Where the buffer is smaller than the
    place the window shows the window's material there, as a pixel of
    cover 0; where it is larger, the place cuts it (display.md D7 rule
    4). A window at least twice its corners' reach across and high has at
    each corner the coverage of its rounded rectangle; compose_corner
    says what a smaller one has. */
typedef struct {
    const UINT32 *pixels;            /**< the buffer's top-left pixel */
    INT64 stride;                    /**< pixels from a row to the next */
    INT64 columns;                   /**< pixels in use in each row */
    INT64 rows;                      /**< rows in use */
    RECT place;                      /**< where the window is on the
                                          screen, which may reach past
                                          its edges */
    const COMPOSE_MATERIAL *material; /**< its surface's material */
    const COMPOSE_CORNERS *corners;  /**< its corners at its theme's
                                          radius, or 0 for square
                                          ones */
} COMPOSE_WINDOW;

/** A band: memory holding the composed pixels of a rectangle of the
    screen, as 0x00RRGGBB. */
typedef struct {
    UINT32 *pixels; /**< the rectangle's top-left pixel */
    INT64 stride;   /**< pixels from a row's start to the next's */
    RECT rect;      /**< the rectangle of the screen it holds */
} COMPOSE_BAND;

/** A shape the theme holds, in the form the interface description
    language's generator writes for an outline setting
    (docs/spec/interface.md P12): contours of points in 256ths of a
    point, x right and y down from the shape's origin. A contour's first
    point lies on the curve and its last is joined to its first by a
    line. */
typedef struct {
    const INT32 *points;  /**< x and y of each point */
    const UINT8 *flags;   /**< each point's COMPOSE_POINT_* */
    const UINT32 *ends;   /**< each contour's end: the index of the
                               point after its last */
    UINT32 point_count;   /**< the points in all */
    UINT32 contours;      /**< the contours in all */
} COMPOSE_SHAPE;

/** The pointer's image as the theme gives it (docs/spec/look.md V15):
    the edge is filled first, then the body over it, both by
    lib/outline.c's exact area and mixed in linear light. */
typedef struct {
    COMPOSE_SHAPE body; /**< the arrow, its hotspot at the origin */
    COMPOSE_SHAPE edge; /**< the arrow grown all round */
    UINT32 body_ink;    /**< the body's colour, 0x00RRGGBB */
    UINT32 edge_ink;    /**< the edge's colour, 0x00RRGGBB */
    UINT32 edge_percent; /**< how much of the edge's ink is mixed over
                              what lies beneath, 0 to COMPOSE_PERCENT */
} COMPOSE_POINTER;

/** The pointer as a frame draws it (display.md D19, rule 7): the theme's
    image at the screen's scale, the memory drawing it takes, and the
    pixel its hotspot is on once it has a place. Until it has one, which
    is the first move compose_cursor_move is given, no frame draws it. A
    zero COMPOSE_CURSOR but for image, quarters and work is a pointer
    with no place. */
typedef struct {
    const COMPOSE_POINTER *image; /**< the theme's pointer */
    UINT32 quarters;              /**< the screen's scale, in
                                       COMPOSE_QUARTERS to a scale of 1 */
    OUTLINE *work;                /**< the memory filling takes, which
                                       outline.h says is large */
    UINT32 placed;                /**< nonzero once the pointer has a
                                       place */
    INT64 x;                      /**< the column of the pixel whose top
                                       left corner is the hotspot */
    INT64 y;                      /**< its row */
} COMPOSE_CURSOR;

/** The most rectangles a frame composes for the pointer's moves: the
    image's old box and its new. */
#define COMPOSE_CURSOR_RECTS_MAX 2

/** What the pointer's moves between two frames damage (display.md D19,
    rule 7, and its note on the pointer's two boxes): the image's box
    where the screen shows it and its box where the pointer is now, each
    cut by the screen and in whole blocks of its framebuffer
    (compose_blocks). The two are kept apart from a frame's other damage
    and from each other, so that a move across the screen composes two
    images and nothing between them. All zero, no move has come since a
    frame; the compositor zeroes it once a frame has composed its
    rectangles (compose_cursor_rects). */
typedef struct {
    UINT32 moved; /**< nonzero once a move has come since the frame */
    RECT old;     /**< the image's box where the last frame drew it,
                       which the first move since that frame sets; no
                       pixel where no frame has drawn the image, or its
                       box lay off the screen */
    RECT now;     /**< its box where the pointer is, which every move
                       sets; no pixel where it lies off the screen */
} COMPOSE_CURSOR_DAMAGE;

/** Make the corners of radius, in OUTLINE_ONE units, 256ths of a pixel,
    as outline_rounded_rect takes it, so the console panel's radius at
    an 18 pixel cell, 22 pixels, is 5632: the coverage of a square of
    twice the radius rounded up on each side, from outline_coverage.
    Made once for a radius, whatever the windows that use it. work is
    the memory filling takes, which outline.h says is large. Returns 1;
    or 0 with corners unchanged if radius is negative or its pixels
    rounded up are past COMPOSE_REACH_MAX. A radius of 0 makes square
    corners. */
int compose_corners(OUTLINE *work, COMPOSE_CORNERS *corners, INT64 radius);

/** The coverage, 0 to COMPOSE_COVER_MAX, that the corners leave of the
    pixel at column dx and row dy of a window width pixels across and
    height high: COMPOSE_COVER_MAX anywhere but within the corners' reach
    of two edges, 0 outside the shape. For a window at least twice the
    reach across and high it is the pixel of outline_coverage of the
    window's rounded rectangle. For a smaller one the corners overlap
    and no rounded rectangle has their pixels: a pixel is one of the
    plane's, the left or top corner's where both reach it, and none
    lies outside the plane. The pixel lies in the window, and corners
    may be 0 for square ones. */
UINT32 compose_corner(const COMPOSE_CORNERS *corners, INT64 width,
                      INT64 height, INT64 dx, INT64 dy);

/** Make material from tint, 0xRRGGBB in sRGB codes, and amount, the
    percent of the tint mixed over the wallpaper, as the console mixes
    its panel: for each channel and code, gfx_cross_fade of the code
    toward the tint's. Returns 1; or 0 with material unchanged if amount
    is past COMPOSE_PERCENT. */
int compose_material(COMPOSE_MATERIAL *material, UINT32 tint,
                     UINT32 amount);

/** Compose the rectangle band holds, row by row: the wallpaper, then
    each of the count windows from the first, the lowest, to the last. A
    pixel of cover COMPOSE_COVER_MAX is the window's colour, copied. One
    of cover 0 is the window's material of the wallpaper beneath, three
    lookups. One between is the colour mixed over that material in
    linear light by its cover. A pixel in a window's corner is the
    window's pixel so made mixed over what is composed beneath by the
    corner's coverage (compose_corner). Whatever lies between a window
    and the wallpaper is not seen through it. The rectangle lies within
    the wallpaper, every material is set and every window's coordinates
    are within COMPOSE_COORDINATE_MAX. */
void compose_band(const COMPOSE_BAND *band, const COMPOSE_WALLPAPER *wallpaper,
                  const COMPOSE_WINDOW *windows, UINT32 count);

/** The rectangle to compose so that compose_write writes whole blocks
    of screen's framebuffer for rect: rect cut to the screen and
    widened, in each row, from the start of the block holding its left
    edge to the end of the block holding its last column, which are
    blocks of COMPOSE_BLOCK_BYTES at addresses that are multiples of it, and
    clipped to the screen's row. It holds the widening of every row, so
    it is wider than a row's where the rows' blocks do not line up, as
    when a row is not a whole number of blocks long. */
RECT compose_blocks(const SCREEN *screen, const RECT *rect);

/** Write rect of band to the screen, row by row, as each pixel's
    screen order gfx_pixel gives, its unused byte 0: each row from the
    start of the block holding rect's left edge to the end of the block
    holding its last column, cut to the screen's row and to the band's
    rectangle. The band holds compose_blocks of rect, or the writes are
    cut where it ends. Makes no stores visible: the caller calls
    gfx_flush when its frame is written. */
void compose_write(const COMPOSE_BAND *band, const SCREEN *screen,
                   const RECT *rect);

/** Why the kernel's background, as background labels it and as it holds
    sections sections, cannot be the wallpaper on display, a short phrase
    for the log; or 0 if it can: Limb, in a pixel format the composer
    reads, at the display's size, which Limb takes, in as many sections
    as an image that size needs (docs/spec/display.md D2 rule 2). */
const char *compose_wallpaper_refusal(const BOOTSTRAP_BACKGROUND *background,
                                      const BOOTSTRAP_DISPLAY *display,
                                      UINT32 sections);

/** Compose rect of the screen, which lies within it, in bands of rows
    rows from its top: each band composed in memory (compose_band) from
    the wallpaper and the count windows, lowest first, then the pointer's
    image drawn over it (compose_pointer) where cursor, if it is not 0,
    has a place, and written in whole blocks (compose_write), so that
    pixels beside rect's, in the blocks it touches, are written too, with
    the pixels the frame has there. A band the image's pixels
    (compose_pointer_box) do not touch is not drawn on, and one it does
    has the image drawn whole, cut by the band, so the screen's pixels
    are those of the whole screen composed and the image drawn over it,
    whatever rect is. memory holds the screen's width times rows pixels,
    room for a band whose blocks reach the width of the screen. The
    wallpaper is as wide as the screen and at least as high; with rows
    below 1, or a rect with no pixel, nothing is written. An image that
    compose_pointer refuses is drawn nowhere. Makes no stores visible: the
    caller calls gfx_flush. Returns the pixels it composed, those of
    every band's rectangle (compose_blocks), which are the pixels it
    wrote wherever each row of the screen's framebuffer starts a block;
    0 where nothing is written. */
UINT64 compose_frame(const SCREEN *screen, const COMPOSE_WALLPAPER *wallpaper,
                     const COMPOSE_WINDOW *windows, UINT32 count,
                     const RECT *rect, UINT32 *memory, INT64 rows,
                     const COMPOSE_CURSOR *cursor);

/** Write the wallpaper alone to the whole screen, a frame with no
    window, in bands of rows rows from the top: each band composed in
    memory (compose_band) and written in whole blocks (compose_write).
    memory holds the screen's width times rows pixels, room for a band
    whose blocks reach the width of the screen. The wallpaper is as wide
    as the screen and at least as high; with rows below 1 nothing is
    written. Makes no stores visible: the caller calls gfx_flush. */
void compose_wallpaper_frame(const SCREEN *screen,
                             const COMPOSE_WALLPAPER *wallpaper,
                             UINT32 *memory, INT64 rows);

/** Draw the pointer's image over band: the edge, then the body, each
    mixed in linear light by lib/outline.c's exact area, at a screen of
    scale quarters / COMPOSE_QUARTERS. The hotspot lies on the top left
    corner of pixel (x, y) of the screen, so the arrow covers the same
    pixels wherever it is. A shape's coordinates are scaled and rounded
    once, to the nearest 256th of a pixel, halves up. Pixels outside the
    band are not drawn. work is the memory filling takes, which
    outline.h says is large. Returns 1; or 0 with nothing drawn if a
    shape is malformed, holds more lines than OUTLINE_EDGES_MAX once its
    curves are cut, or reaches past OUTLINE_COORDINATE_MAX, if quarters
    is 0, edge_percent is past COMPOSE_PERCENT, the hotspot lies past
    COMPOSE_COORDINATE_MAX, or the band is wider than OUTLINE_WIDTH_MAX. */
int compose_pointer(OUTLINE *work, const COMPOSE_POINTER *pointer,
                    const COMPOSE_BAND *band, INT64 x, INT64 y,
                    UINT32 quarters);

/** Set *box to the pixels the pointer's image can change with its
    hotspot at the top left corner of pixel (x, y), at a scale of
    quarters / COMPOSE_QUARTERS: the smallest rectangle holding every
    pixel any of its outlines touches. Returns 1; or 0 with *box
    unchanged if compose_pointer would refuse the shapes, the scale, the
    opacity or the hotspot. */
int compose_pointer_box(OUTLINE *work, const COMPOSE_POINTER *pointer,
                        INT64 x, INT64 y, UINT32 quarters, RECT *box);

/** Move the pointer, which a frame draws where cursor says, to the pixel
    (x, y) of screen: the hotspot is that pixel's top left corner, wherever
    in the pixel the pointer's position is (display.md D19, rule 7), so a
    move within a pixel changes nothing. Records in *damage what the move
    damages. The first move since a frame, which finds damage->moved 0,
    sets damage->old to the image's box at the place it leaves, where the
    last frame drew it, or to no pixel if the pointer had no place; a
    later move leaves damage->old, since no frame drew the image at the
    places between. Every move sets damage->now to the image's box at
    (x, y). Each box is compose_pointer_box's for its pixel, as
    compose_blocks gives it for screen. cursor and *damage are unchanged
    where the pointer is on that pixel already, and where
    compose_pointer_box refuses the image at the scale, which then is
    drawn nowhere; the compositor never meets that, for the theme's arrow
    is within the rasterizer's reach at every scale a screen it draws on
    gives. */
void compose_cursor_move(COMPOSE_CURSOR *cursor, const SCREEN *screen,
                         INT64 x, INT64 y, COMPOSE_CURSOR_DAMAGE *damage);

/** The rectangles a frame composes for the pointer's moves since the
    last frame (display.md D19, the note on the pointer's two boxes):
    sets rects from rects[0] and returns how many it set, at most
    COMPOSE_CURSOR_RECTS_MAX. Where damage's old box and its new share no
    pixel they are each of them that has one, the old first, so that
    nothing between them is composed however far the pointer moved. Where
    they share one they are their join alone, the smallest rectangle that
    holds both, so that no pixel is composed twice. It returns 0 where no
    move has come or neither box has a pixel. A frame composed over each
    of them (compose_frame), with the cursor the moves were made on,
    shows the image at its new place and at no other. */
UINT32 compose_cursor_rects(const COMPOSE_CURSOR_DAMAGE *damage,
                            RECT rects[COMPOSE_CURSOR_RECTS_MAX]);

#endif
