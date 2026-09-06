/*
 * touch_xpt2046.h - XPT2046 resistive touch controller, Arduino header.
 *
 * Wired up (pins reserved, SPI bus shared with the LCD/microSD slot - see
 * spi1_bus.h) but NOT called from anywhere in main.c - this project's
 * face-detection pipeline has no touch UI. Provided so the panel's touch
 * pins aren't left dangling/unused: TOUCH_Init()/TOUCH_IsPressed()/
 * TOUCH_ReadRaw() are ready for a future feature to build on.
 *
 * TOUCH_ReadRaw() returns raw, UNCALIBRATED 12-bit ADC counts (0-4095),
 * not screen pixel coordinates - converting to pixels needs a per-panel
 * calibration (4-point or corner-based) that can only be done against
 * real hardware. X/Y channel mapping and rotation are also unverified -
 * see the .c file's header comment and README.md's Known Limitations.
 *
 * UNTESTED ON REAL HARDWARE - see WORKLOG.md.
 */
#ifndef _TOUCH_XPT2046_H_
#define _TOUCH_XPT2046_H_

#include <stdbool.h>
#include <stdint.h>

/*! @brief Init the shared SPI bus (if not already done). Pins themselves
 *  are configured by pin_mux.c's BOARD_InitTouchPins(), called
 *  unconditionally from hardware_init.c alongside the LCD/SD pin init. */
void TOUCH_Init(void);

/*! @brief True if the panel is currently being touched (T_IRQ asserted
 *  low) - cheap to poll every frame, no SPI transfer needed. */
bool TOUCH_IsPressed(void);

/*! @brief Read raw (uncalibrated) 12-bit X/Y ADC counts over SPI. Only
 *  meaningful while TOUCH_IsPressed() is true - a read while not pressed
 *  returns noise. */
void TOUCH_ReadRaw(uint16_t *x, uint16_t *y);

#endif /* _TOUCH_XPT2046_H_ */
