/* Limb, the desktop background: the night side of a planet far larger
   than the screen, its edge catching the first light of a sun just below
   it. The shading is design/backgrounds/limb.py's, in the same integer
   arithmetic, so each pixel equals the prototype's at every screen size
   limb_init accepts. */

#ifndef NOVA_LIMB_H
#define NOVA_LIMB_H

#include "types.h"

/* The sides limb_init accepts, in pixels. From the least, every glow has
   a width: the narrowest along the edge, W * 9 / 100 pixels, has one from
   12 columns, and the narrowest across it, H / 2000 pixels in Q8, from 8
   rows; a glow without one would divide zero by zero. Up to the most,
   every product stays within 63 bits whatever the screen's shape. */
#define LIMB_MIN_SIDE 12
#define LIMB_MAX_SIDE 16384

/* Limb's geometry for one screen size: the planet's centre and radius,
   the sun's column, and the squared width of each glow across the edge
   (in Q8 pixels) and along it (in pixels). */
typedef struct {
    INT64 cx, cy, radius, sun_x;
    INT64 rim_out, rim_in, band_out, band_in;
    INT64 warm_out, warm_in, haze_out, night_in;
    INT64 along_rim, along_band, along_warm, along_haze;
} LIMB;

/* Set limb up for a width x height screen. Returns 0, with limb unset, if
   a side lies outside LIMB_MIN_SIDE..LIMB_MAX_SIDE. */
int limb_init(LIMB *limb, INT64 width, INT64 height);

/* Pixel (x, y) as 0x00RRGGBB, each channel an sRGB code. */
UINT32 limb_pixel(const LIMB *limb, INT64 x, INT64 y);

#endif
