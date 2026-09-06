/*
 * lcd_bitbang.c - see lcd_bitbang.h
 *
 * Generic GPIO bit-bang 8080 8-bit LCD driver, pin-agnostic - drives the
 * abandoned J8 header's parallel-bus panel (LCD_BITBANG_DIAGNOSTIC). RD
 * is held high for this driver's whole lifetime (never reads from the
 * panel; a floating RD line caused bugs on the FlexIO path - see
 * WORKLOG.md). Deliberately unoptimized for reliability over speed - a
 * full frame push takes about half a second.
 */

#include "lcd_bitbang.h"
#include "app.h"
#include "board.h"
#include "fsl_debug_console.h"
#include "fsl_gpio.h"
#include <stdbool.h>

/* Delay between bus transitions, in microseconds - generous setup/hold
 * margin by default. 0 removes it entirely (fastest, riskiest); raise it
 * if the image comes out garbled/noisy. */
#ifndef LCD_BITBANG_DELAY_US
#define LCD_BITBANG_DELAY_US 0U
#endif

static void LCD_BitbangDelay(void) {
#if LCD_BITBANG_DELAY_US > 0U
  SDK_DelayAtLeastUs(LCD_BITBANG_DELAY_US, SystemCoreClock);
#endif
}

static void LCD_SetCSPin(bool set) {
  GPIO_PinWrite(DEMO_LCD_CS_GPIO, DEMO_LCD_CS_PIN, set ? 1U : 0U);
}

static void LCD_SetRSPin(bool set) {
  GPIO_PinWrite(DEMO_LCD_RS_GPIO, DEMO_LCD_RS_PIN, set ? 1U : 0U);
}

static void LCD_SetResetPin(bool set) {
  GPIO_PinWrite(DEMO_LCD_RST_GPIO, DEMO_LCD_RST_PIN, set ? 1U : 0U);
}

static void LCD_SetBacklight(bool on) {
  GPIO_PinWrite(DEMO_LCD_BLK_GPIO, DEMO_LCD_BLK_PIN, on ? 1U : 0U);
}

#if DEMO_LCD_ARDUINO_HEADER
/* Arduino-header pin mapping puts all 8 data lines on two GPIO instances
 * (GPIO0: D0/D1/D2/D4/D7, GPIO1: D3/D5/D6) - batched into one PSOR/PCOR
 * write per port instead of 8 separate GPIO_PinWrite() calls. Hardcoded to
 * this exact pin table; the #else fallback below is pin-agnostic. */
#define LCD_GPIO0_DATA_MASK                                                    \
  ((1UL << 28U) | (1UL << 10U) | (1UL << 29U) | (1UL << 30U) |                 \
   (1UL << 31U)) /* D0,D1,D2,D4,D7 */
#define LCD_GPIO1_DATA_MASK                                                    \
  ((1UL << 23U) | (1UL << 21U) | (1UL << 2U)) /* D3,D5,D6 */

static void LCD_SetDataBus(uint8_t value) {
  uint32_t gpio0Set = 0U;
  uint32_t gpio1Set = 0U;

  if ((value & (1U << 0U)) != 0U) {
    gpio0Set |= (1UL << 28U);
  } /* D0 */
  if ((value & (1U << 1U)) != 0U) {
    gpio0Set |= (1UL << 10U);
  } /* D1 */
  if ((value & (1U << 2U)) != 0U) {
    gpio0Set |= (1UL << 29U);
  } /* D2 */
  if ((value & (1U << 3U)) != 0U) {
    gpio1Set |= (1UL << 23U);
  } /* D3 */
  if ((value & (1U << 4U)) != 0U) {
    gpio0Set |= (1UL << 30U);
  } /* D4 */
  if ((value & (1U << 5U)) != 0U) {
    gpio1Set |= (1UL << 21U);
  } /* D5 */
  if ((value & (1U << 6U)) != 0U) {
    gpio1Set |= (1UL << 2U);
  } /* D6 */
  if ((value & (1U << 7U)) != 0U) {
    gpio0Set |= (1UL << 31U);
  } /* D7 */

  GPIO_PortSet(GPIO0, gpio0Set);
  GPIO_PortClear(GPIO0, LCD_GPIO0_DATA_MASK & ~gpio0Set);
  GPIO_PortSet(GPIO1, gpio1Set);
  GPIO_PortClear(GPIO1, LCD_GPIO1_DATA_MASK & ~gpio1Set);
}
#else
static void LCD_SetDataBus(uint8_t value) {
  GPIO_PinWrite(DEMO_LCD_D0_GPIO, DEMO_LCD_D0_PIN, (value >> 0U) & 1U);
  GPIO_PinWrite(DEMO_LCD_D1_GPIO, DEMO_LCD_D1_PIN, (value >> 1U) & 1U);
  GPIO_PinWrite(DEMO_LCD_D2_GPIO, DEMO_LCD_D2_PIN, (value >> 2U) & 1U);
  GPIO_PinWrite(DEMO_LCD_D3_GPIO, DEMO_LCD_D3_PIN, (value >> 3U) & 1U);
  GPIO_PinWrite(DEMO_LCD_D4_GPIO, DEMO_LCD_D4_PIN, (value >> 4U) & 1U);
  GPIO_PinWrite(DEMO_LCD_D5_GPIO, DEMO_LCD_D5_PIN, (value >> 5U) & 1U);
  GPIO_PinWrite(DEMO_LCD_D6_GPIO, DEMO_LCD_D6_PIN, (value >> 6U) & 1U);
  GPIO_PinWrite(DEMO_LCD_D7_GPIO, DEMO_LCD_D7_PIN, (value >> 7U) & 1U);
}
#endif

/* One 8080 write cycle: data must already be stable on the bus. WR idles
 * high; pulse it low then back high (data latched on the rising edge). */
static void LCD_PulseWR(void) {
  GPIO_PinWrite(DEMO_LCD_WR_GPIO, DEMO_LCD_WR_PIN, 0U);
  LCD_BitbangDelay();
  GPIO_PinWrite(DEMO_LCD_WR_GPIO, DEMO_LCD_WR_PIN, 1U);
  LCD_BitbangDelay();
}

static void LCD_WriteByte(uint8_t value) {
  LCD_SetDataBus(value);
  LCD_BitbangDelay(); /* Data setup time before WR falls. */
  LCD_PulseWR();
}

/* Command/data helpers below assume CS is already asserted (low) by the
 * caller, so LCD_SetWindow() can keep CS asserted across multiple commands
 * and LCD_PushPixels() closes it. */
static void LCD_WriteCommandOpen(uint8_t command) {
  LCD_SetRSPin(false); /* RS low = command */
  LCD_WriteByte(command);
  LCD_SetRSPin(true);
}

static void LCD_WriteDataArrayOpen(const uint8_t *data, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) {
    LCD_WriteByte(data[i]);
  }
}

/* Write one command byte with no data, in its own CS-bracketed transfer. */
static void LCD_WriteCommand(uint8_t command) {
  LCD_SetCSPin(false);
  LCD_WriteCommandOpen(command);
  LCD_SetCSPin(true);
}

/* Write one command byte followed by 1-4 data bytes, in one CS-bracketed
 * transfer. */
static void LCD_WriteCommandData(uint8_t command, const uint8_t *data,
                                 uint32_t len) {
  LCD_SetCSPin(false);
  LCD_WriteCommandOpen(command);
  LCD_WriteDataArrayOpen(data, len);
  LCD_SetCSPin(true);
}

static void LCD_InitGpioPins(void) {
  const gpio_pin_config_t outConfig = {.pinDirection = kGPIO_DigitalOutput,
                                       .outputLogic = 1};

  GPIO_PinInit(DEMO_LCD_RST_GPIO, DEMO_LCD_RST_PIN, &outConfig);
  GPIO_PinInit(DEMO_LCD_CS_GPIO, DEMO_LCD_CS_PIN, &outConfig);
  GPIO_PinInit(DEMO_LCD_RS_GPIO, DEMO_LCD_RS_PIN, &outConfig);
  GPIO_PinInit(DEMO_LCD_WR_GPIO, DEMO_LCD_WR_PIN, &outConfig);

  /* RD held high (inactive) for this driver's whole lifetime - see the
   * file header comment above. */
  GPIO_PinInit(DEMO_LCD_RD_GPIO, DEMO_LCD_RD_PIN, &outConfig);
  GPIO_PinWrite(DEMO_LCD_RD_GPIO, DEMO_LCD_RD_PIN, 1U);

  GPIO_PinInit(DEMO_LCD_D0_GPIO, DEMO_LCD_D0_PIN, &outConfig);
  GPIO_PinInit(DEMO_LCD_D1_GPIO, DEMO_LCD_D1_PIN, &outConfig);
  GPIO_PinInit(DEMO_LCD_D2_GPIO, DEMO_LCD_D2_PIN, &outConfig);
  GPIO_PinInit(DEMO_LCD_D3_GPIO, DEMO_LCD_D3_PIN, &outConfig);
  GPIO_PinInit(DEMO_LCD_D4_GPIO, DEMO_LCD_D4_PIN, &outConfig);
  GPIO_PinInit(DEMO_LCD_D5_GPIO, DEMO_LCD_D5_PIN, &outConfig);
  GPIO_PinInit(DEMO_LCD_D6_GPIO, DEMO_LCD_D6_PIN, &outConfig);
  GPIO_PinInit(DEMO_LCD_D7_GPIO, DEMO_LCD_D7_PIN, &outConfig);

  /* Backlight: init as output and turn on immediately. */
  GPIO_PinInit(DEMO_LCD_BLK_GPIO, DEMO_LCD_BLK_PIN, &outConfig);
  LCD_SetBacklight(true);
}

/* Generic MIPI-DCS init sequence, proven working against the physical
 * panel (see WORKLOG.md). */
static void LCD_InitPanel(void) {
  LCD_SetResetPin(false);
  SDK_DelayAtLeastUs(20000, SystemCoreClock);
  LCD_SetResetPin(true);
  SDK_DelayAtLeastUs(150000, SystemCoreClock);

  LCD_WriteCommand(0x01U); /* Software reset */
  SDK_DelayAtLeastUs(20000, SystemCoreClock);

  LCD_WriteCommand(0x11U); /* Sleep out */
  SDK_DelayAtLeastUs(150000, SystemCoreClock);

  /* MADCTL: memory access control. MV=1 (row/column exchange) matches this
   * panel's native 240x320 GRAM to the camera's 320x240 landscape buffer.
   * BGR=1 confirmed correct on real hardware (see WORKLOG.md). */
  LCD_WriteCommandData(0x36U, (const uint8_t[]){0x28U}, 1U);

  LCD_WriteCommandData(0x3AU, (const uint8_t[]){0x55U},
                       1U); /* Pixel format: 16bpp RGB565 */

  LCD_WriteCommand(0x29U); /* Display ON */
  SDK_DelayAtLeastUs(50000, SystemCoreClock);
}

void LCD_Init(void) {
#if DEMO_LCD_ARDUINO_HEADER
  PRINTF("LCD: bit-bang GPIO on the Arduino header\r\n");
#else
  PRINTF("LCD: bit-bang GPIO on J8\r\n");
#endif
  LCD_InitGpioPins();
  LCD_InitPanel();
}

void LCD_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  const uint8_t colBuf[4] = {(uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFFU),
                             (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFFU)};
  const uint8_t rowBuf[4] = {(uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFFU),
                             (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFFU)};

  LCD_SetCSPin(false);
  LCD_WriteCommandOpen(0x2AU); /* Column address set */
  LCD_WriteDataArrayOpen(colBuf, sizeof(colBuf));
  LCD_WriteCommandOpen(0x2BU); /* Page (row) address set */
  LCD_WriteDataArrayOpen(rowBuf, sizeof(rowBuf));
  LCD_WriteCommandOpen(0x2CU); /* Memory write - following bytes are pixels */
  /* CS stays asserted - LCD_PushPixels() closes it. */
}

void LCD_PushPixelsOpen(const uint16_t *pixels, uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    LCD_WriteByte((uint8_t)(pixels[i] >> 8));
    LCD_WriteByte((uint8_t)(pixels[i] & 0xFFU));
  }
}

void LCD_EndWindow(void) { LCD_SetCSPin(true); }

void LCD_PushPixels(const uint16_t *pixels, uint32_t count) {
  LCD_PushPixelsOpen(pixels, count);
  LCD_EndWindow();
}

void LCD_DrawImage(uint16_t x0, uint16_t y0, uint16_t width, uint16_t height,
                   const uint16_t *pixels) {
  LCD_SetWindow(x0, y0, (uint16_t)(x0 + width - 1U),
                (uint16_t)(y0 + height - 1U));
  LCD_PushPixels(pixels, (uint32_t)width * height);
}
