/* Writes the console font's masks as gfx/font.c renders them for cells
   WIDTH pixels wide, to standard output as a binary PGM, for
   check-font.sh to compare with the prototype's: every mask, 16 to a row
   in code order, each pixel its coverage, 0 to COVERAGE_MAX. Built for
   the host. */

#include <stdio.h>
#include <stdlib.h>

#include "font.h"

#define ATLAS_COLUMNS 16

/* A width given in decimal, or -1 if text is not one. */
static long width_of(const char *text)
{
    char *end;
    long n = strtol(text, &end, 10);

    return *text != '\0' && *end == '\0' ? n : -1;
}

int main(int argc, char **argv)
{
    long width, height, rows, row, y, column;
    UINT8 *masks;

    if (argc != 2) {
        fprintf(stderr, "usage: %s WIDTH > atlas.pgm\n", argv[0]);
        return 2;
    }
    width = width_of(argv[1]);
    if (width < FONT_WIDTH_MIN || width > FONT_WIDTH_MAX) {
        fprintf(stderr, "%s: a cell is %d to %d pixels wide\n", argv[0],
                FONT_WIDTH_MIN, FONT_WIDTH_MAX);
        return 2;
    }
    height = font_height(width);
    masks = malloc(font_masks_size(width));
    if (!masks) {
        perror(argv[0]);
        return 1;
    }
    font_render(width, masks);
    rows = (FONT_MASKS + ATLAS_COLUMNS - 1) / ATLAS_COLUMNS;
    printf("P5\n%ld %ld\n%d\n", ATLAS_COLUMNS * width, rows * height,
           COVERAGE_MAX);
    for (row = 0; row < rows; row++)
        for (y = 0; y < height; y++)
            for (column = 0; column < ATLAS_COLUMNS; column++) {
                long index = row * ATLAS_COLUMNS + column;

                if (index < FONT_MASKS)
                    fwrite(font_mask(masks, width,
                                     (UINT32)(FONT_FIRST + index)) +
                               y * width,
                           1, (size_t)width, stdout);
                else
                    for (long x = 0; x < width; x++)
                        putchar(0);
            }
    free(masks);
    if (fflush(stdout) != 0 || ferror(stdout)) {
        perror(argv[0]);
        return 1;
    }
    return 0;
}
