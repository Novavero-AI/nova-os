/** @file
    Filled outlines: paths of lines and quadratic and cubic Bezier
    curves, filled by the nonzero winding rule with the exact area each
    pixel has inside them, written as coverage into a plane of bytes or
    mixed into a canvas in linear light with gfx/raster.h's sRGB tables
    (design/text/README.md). Integer arithmetic only, with no machine
    dependence, so a host build draws the same pixels as nova-os does;
    design/text/outline.py is its prototype.

    Coordinates are in OUTLINE_ONE units to a pixel, x growing right and
    y down, and lie within OUTLINE_COORDINATE_MAX of 0. */

#ifndef NOVA_OUTLINE_H
#define NOVA_OUTLINE_H

#include "types.h"

/** Units to a pixel, on each axis. */
#define OUTLINE_ONE 256

/** The furthest a coordinate lies from 0, in units: 65,536 pixels. */
#define OUTLINE_COORDINATE_MAX ((INT64)1 << 24)

/** A pixel's coverage when an outline fills it whole. */
#define OUTLINE_COVERAGE_MAX 255

/** An ink's opacity when it covers what is under it whole. */
#define OUTLINE_OPACITY_ONE 65536

/** The most lines an outline holds once its curves are cut into
    lines. */
#define OUTLINE_EDGES_MAX 4096

/** The widest canvas outline_fill draws on, and the widest plane
    outline_coverage writes, in pixels. */
#define OUTLINE_WIDTH_MAX 16384

/** Pixels in memory: width x height of them, each 0x00RRGGBB in sRGB
    codes, a row stride pixels after the one above it. */
typedef struct {
    UINT32 *pixels; /**< the top-left pixel */
    INT64 width;    /**< pixels across */
    INT64 height;   /**< pixels down */
    INT64 stride;   /**< pixels from a row's start to the next's */
} CANVAS;

/** A line of an outline, from (x0, y0) to (x1, y1), in units. */
typedef struct {
    INT64 x0; /**< where it starts, across */
    INT64 y0; /**< where it starts, down */
    INT64 x1; /**< where it ends, across */
    INT64 y1; /**< where it ends, down */
} OUTLINE_EDGE;

/** A part of an edge that crosses a pixel row: the edge, top end
    first, and the span of y the row holds of it. */
typedef struct {
    INT64 top_x;     /**< the edge's upper end, across */
    INT64 top_y;     /**< the edge's upper end, down */
    INT64 bottom_x;  /**< the edge's lower end, across */
    INT64 bottom_y;  /**< the edge's lower end, down */
    INT64 from;      /**< the first y in the row */
    INT64 to;        /**< the y past the last in the row */
    INT64 direction; /**< 1 if the path goes down it, -1 if up */
} OUTLINE_PIECE;

/** A piece's crossing of one slice of a row: its x at the slice's top
    and bottom, their sum, by which the slice orders its pieces, and its
    direction. */
typedef struct {
    INT64 key;       /**< top plus bottom */
    INT64 top;       /**< x at the slice's top */
    INT64 bottom;    /**< x at the slice's bottom */
    INT64 direction; /**< as its piece's */
} OUTLINE_CROSSING;

/** A path being built, as lines, and the memory filling it takes. An
    outline is large: a program keeps one, in static memory, and builds
    each path in it in turn. */
typedef struct {
    UINT32 count;  /**< lines in edges */
    UINT32 failed; /**< nonzero once a call could not be followed */
    UINT32 open;   /**< nonzero while a contour is being built */
    INT64 start_x; /**< the open contour's first point, across */
    INT64 start_y; /**< the open contour's first point, down */
    INT64 x;       /**< the current point, across */
    INT64 y;       /**< the current point, down */
    OUTLINE_EDGE edges[OUTLINE_EDGES_MAX];         /**< the lines */
    OUTLINE_PIECE pieces[OUTLINE_EDGES_MAX];       /**< a row's pieces */
    OUTLINE_CROSSING crossings[OUTLINE_EDGES_MAX]; /**< a slice's pieces */
    INT64 splits[OUTLINE_ONE + 1];                 /**< a row's slices */
    /** a row's areas, then its coverage */
    INT64 area[OUTLINE_WIDTH_MAX + 1];
} OUTLINE;

/** Empty outline, to build a path in it. */
void outline_start(OUTLINE *outline);

/** Close the open contour, if there is one, and open another at
    (x, y). */
void outline_move(OUTLINE *outline, INT64 x, INT64 y);

/** Add a line from the current point to (x, y), which becomes it. */
void outline_line(OUTLINE *outline, INT64 x, INT64 y);

/** Add a quadratic Bezier curve from the current point, pulled toward
    (cx, cy), to (x, y). It is cut into lines that stray from it by at
    most a sixteenth of a pixel, unless that would take more than 64. */
void outline_quad(OUTLINE *outline, INT64 cx, INT64 cy, INT64 x, INT64 y);

/** Add a cubic Bezier curve from the current point, pulled toward
    (c1x, c1y) and then (c2x, c2y), to (x, y), cut into lines as
    outline_quad's is. */
void outline_cubic(OUTLINE *outline, INT64 c1x, INT64 c1y, INT64 c2x,
                   INT64 c2y, INT64 x, INT64 y);

/** Close the open contour with a line back to its first point. */
void outline_close(OUTLINE *outline);

/** Add a contour of its own: the rectangle from (left, top) to
    (right, bottom), its corners rounded to circular arcs of radius
    radius, at most half its shorter side, each a cubic Bezier curve. */
void outline_rounded_rect(OUTLINE *outline, INT64 left, INT64 top,
                          INT64 right, INT64 bottom, INT64 radius);

/** Add a contour of its own: the circle of radius radius about
    (x, y), four cubic Bezier curves. */
void outline_circle(OUTLINE *outline, INT64 x, INT64 y, INT64 radius);

/** Close the open contour, then write into cover, a plane of width x
    height bytes, a row after the row above it, the coverage of each
    pixel: the share of its area the outline's nonzero winding covers,
    from 0 where it covers none to OUTLINE_COVERAGE_MAX where it covers
    the whole pixel. The pixel at column x and row y of the plane is the
    one whose top left corner lies x and y pixels from the outline's
    origin, as in outline_fill's canvas. Every byte of the plane is
    written. Returns 1; or 0 with nothing written if a call building the
    outline could not be followed (more than OUTLINE_EDGES_MAX lines, a
    coordinate or radius past OUTLINE_COORDINATE_MAX, a line with no
    contour open), width is past OUTLINE_WIDTH_MAX, or width or height is
    negative. */
int outline_coverage(OUTLINE *outline, UINT8 *cover, INT64 width,
                     INT64 height);

/** Close the open contour, then mix ink, 0x00RRGGBB in sRGB codes, at
    opacity of OUTLINE_OPACITY_ONE, into each pixel of canvas the
    outline covers, by outline_coverage's coverage of the pixel, in
    linear light; a pixel it covers none of is left as it is. Returns 1;
    or 0 with nothing drawn if a call building the outline could not be
    followed (more than OUTLINE_EDGES_MAX lines, a coordinate or radius
    past OUTLINE_COORDINATE_MAX, a line with no contour open), canvas is
    wider than OUTLINE_WIDTH_MAX, or opacity is past
    OUTLINE_OPACITY_ONE. */
int outline_fill(OUTLINE *outline, const CANVAS *canvas, UINT32 ink,
                 UINT32 opacity);

#endif
