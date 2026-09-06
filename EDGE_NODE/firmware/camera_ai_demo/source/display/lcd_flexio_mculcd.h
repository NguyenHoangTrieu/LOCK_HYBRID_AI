/*
 * lcd_flexio_mculcd.h - hardware-accelerated 8-bit parallel LCD driver.
 *
 * Wraps the SDK's FLEXIO_MCULCD driver (FlexIO peripheral, MIPI-DBI Type-B
 * / 8080 bus) for the J8 FlexIO/LCD header, configured for an 8-bit data
 * bus (FLEXIO_MCULCD_DATA_BUS_WIDTH=8 in CMakeLists.txt). Abandoned path -
 * see WORKLOG.md. See ../../README.md for J8 pinout.
 */
#ifndef _LCD_FLEXIO_MCULCD_H_
#define _LCD_FLEXIO_MCULCD_H_

#include <stdint.h>

/*! @brief Init the FlexIO MCULCD bus and reset/configure the panel. */
void LCD_Init(void);

/*! @brief Set the active drawing window (inclusive pixel coordinates) and push pixels. */
void LCD_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/*! @brief Push `count` RGB565 pixels into the window set by LCD_SetWindow,
 *  then close the transfer. For a single push per window - most callers
 *  want this. */
void LCD_PushPixels(const uint16_t *pixels, uint32_t count);

/*! @brief Same as LCD_PushPixels(), but leaves the transfer open - for
 *  streaming several pushes into one LCD_SetWindow() call. Call
 *  LCD_EndWindow() after the last push. See lcd_bitbang.h for why calling
 *  LCD_PushPixels() in a loop instead is a bug. */
void LCD_PushPixelsOpen(const uint16_t *pixels, uint32_t count);

/*! @brief Close the transfer after one or more LCD_PushPixelsOpen() calls. */
void LCD_EndWindow(void);

/*! @brief Convenience: set window then push a full width*height block. */
void LCD_DrawImage(uint16_t x0, uint16_t y0, uint16_t width, uint16_t height, const uint16_t *pixels);

#endif /* _LCD_FLEXIO_MCULCD_H_ */
