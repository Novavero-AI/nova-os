/* The rules of a window (docs/spec/display.md D6 and D7, D19's phase 3
   arrangement, rules 4 and 5), as decisions over message fields and a
   WINDOW_RECORD. See window.h. */

#include "window.h"

#include "compose.h"
#include "native_abi.h"

/* The share of the area's height above a window's centre, which is the
   console panel's (D6, Placement; console.md C5). */
#define CENTRE_PERCENT 40
#define PERCENT 100

/* A pixel's bytes in a buffer (D6, Buffers). */
#define PIXEL_BYTES 4

/* The most a buffer holds: a message carries at most CHANNEL_MAX_HANDLES
   sections, of SECTION_MAX_SIZE each but the last (D6, Buffers). */
#define BUFFER_BYTES_MAX ((UINT64)CHANNEL_MAX_HANDLES * SECTION_MAX_SIZE)

#define UINT32_GREATEST 0xFFFFFFFFu
#define UINT64_GREATEST 0xFFFFFFFFFFFFFFFFull

static UINT32 smaller(UINT32 a, UINT32 b) { return a < b ? a : b; }

/* The area's depth below the strip; 0 where the strip fills the screen. */
static UINT32 area_height(const WINDOW_AREA *area)
{
    return area->strip < area->screen_height
               ? area->screen_height - area->strip
               : 0;
}

static UINT32 fit(UINT32 value, UINT32 low, UINT32 high)
{
    return value < low ? low : value > high ? high : value;
}

UINT32 window_scale(UINT32 screen_height)
{
    UINT64 quarters =
        ((UINT64)screen_height * DISPLAY_SCALE_ONE + WINDOW_SCALE_HEIGHT / 2) /
        WINDOW_SCALE_HEIGHT;

    return quarters < 1 ? 1 : (UINT32)quarters;
}

UINT32 window_state(UINT32 layer)
{
    return layer == DISPLAY_LAYER_SYSTEM ? WINDOW_STATE_FOCUSED : 0;
}

static int has_pixels(const RECT *rect)
{
    return rect->left < rect->right && rect->top < rect->bottom;
}

void window_damage_join(RECT *total, const RECT *rect)
{
    if (!has_pixels(rect))
        return;
    if (!has_pixels(total)) {
        total->left = rect->left;
        total->top = rect->top;
        total->right = rect->right;
        total->bottom = rect->bottom;
        return;
    }
    if (rect->left < total->left)
        total->left = rect->left;
    if (rect->top < total->top)
        total->top = rect->top;
    if (rect->right > total->right)
        total->right = rect->right;
    if (rect->bottom > total->bottom)
        total->bottom = rect->bottom;
}

void window_size(const WINDOW_AREA *area, UINT32 reach, UINT32 width,
                 UINT32 height, UINT32 *fitted_width, UINT32 *fitted_height)
{
    UINT32 greatest_width = smaller(area->screen_width, WINDOW_WIDTH_MAX);
    UINT32 greatest_height = smaller(area_height(area), WINDOW_HEIGHT_MAX);
    UINT64 least = 2 * (UINT64)reach;
    UINT32 least_width =
        least < greatest_width ? (UINT32)least : greatest_width;
    UINT32 least_height =
        least < greatest_height ? (UINT32)least : greatest_height;

    *fitted_width = fit(width, least_width, greatest_width);
    *fitted_height = fit(height, least_height, greatest_height);
}

/* D6 also lowers a top to at most the screen's height less the window's,
   where that is not below the strip. For a window within the area that
   never applies. With D the area's depth and h at most D, that bound is
   the strip plus D less h, which is not below the strip, and it exceeds
   the top before the raise, the strip plus floor(D * 40 / 100) less
   floor(h / 2), by ceil(D * 60 / 100) less ceil(h / 2), which is not
   negative since ceil(h / 2) is at most ceil(D / 2). So neither that top
   nor the strip, to which it is raised, is past the bound. For a taller
   window the bound is the strip plus D less h, below the strip, where D6
   does not apply it. */
void window_place(const WINDOW_AREA *area, UINT32 width, UINT32 height,
                  UINT32 *x, UINT32 *y)
{
    INT64 strip = area->strip;
    INT64 top = strip +
                (INT64)((UINT64)area_height(area) * CENTRE_PERCENT / PERCENT) -
                (INT64)(height / 2);

    *x = width < area->screen_width ? (area->screen_width - width) / 2 : 0;
    *y = (UINT32)(top < strip ? strip : top);
}

UINT32 window_open(const WINDOW_AREA *area, UINT32 reach, UINT32 layers,
                   UINT32 open, const DISPLAY_WINDOW_REQUEST *request,
                   WINDOW_RECORD *window)
{
    UINT32 layer = request->layer;
    UINT32 width, height, i;

    if (layer == 0 || (layer & (layer - 1)) != 0 || (layer & ~layers) != 0)
        return ERR_ACCESS_DENIED;
    if (request->surface != DISPLAY_SURFACE_PANEL)
        return ERR_NOT_SUPPORTED;
    if (open >= WINDOW_SERVED_MAX || area->screen_width == 0 ||
        area_height(area) == 0)
        return ERR_NO_RESOURCES;
    window_size(area, reach, request->width, request->height, &width, &height);
    window_place(area, width, height, &window->x, &window->y);
    window->width = width;
    window->height = height;
    window->layer = layer;
    window->surface = request->surface;
    window->serial = 0;
    window->shown = WINDOW_NO_BUFFER;
    window->owed = 0;
    for (i = 0; i < WINDOW_BUFFERS_MAX; i++) {
        window->buffers[i].given = 0;
        window->buffers[i].width = 0;
        window->buffers[i].height = 0;
        window->buffers[i].stride = 0;
    }
    return OK;
}

UINT32 window_configure(WINDOW_RECORD *window)
{
    if (window->serial < UINT32_GREATEST)
        window->serial++;
    return window->serial;
}

UINT64 window_buffer_size(UINT32 stride, UINT32 height)
{
    UINT64 bytes;

    if (height != 0 && stride > UINT64_GREATEST / PIXEL_BYTES / height)
        return UINT64_GREATEST;
    bytes = (UINT64)stride * height * PIXEL_BYTES;
    if (bytes > UINT64_GREATEST - (PAGE_SIZE - 1))
        return UINT64_GREATEST;
    return (bytes + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE;
}

UINT64 window_buffer_sections(UINT64 size)
{
    return size / SECTION_MAX_SIZE + (size % SECTION_MAX_SIZE != 0);
}

UINT32 window_give_buffer(WINDOW_RECORD *window,
                          const WINDOW_BUFFER_REQUEST *request,
                          const UINT64 *section_sizes, UINT32 count)
{
    UINT32 number = request->buffer;
    UINT64 size, sections;
    UINT32 i;
    WINDOW_HELD_BUFFER *held;

    if (number > WINDOW_BUFFER_LAST || number == window->shown)
        return ERR_INVALID_ARGS;
    if (request->width < 1 || request->height < 1 ||
        request->stride < request->width)
        return ERR_INVALID_ARGS;
    size = window_buffer_size(request->stride, request->height);
    if (size > BUFFER_BYTES_MAX)
        return ERR_INVALID_ARGS;
    sections = window_buffer_sections(size);
    /* A buffer has a pixel, so sections is at least 1 and so is count. */
    if (count != sections)
        return ERR_INVALID_ARGS;
    for (i = 0; i + 1 < count; i++)
        if (section_sizes[i] != SECTION_MAX_SIZE)
            return ERR_INVALID_ARGS;
    if (section_sizes[count - 1] !=
        size - (UINT64)(count - 1) * SECTION_MAX_SIZE)
        return ERR_INVALID_ARGS;
    held = &window->buffers[number];
    held->given = 1;
    held->width = request->width;
    held->height = request->height;
    held->stride = request->stride;
    return OK;
}

/* Add the blocks of the rectangle left, top, right, bottom, in window-local
   pixels, to presented's damage; nothing for one with no pixels, which
   compose_blocks returns empty. */
static void add_damage(const WINDOW_RECORD *window, const SCREEN *screen,
                       UINT64 left, UINT64 top, UINT64 right, UINT64 bottom,
                       WINDOW_PRESENTED *presented)
{
    RECT rect, blocks;

    rect.left = (INT64)window->x + (INT64)left;
    rect.top = (INT64)window->y + (INT64)top;
    rect.right = (INT64)window->x + (INT64)right;
    rect.bottom = (INT64)window->y + (INT64)bottom;
    blocks = compose_blocks(screen, &rect);
    if (blocks.left < blocks.right && blocks.top < blocks.bottom)
        presented->damage[presented->count++] = blocks;
}

UINT32 window_present(WINDOW_RECORD *window, const SCREEN *screen,
                      const WINDOW_PRESENT_REQUEST *request,
                      const WINDOW_RECT *rects, WINDOW_PRESENTED *presented)
{
    UINT32 number = request->buffer;
    const WINDOW_HELD_BUFFER *buffer;
    UINT32 columns, rows, i;
    int whole;

    if (number > WINDOW_BUFFER_LAST || !window->buffers[number].given ||
        request->rectangles > WINDOW_DAMAGE_MAX)
        return ERR_INVALID_ARGS;
    buffer = &window->buffers[number];
    presented->shown = 0;
    presented->released = 0;
    presented->count = 0;
    if (request->serial == 0 || request->serial != window->serial) {
        window->owed = 1;
        if (number != window->shown)
            presented->released = (UINT32)1 << number;
        return OK;
    }
    whole = window->owed || window->shown == WINDOW_NO_BUFFER ||
            window->buffers[window->shown].width != buffer->width ||
            window->buffers[window->shown].height != buffer->height;
    columns = smaller(buffer->width, window->width);
    rows = smaller(buffer->height, window->height);
    if (whole)
        add_damage(window, screen, 0, 0, window->width, window->height,
                   presented);
    else if (request->rectangles == 0)
        add_damage(window, screen, 0, 0, columns, rows, presented);
    else
        /* A rectangle that starts at or past the buffer's edge is cut to a
           right or bottom edge that is not after its own left or top, so
           add_damage drops it; it needs no cut of its start. */
        for (i = 0; i < request->rectangles; i++) {
            UINT64 right = (UINT64)rects[i].x + rects[i].width;
            UINT64 bottom = (UINT64)rects[i].y + rects[i].height;

            add_damage(window, screen, rects[i].x, rects[i].y,
                       right < columns ? right : columns,
                       bottom < rows ? bottom : rows, presented);
        }
    window->owed = 0;
    if (window->shown != WINDOW_NO_BUFFER && window->shown != number)
        presented->released = (UINT32)1 << window->shown;
    window->shown = number;
    presented->shown = 1;
    return OK;
}
