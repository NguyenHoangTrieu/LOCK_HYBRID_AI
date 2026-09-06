/*
 * lcd_spi_hw.h - hardware LPSPI1 LCD driver (4-wire SPI, mode 0), shared
 * bus.
 *
 * Drives the Arduino-header 2.4" SPI TFT module (ILI9341-family: CS/RESET/
 * DC/SDI(MOSI)/SCK/LED) over the same physical LPSPI1 bus as the onboard
 * microSD slot and touch controller - see spi1_bus.h for how the sharing
 * works. Same public API as lcd_bitbang.h (the earlier 8-bit-parallel/8080
 * driver, still used for the abandoned J8 header path) and
 * lcd_flexio_mculcd.h, so main.c/text_overlay.c/bbox_overlay.c don't
 * change no matter which backend is active. Pin set comes from app.h's
 * DEMO_LCD_* macros.
 */
#ifndef _LCD_SPI_HW_H_
#define _LCD_SPI_HW_H_

#include <stdint.h>

/*! @brief Init the shared SPI bus (if not already done) and reset/
 *  configure the panel. */
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

#endif /* _LCD_SPI_HW_H_ */
