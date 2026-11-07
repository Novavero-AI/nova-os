/* The rasterizer every image that draws shares: the coverage of
   geometric strokes, sampled at 16 points a pixel, and the mixing of sRGB
   codes by the light they encode. Integer arithmetic only, with no
   machine dependence, so a host program checks what it draws. */

#ifndef NOVA_RASTER_H
#define NOVA_RASTER_H

#include "types.h"

/* Geometry is in eighths of a pixel, and a pixel's coverage is how many
   of its COVERAGE_MAX sample points lie inside a stroke. */
#define SUBPIXEL 8
#define SAMPLES 4
#define COVERAGE_MAX (SAMPLES * SAMPLES)

#define CAPSULE 0 /* the segment (ax, ay)-(bx, by), radius r */
#define RING 1    /* centre (ax, ay), radius bx, half-width r */
#define ARCH 2    /* the upper half of a RING */
#define ARC 3     /* a RING from direction (sx, sy) clockwise to (ex, ey) */

/* A stroke. A RING's by and every shape's sx, sy, ex and ey but an
   ARC's are unused. An ARC's directions are vectors from its centre, and
   clockwise is as the screen shows it, with y growing downward. */
typedef struct {
    int kind;
    INT64 ax, ay, bx, by, r;
    INT64 sx, sy, ex, ey;
} SHAPE;

/* How many of pixel (x, y)'s COVERAGE_MAX sample points lie inside any
   of count shapes. */
INT64 gfx_coverage(const SHAPE *shapes, int count, INT64 x, INT64 y);

/* The sRGB code whose light mixes the light of codes from and to, to's
   weighted by weight of total and from's by the rest. */
INT64 gfx_cross_fade(INT64 from, INT64 to, INT64 weight, INT64 total);

#endif
