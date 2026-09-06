/*
 * bbox_overlay.h - draws bounding-box rectangles into an RGB565 framebuffer
 * before it's pushed to the LCD, so AI detections can be checked visually.
 */
#ifndef _BBOX_OVERLAY_H_
#define _BBOX_OVERLAY_H_

#include <stdint.h>

/*!
 * @brief Draw a 2px-thick rectangle outline into an RGB565 buffer.
 *
 * Coordinates/size are clipped to the buffer bounds - safe to call with a
 * box that partially or fully falls outside [0,bufWidth)x[0,bufHeight).
 *
 * @param buf       RGB565 buffer, bufWidth*bufHeight pixels.
 * @param bufWidth  buffer width in pixels.
 * @param bufHeight buffer height in pixels.
 * @param x,y       top-left corner of the box, in buf's pixel space.
 * @param boxWidth,boxHeight box size, in buf's pixel space.
 * @param color     RGB565 color.
 */
void BBOX_DrawRect(uint16_t *buf, uint16_t bufWidth, uint16_t bufHeight, int x, int y, int boxWidth, int boxHeight,
                    uint16_t color);

#endif /* _BBOX_OVERLAY_H_ */
