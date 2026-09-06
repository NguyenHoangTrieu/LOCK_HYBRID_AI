/*
 * touch_xpt2046.c - see touch_xpt2046.h
 *
 * Standard XPT2046 command bytes (widely used in Arduino/embedded XPT2046
 * libraries - e.g. Adafruit's, TFT_eSPI's): 0x90 starts a 12-bit
 * differential X-position conversion, 0xD0 a Y-position one. Each read is
 * one 3-byte SPI transaction (command byte, then 2 dummy clock bytes to
 * shift the 16-bit result out) - the first received byte is a null bit
 * plus high conversion bits, so the 12-bit result is
 * `((rx[1] << 8) | rx[2]) >> 3`.
 *
 * X/Y channel-to-axis mapping (which command reads "X" vs. "Y" on screen)
 * and rotation are wiring/panel-dependent and UNVERIFIED here - swap
 * TOUCH_ReadChannel() calls in TOUCH_ReadRaw() below if X/Y come out
 * transposed on real hardware.
 */

#include "touch_xpt2046.h"
#include "app.h"
#include "fsl_gpio.h"
#include "fsl_lpspi.h"
#include "spi1_bus.h"

/* XPT2046 datasheet caps reliable conversion around 2MHz - conservative
 * default, same "start slow" rationale as the LCD's own baud rate. */
#ifndef TOUCH_SPI_BAUDRATE_HZ
#define TOUCH_SPI_BAUDRATE_HZ 1000000U
#endif

#define TOUCH_CMD_READ_X 0x90U /* differential, 12-bit, X channel - see file header comment */
#define TOUCH_CMD_READ_Y 0xD0U /* differential, 12-bit, Y channel */

static void TOUCH_SetCSPin(bool set) {
  GPIO_PinWrite(DEMO_TOUCH_CS_GPIO, DEMO_TOUCH_CS_PIN, set ? 1U : 0U);
}

/* Reads one 12-bit ADC channel - see file header for the command-byte/
 * extraction details. kLPSPI_MasterPcs1 is a "don't care" value (never
 * muxed to a pin) - the real CS is the manual T_CS GPIO toggle here.
 * kLPSPI_MasterPcsContinuous still matters though (see lcd_spi_hw.c). */
static uint16_t TOUCH_ReadChannel(uint8_t command) {
  const uint8_t tx[3] = {command, 0x00U, 0x00U};
  uint8_t rx[3] = {0};

  (void)SPI1_BUS_SetBaudRate(TOUCH_SPI_BAUDRATE_HZ);
  TOUCH_SetCSPin(false);
  (void)SPI1_BUS_TransferBlocking(tx, rx, sizeof(tx), kLPSPI_MasterPcs1 | kLPSPI_MasterPcsContinuous);
  TOUCH_SetCSPin(true);

  return (uint16_t)(((uint16_t)rx[1] << 8U | rx[2]) >> 3U);
}

void TOUCH_Init(void) { SPI1_BUS_Init(); }

bool TOUCH_IsPressed(void) {
  return GPIO_PinRead(DEMO_TOUCH_IRQ_GPIO, DEMO_TOUCH_IRQ_PIN) == 0U;
}

void TOUCH_ReadRaw(uint16_t *x, uint16_t *y) {
  *x = TOUCH_ReadChannel(TOUCH_CMD_READ_X);
  *y = TOUCH_ReadChannel(TOUCH_CMD_READ_Y);
}
