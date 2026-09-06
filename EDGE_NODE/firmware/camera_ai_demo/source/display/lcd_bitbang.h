/*
 * lcd_bitbang.h - GPIO bit-bang LCD driver (8080 8-bit bus).
 *
 * Same public API as lcd_flexio_mculcd.h and lcd_spi_bitbang.h (the
 * Arduino header's SPI panel driver). Pin set comes from app.h's
 * DEMO_LCD_* macros - J8 header only (see CMakeLists.txt's
 * LCD_BITBANG_DIAGNOSTIC).
 */
#ifndef _LCD_BITBANG_H_
#define _LCD_BITBANG_H_

#include <stdint.h>

/*! @brief Init the bit-banged GPIO bus and reset/configure the panel. */
void LCD_Init(void);

/*! @brief Set the active drawing window (inclusive pixel coordinates) and push pixels. */
void LCD_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/*! @brief Push `count` RGB565 pixels into the window set by LCD_SetWindow,
 *  then close the transfer (deassert CS). For a single push per window -
 *  most callers want this. */
void LCD_PushPixels(const uint16_t *pixels, uint32_t count);

/*! @brief Same as LCD_PushPixels(), but leaves the transfer open (CS still
 *  asserted) - for streaming several pushes into one LCD_SetWindow() call
 *  (e.g. one row buffer reused across many rows). Call LCD_EndWindow()
 *  after the last push. Calling LCD_PushPixels() in a loop instead is a
 *  bug: it closes CS after every call, so only the first push actually
 *  reaches the panel. */
void LCD_PushPixelsOpen(const uint16_t *pixels, uint32_t count);

/*! @brief Close the transfer (deassert CS) after one or more
 *  LCD_PushPixelsOpen() calls. */
void LCD_EndWindow(void);

/*! @brief Convenience: set window then push a full width*height block. */
void LCD_DrawImage(uint16_t x0, uint16_t y0, uint16_t width, uint16_t height, const uint16_t *pixels);

#endif /* _LCD_BITBANG_H_ */
