/*
 * bbox_overlay.c - see bbox_overlay.h.
 */
#include "bbox_overlay.h"

static void draw_hline(uint16_t *buf, uint16_t bufWidth, uint16_t bufHeight, int x0, int x1, int y, uint16_t color)
{
    if (y < 0 || y >= (int)bufHeight)
    {
        return;
    }
    if (x0 < 0)
    {
        x0 = 0;
    }
    if (x1 >= (int)bufWidth)
    {
        x1 = (int)bufWidth - 1;
    }
    for (int x = x0; x <= x1; x++)
    {
        buf[(uint32_t)y * bufWidth + (uint32_t)x] = color;
    }
}

static void draw_vline(uint16_t *buf, uint16_t bufWidth, uint16_t bufHeight, int y0, int y1, int x, uint16_t color)
{
    if (x < 0 || x >= (int)bufWidth)
    {
        return;
    }
    if (y0 < 0)
    {
        y0 = 0;
    }
    if (y1 >= (int)bufHeight)
    {
        y1 = (int)bufHeight - 1;
    }
    for (int y = y0; y <= y1; y++)
    {
        buf[(uint32_t)y * bufWidth + (uint32_t)x] = color;
    }
}

void BBOX_DrawRect(uint16_t *buf, uint16_t bufWidth, uint16_t bufHeight, int x, int y, int boxWidth, int boxHeight,
                    uint16_t color)
{
    int x1 = x + boxWidth;
    int y1 = y + boxHeight;

    /* 2px-thick outline so it's visible on a low-res panel. */
    draw_hline(buf, bufWidth, bufHeight, x, x1, y, color);
    draw_hline(buf, bufWidth, bufHeight, x, x1, y + 1, color);
    draw_hline(buf, bufWidth, bufHeight, x, x1, y1, color);
    draw_hline(buf, bufWidth, bufHeight, x, x1, y1 - 1, color);
    draw_vline(buf, bufWidth, bufHeight, y, y1, x, color);
    draw_vline(buf, bufWidth, bufHeight, y, y1, x + 1, color);
    draw_vline(buf, bufWidth, bufHeight, y, y1, x1, color);
    draw_vline(buf, bufWidth, bufHeight, y, y1, x1 - 1, color);
}
