/*
 * app.h - core1 (Camera_AI_Test1 dual-core RTOS migration - see
 * WORKLOG.md). Camera/LCD macros, copied from the legacy core0/app.h
 * (Arduino header only - J8/FlexIO not carried into the dual-core build).
 */
#ifndef _APP_CORE1_H_
#define _APP_CORE1_H_

#include "fsl_gpio.h"
#include "fsl_port.h"
#include "fsl_smartdma.h"

#define DEMO_LCD_ARDUINO_HEADER 1
#define DEMO_LCD_BITBANG        1

/*******************************************************************************
 * Camera / frame buffer - see board_port/cm33_core0/app.h's original
 * comment (WORKLOG.md) for why whole-frame QVGA RGB565 is the confirmed-
 * working mode (a 384x384 grayscale mode was tried and reverted).
 ******************************************************************************/
#define DEMO_CAMERA_RESOLUTION kVIDEO_ResolutionQVGA /* 320*240 */
#define DEMO_BUFFER_WIDTH      320U
#define DEMO_BUFFER_HEIGHT     240U
#define DEMO_SMARTDMA_API      kSMARTDMA_CameraWholeFrameQVGA /* whole 320x240 RGB565 frame per interrupt */

/*******************************************************************************
 * TFT LCD panel geometry - ILI9341-family, 240x320 native GRAM (portrait),
 * driven here in landscape via MADCTL (MV bit, see lcd_spi_hw.c's
 * LCD_InitPanel).
 ******************************************************************************/
#define DEMO_PANEL_WIDTH  320U
#define DEMO_PANEL_HEIGHT 240U

/*******************************************************************************
 * Arduino header - baseboard PCB schematic (LOCK_EDGE_BASEBOARD):
 *  - Shared LPSPI1 bus: SCK=D13 (P0_25), MOSI=D11 (P0_24), MISO=D12 (P0_26)
 *  - TFT LCD: CS=D10 (P0_27), DC=D5 (P1_21), RST=D7 (P0_31), LED=3.3V
 *  - Touch (XPT2046): CS=D4 (P0_30), IRQ=D3 (P1_23)
 *  - SD Card: CS=A3 (P0_22)
 *  - RFID-RC522: CS=D8 (P0_28), RST=A2 (P0_14), IRQ=A4 (P0_15)
 *  - ESP32: CS=A5 (P0_23)
 *  - INA228: SCL=SCL (P4_1), SDA=SDA (P4_0), ALRT=D2 (P0_29)
 *  - Servo: PWM=D9 (P0_10)
 ******************************************************************************/
#define DEMO_LCD_DC_GPIO  GPIO1
#define DEMO_LCD_DC_PIN   21U /* Arduino D5, P1_21 */
#define DEMO_LCD_DC_PORT  PORT1

#define DEMO_LCD_CS_GPIO  GPIO0
#define DEMO_LCD_CS_PIN   27U /* Arduino D10, P0_27 */
#define DEMO_LCD_CS_PORT  PORT0

#define DEMO_LCD_RST_GPIO GPIO0
#define DEMO_LCD_RST_PIN  31U /* Arduino D7, P0_31 */
#define DEMO_LCD_RST_PORT PORT0

/* TFT backlight LED is hardwired to VDD_3V3 on the baseboard PCB */
#define DEMO_LCD_HAS_BLK_PIN 0

/*******************************************************************************
 * Touch controller (XPT2046) - source/display/touch_xpt2046.c
 ******************************************************************************/
#define DEMO_TOUCH_CS_GPIO  GPIO0
#define DEMO_TOUCH_CS_PIN   30U /* Arduino D4, P0_30 */
#define DEMO_TOUCH_CS_PORT  PORT0

#define DEMO_TOUCH_IRQ_GPIO GPIO1
#define DEMO_TOUCH_IRQ_PIN  23U /* Arduino D3, P1_23 - input, active low */
#define DEMO_TOUCH_IRQ_PORT PORT1

/*******************************************************************************
 * MicroSD card (TFT SPI slot) - source/storage/sd_spi_disk.c
 ******************************************************************************/
#define DEMO_SD_CS_GPIO     GPIO0
#define DEMO_SD_CS_PIN      22U /* Arduino A3, P0_22 */
#define DEMO_SD_CS_PORT     PORT0

/*******************************************************************************
 * RFID-RC522 reader
 ******************************************************************************/
#define DEMO_RFID_CS_GPIO   GPIO0
#define DEMO_RFID_CS_PIN    28U /* Arduino D8, P0_28 */
#define DEMO_RFID_CS_PORT   PORT0

#define DEMO_RFID_RST_GPIO  GPIO0
#define DEMO_RFID_RST_PIN   14U /* Arduino A2, P0_14 */
#define DEMO_RFID_RST_PORT  PORT0

#define DEMO_RFID_IRQ_GPIO  GPIO0
#define DEMO_RFID_IRQ_PIN   15U /* Arduino A4, P0_15 */
#define DEMO_RFID_IRQ_PORT  PORT0

/*******************************************************************************
 * ESP32-C5 Slave Select
 ******************************************************************************/
#define DEMO_ESP32_CS_GPIO  GPIO0
#define DEMO_ESP32_CS_PIN   23U /* Arduino A5, P0_23 */
#define DEMO_ESP32_CS_PORT  PORT0

/*******************************************************************************
 * INA228 Power Monitor Alert & I2C
 ******************************************************************************/
#define DEMO_INA_ALRT_GPIO  GPIO0
#define DEMO_INA_ALRT_PIN   29U /* Arduino D2, P0_29 */
#define DEMO_INA_ALRT_PORT  PORT0

/*******************************************************************************
 * Servo Motor (PWM control)
 ******************************************************************************/
#define DEMO_SERVO_PWM_GPIO GPIO0
#define DEMO_SERVO_PWM_PIN  10U /* Arduino D9, P0_10 */
#define DEMO_SERVO_PWM_PORT PORT0


/*******************************************************************************
 * Prototypes
 ******************************************************************************/
void BOARD_InitHardware(void);

#endif /* _APP_CORE1_H_ */
