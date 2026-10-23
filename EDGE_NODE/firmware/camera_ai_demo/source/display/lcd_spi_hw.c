/*
 * lcd_spi_hw.c - see lcd_spi_hw.h
 *
 * Hardware LPSPI1 SPI (mode 0, MSB first) LCD driver for the Arduino
 * header's 2.4" SPI TFT module (ILI9341-family), sharing the bus with the
 * onboard microSD slot and touch controller - see spi1_bus.h for the
 * sharing contract. CS/DC/RST/BLK are plain GPIO; SCK/SDI/SDO ride LPSPI1.
 *
 * Confirmed on real hardware: correct orientation, no bus-sharing
 * corruption, 7fps at 24MHz SPI (see WORKLOG.md for the fps-tuning trail
 * and the eDMA rewrite that was attempted and abandoned - hung on real
 * hardware, root cause not found - spi1_bus.c's SPI1_BUS_TransferBytesDMA()
 * is left in place, unused, for a future session).
 */

#include "lcd_spi_hw.h"
#include "app.h"
#include "board.h"
#include "fsl_common.h" /* SystemCoreClock - used by SDK_DelayAtLeastUs() below. */
#include "fsl_debug_console.h"
#include "fsl_gpio.h"
#include "fsl_lpspi.h"
#include "spi1_bus.h"
#include <stdbool.h>

/* One 320x240 RGB565 frame is 153,600 bytes = 1,228,800 bits - at 37.5MHz
 * (PLL0 150MHz / 2 = 75MHz source clock, / 2 LPSPI divider) that's a
 * ~32.8ms wire-time floor (~30fps best case). If the image comes back
 * glitchy/torn on long jumper wires, lower this toward 24MHz. */
#ifndef LCD_SPI_BAUDRATE_HZ
#define LCD_SPI_BAUDRATE_HZ 37500000U
#endif

static void LCD_SetCSPin(bool set) {
  GPIO_PinWrite(DEMO_LCD_CS_GPIO, DEMO_LCD_CS_PIN, set ? 1U : 0U);
}

static void LCD_SetDCPin(bool set) {
  GPIO_PinWrite(DEMO_LCD_DC_GPIO, DEMO_LCD_DC_PIN, set ? 1U : 0U);
}

static void LCD_SetResetPin(bool set) {
  GPIO_PinWrite(DEMO_LCD_RST_GPIO, DEMO_LCD_RST_PIN, set ? 1U : 0U);
}

static void LCD_SetBacklight(bool on) {
#if defined(DEMO_LCD_HAS_BLK_PIN) && DEMO_LCD_HAS_BLK_PIN
  GPIO_PinWrite(DEMO_LCD_BLK_GPIO, DEMO_LCD_BLK_PIN, on ? 1U : 0U);
#else
  (void)on;
#endif
}

/* kLPSPI_MasterPcs1 is never muxed to a physical pin here - it's just the
 * "don't care" PCS value the transfer API requires; the real chip select
 * is DEMO_LCD_CS_GPIO/PIN, plain GPIO. kLPSPI_MasterPcsContinuous still
 * matters despite that: without it, fsl_lpspi.c inserts a full PCS
 * setup/hold delay between every single byte regardless of physical
 * wiring, which dominated fps until fixed (confirmed on real hardware -
 * see WORKLOG.md). */
static void LCD_WriteByte(uint8_t value) {
  (void)SPI1_BUS_TransferBlocking(&value, NULL, 1U, kLPSPI_MasterPcs1 | kLPSPI_MasterPcsContinuous);
}

/* Reclaim the shared bus at the LCD's own rate, then assert CS - the bus
 * may have been left at a different rate/PCS by another device. */
static void LCD_BeginTransaction(void) {
  (void)SPI1_BUS_SetBaudRate(LCD_SPI_BAUDRATE_HZ);
  LCD_SetCSPin(false);
}

/* Command/data helpers below assume CS is already asserted by the caller,
 * so LCD_SetWindow() can keep CS asserted across multiple commands and
 * LCD_PushPixels() closes it. */
static void LCD_WriteCommandOpen(uint8_t command) {
  LCD_SetDCPin(false); /* DC low = command */
  LCD_WriteByte(command);
  LCD_SetDCPin(true);
}

static void LCD_WriteDataArrayOpen(const uint8_t *data, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) {
    LCD_WriteByte(data[i]);
  }
}

/* Write one command byte with no data, in its own CS-bracketed transfer. */
static void LCD_WriteCommand(uint8_t command) {
  LCD_BeginTransaction();
  LCD_WriteCommandOpen(command);
  LCD_SetCSPin(true);
}

/* Write one command byte followed by 1-4 data bytes, in one CS-bracketed
 * transfer. */
static void LCD_WriteCommandData(uint8_t command, const uint8_t *data,
                                 uint32_t len) {
  LCD_BeginTransaction();
  LCD_WriteCommandOpen(command);
  LCD_WriteDataArrayOpen(data, len);
  LCD_SetCSPin(true);
}

static void LCD_InitGpioPins(void) {
  const gpio_pin_config_t outConfig = {.pinDirection = kGPIO_DigitalOutput,
                                       .outputLogic = 0};
  const gpio_pin_config_t idleHighConfig = {.pinDirection = kGPIO_DigitalOutput,
                                            .outputLogic = 1};

  GPIO_PinInit(DEMO_LCD_RST_GPIO, DEMO_LCD_RST_PIN, &idleHighConfig);
  GPIO_PinInit(DEMO_LCD_CS_GPIO, DEMO_LCD_CS_PIN, &idleHighConfig);
  GPIO_PinInit(DEMO_LCD_DC_GPIO, DEMO_LCD_DC_PIN, &outConfig);

#if defined(DEMO_LCD_HAS_BLK_PIN) && DEMO_LCD_HAS_BLK_PIN
  GPIO_PinInit(DEMO_LCD_BLK_GPIO, DEMO_LCD_BLK_PIN, &outConfig);
#endif
  LCD_SetBacklight(true);
}

/* Generic MIPI-DCS init sequence, carried over unchanged from the earlier
 * bit-bang drivers - same command set works over hardware SPI. */
static void LCD_InitPanel(void) {
  LCD_SetResetPin(false);
  SDK_DelayAtLeastUs(20000, SystemCoreClock);
  LCD_SetResetPin(true);
  SDK_DelayAtLeastUs(150000, SystemCoreClock);

  LCD_WriteCommand(0x01U); /* Software reset */
  SDK_DelayAtLeastUs(20000, SystemCoreClock);

  LCD_WriteCommand(0x11U); /* Sleep out */
  SDK_DelayAtLeastUs(150000, SystemCoreClock);

#if defined(DEMO_LCD_CAMERA_PREVIEW) && DEMO_LCD_CAMERA_PREVIEW
  /* MADCTL: MV=1 (landscape 320x240) matches camera buffer 1:1 for lens focus preview */
  LCD_WriteCommandData(0x36U, (const uint8_t[]){0x20U}, 1U);
#else
  /* MADCTL: MV=0 (portrait 240x320 native GRAM), MX=1 (un-mirrored column order), MY=1 (rotated 180 deg).
   * BGR=0 (RGB color order). Matches physical upright orientation on FRDM-MCXN947. */
  LCD_WriteCommandData(0x36U, (const uint8_t[]){0xC0U}, 1U);
#endif

  LCD_WriteCommandData(0x3AU, (const uint8_t[]){0x55U},
                       1U); /* Pixel format: 16bpp RGB565 */

  LCD_WriteCommand(0x29U); /* Display ON */
  SDK_DelayAtLeastUs(50000, SystemCoreClock);
}

void LCD_Init(void) {
  PRINTF("LCD: hardware SPI (LPSPI1, shared bus) on the Arduino header\r\n");
  SPI1_BUS_Init();

  /* Prints the actually-achieved baud rate, not just the requested one -
   * confirms the clock-source setup in hardware_init.c is really taking
   * effect. */
  uint32_t srcClockHz = SPI1_BUS_GetSourceClockFreq();
  uint32_t achievedHz = SPI1_BUS_SetBaudRate(LCD_SPI_BAUDRATE_HZ);
  PRINTF("LCD: SPI1 source clock = %u Hz, requested %u Hz, achieved %u Hz\r\n",
         srcClockHz, LCD_SPI_BAUDRATE_HZ, achievedHz);

  LCD_InitGpioPins();
#ifdef DUALCORE_RTOS
  /* Dual-core build only: protects the panel init sequence (a multi-
   * command, CS-held-low transaction) from scheduler preemption and a
   * boot-time race against SNAPSHOT_Init() - see spi1_bus.h. */
  SPI1_BUS_LockNoPreempt();
#endif
  LCD_InitPanel();
#ifdef DUALCORE_RTOS
  SPI1_BUS_UnlockNoPreempt();
#endif
}

void LCD_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  const uint8_t colBuf[4] = {(uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFFU),
                             (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFFU)};
  const uint8_t rowBuf[4] = {(uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFFU),
                             (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFFU)};

  LCD_BeginTransaction();
  LCD_WriteCommandOpen(0x2AU); /* Column address set */
  LCD_WriteDataArrayOpen(colBuf, sizeof(colBuf));
  LCD_WriteCommandOpen(0x2BU); /* Page (row) address set */
  LCD_WriteDataArrayOpen(rowBuf, sizeof(rowBuf));
  LCD_WriteCommandOpen(0x2CU); /* Memory write - following bytes are pixels */
  /* CS stays asserted - LCD_PushPixels() closes it. */
}

/* eDMA pixel push. The panel wants MSB-first bytes per pixel, but the
 * source buffer is native (little-endian) uint16_t, so bytes are swapped
 * into this scratch buffer before each chunk. SPI1_BUS_PrepareDMA() runs
 * once per call, not once per chunk - the per-call setup cost otherwise
 * dominated fps (see WORKLOG.md). */
#ifdef DUALCORE_RTOS
#define LCD_SPI_CHUNK_PIXELS 1024U
static uint8_t s_pixelSwapBuf[LCD_SPI_CHUNK_PIXELS * 2U] __attribute__((aligned(4)));
#else
#define LCD_SPI_CHUNK_PIXELS 8192U
static uint8_t s_pixelSwapBuf[LCD_SPI_CHUNK_PIXELS * 2U]
    __attribute__((section(".sramx_bss"), aligned(4)));
#endif

void LCD_PushPixelsOpen(const uint16_t *pixels, uint32_t count) {
  (void)SPI1_BUS_PrepareDMA(kLPSPI_MasterPcs1 | kLPSPI_MasterPcsContinuous);

  while (count > 0U) {
    uint32_t chunk = (count > LCD_SPI_CHUNK_PIXELS) ? LCD_SPI_CHUNK_PIXELS : count;
    uint32_t i = 0U;

    if (((uintptr_t)pixels & 3U) == 0U) {
      const uint32_t *src32 = (const uint32_t *)(const void *)pixels;
      uint32_t *dst32 = (uint32_t *)(void *)s_pixelSwapBuf;
      uint32_t pairs = chunk >> 1U;
      for (uint32_t p = 0U; p < pairs; p++) {
        dst32[p] = __REV16(src32[p]);
      }
      i = pairs << 1U;
    }

    uint16_t *dst16 = (uint16_t *)(void *)s_pixelSwapBuf;
    for (; i < chunk; i++) {
      dst16[i] = (uint16_t)__REV16(pixels[i]);
    }
    (void)SPI1_BUS_TransferBytesDMA(s_pixelSwapBuf, chunk * 2U);

    pixels += chunk;
    count -= chunk;
  }
}

void LCD_EndWindow(void) { LCD_SetCSPin(true); }

void LCD_PushPixels(const uint16_t *pixels, uint32_t count) {
  LCD_PushPixelsOpen(pixels, count);
  LCD_EndWindow();
}

void LCD_SetOrientation(bool landscape) {
  /* 0x20: MV=1, 320x240 landscape (matches camera frame)
   * 0xC0: MV=0, MX=1, MY=1, 240x320 portrait (matches lock UI) */
  uint8_t madctl = landscape ? 0x20U : 0xC0U;
  LCD_WriteCommandData(0x36U, &madctl, 1U);
}

void LCD_Clear(uint16_t color, uint16_t width, uint16_t height) {
  uint16_t lineBuf[320];
  uint16_t maxW = (width > 320U) ? 320U : width;
  for (uint16_t i = 0U; i < maxW; i++) {
    lineBuf[i] = color;
  }
  LCD_SetWindow(0U, 0U, width - 1U, height - 1U);
  for (uint16_t row = 0U; row < height; row++) {
    LCD_PushPixelsOpen(lineBuf, width);
  }
  LCD_EndWindow();
}

/* Per-frame push time measured stable at ~56.9ms (near the ~51ms
 * bit-clock floor at 24MHz) - see WORKLOG.md. main.c's own fps counters
 * are the diagnostic to trust if this needs re-measuring. */
void LCD_DrawImage(uint16_t x0, uint16_t y0, uint16_t width, uint16_t height,
                   const uint16_t *pixels) {
#ifdef DUALCORE_RTOS
  /* Dual-core build only: a plain mutex stops other tasks' bus traffic
   * but not the scheduler from preempting THIS task mid-transaction,
   * which produced a torn/wrong-color image on real hardware - the
   * no-preempt lock fixed it (see spi1_bus.h/WORKLOG.md). */
  SPI1_BUS_LockNoPreempt();
#endif
  LCD_SetWindow(x0, y0, (uint16_t)(x0 + width - 1U),
                (uint16_t)(y0 + height - 1U));
  LCD_PushPixels(pixels, (uint32_t)width * height);
#ifdef DUALCORE_RTOS
  SPI1_BUS_UnlockNoPreempt();
#endif
}
