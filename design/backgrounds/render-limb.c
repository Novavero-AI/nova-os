/* Writes Limb as gfx/limb.c draws it, WIDTH x HEIGHT, to standard output
   as a binary PPM, for check-limb.sh to compare with the prototype's
   render. Built for the host. */

#include <stdio.h>
#include <stdlib.h>

#include "limb.h"

#define PPM_MAXVAL 255
#define CHANNEL_MASK 0xFFu

/* A side given in decimal, or -1 if text is not one. */
static long side(const char *text)
{
    char *end;
    long n = strtol(text, &end, 10);

    return *text != '\0' && *end == '\0' ? n : -1;
}

int main(int argc, char **argv)
{
    LIMB limb;
    long width, height, x, y;

    if (argc != 3) {
        fprintf(stderr, "usage: %s WIDTH HEIGHT > limb.ppm\n", argv[0]);
        return 2;
    }
    width = side(argv[1]);
    height = side(argv[2]);
    if (!limb_init(&limb, width, height)) {
        fprintf(stderr, "%s: each side must be %d to %d pixels\n", argv[0],
                LIMB_MIN_SIDE, LIMB_MAX_SIDE);
        return 2;
    }
    printf("P6\n%ld %ld\n%d\n", width, height, PPM_MAXVAL);
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++) {
            UINT32 pixel = limb_pixel(&limb, x, y);

            putchar((int)(pixel >> 16 & CHANNEL_MASK));
            putchar((int)(pixel >> 8 & CHANNEL_MASK));
            putchar((int)(pixel & CHANNEL_MASK));
        }
    if (fflush(stdout) != 0 || ferror(stdout)) {
        perror(argv[0]);
        return 1;
    }
    return 0;
}
