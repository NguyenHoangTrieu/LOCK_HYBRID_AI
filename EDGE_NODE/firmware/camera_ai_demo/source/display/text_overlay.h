/*
 * text_overlay.h - draws a text string straight to the LCD using font5x7.h,
 * one small reusable row buffer at a time (no full-screen framebuffer).
 */
#ifndef _TEXT_OVERLAY_H_
#define _TEXT_OVERLAY_H_

#include <stdint.h>

/*!
 * @brief Draw `str` at (x, y) in the panel's own pixel space.
 *
 * @param str    characters not in font5x7.h's small glyph set fall back to
 *               a blank space.
 * @param scale  each font pixel is drawn as a scale x scale block - e.g.
 *               scale=3 renders each glyph at 15x21 panel pixels.
 */
void TEXT_DrawString(uint16_t x, uint16_t y, const char *str, uint16_t fgColor, uint16_t bgColor, uint8_t scale);

#endif /* _TEXT_OVERLAY_H_ */
