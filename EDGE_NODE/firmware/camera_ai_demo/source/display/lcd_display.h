/*
 * lcd_display.h - selects the LCD backend main.c talks to.
 *
 * All three backends expose the identical API (LCD_Init/SetWindow/
 * PushPixels/DrawImage): lcd_spi_hw.h (Arduino header, current default -
 * hardware LPSPI1, shared with the microSD slot/touch controller, see
 * spi1_bus.h), lcd_bitbang.h (J8 header, bit-bang diagnostic), or
 * lcd_flexio_mculcd.h (J8 header, FlexIO - abandoned, see WORKLOG.md).
 * Selected by DEMO_LCD_ARDUINO_HEADER / DEMO_LCD_BITBANG (set globally by
 * CMakeLists.txt), so this file is the only place that needs to know which
 * one is active.
 */
#ifndef _LCD_DISPLAY_H_
#define _LCD_DISPLAY_H_

#include "app.h" /* for DEMO_LCD_ARDUINO_HEADER / DEMO_LCD_BITBANG */

#if DEMO_LCD_ARDUINO_HEADER
#include "lcd_spi_hw.h"
#elif DEMO_LCD_BITBANG
#include "lcd_bitbang.h"
#else
#include "lcd_flexio_mculcd.h"
#endif

#endif /* _LCD_DISPLAY_H_ */
