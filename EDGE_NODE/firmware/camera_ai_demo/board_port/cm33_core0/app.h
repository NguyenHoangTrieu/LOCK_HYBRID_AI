/*
 * app.h - Camera_AI_Test1 (FRDM-MCXN947)
 *
 * Board-level constants: camera/panel geometry and the GPIO pins used for
 * the LCD bus. pin_mux.c and the LCD driver both include this file.
 *
 * Current default: GPIO bit-bang on the Arduino header
 * (DEMO_LCD_ARDUINO_HEADER=1). The J8 FlexIO/LCD header path and a J8
 * bit-bang diagnostic variant are still selectable (see CMakeLists.txt)
 * but abandoned - see WORKLOG.md.
 */
#ifndef _APP_H_
#define _APP_H_

#include "fsl_gpio.h"
#include "fsl_port.h"
#include "fsl_smartdma.h"

/* Normally set by CMakeLists.txt's LCD_ARDUINO_HEADER_BITBANG /
 * LCD_BITBANG_DIAGNOSTIC options. Defaulted here too so this header is
 * self-contained. Arduino header has no FlexIO route, so it implies
 * bit-bang. */
#ifndef DEMO_LCD_ARDUINO_HEADER
#define DEMO_LCD_ARDUINO_HEADER 1
#endif
#ifndef DEMO_LCD_BITBANG
#define DEMO_LCD_BITBANG (DEMO_LCD_ARDUINO_HEADER || 0)
#endif

/*******************************************************************************
 * Camera / frame buffer. A 384x384 grayscale mode was tried and reverted
 * (HardFault in stripe reassembly - see WORKLOG.md); this whole-frame QVGA
 * RGB565 capture is the confirmed-working default.
 ******************************************************************************/
#define DEMO_CAMERA_RESOLUTION kVIDEO_ResolutionQVGA /* 320*240 */
#define DEMO_BUFFER_WIDTH      320U
#define DEMO_BUFFER_HEIGHT     240U
#define DEMO_SMARTDMA_API      kSMARTDMA_CameraWholeFrameQVGA /* whole 320x240 RGB565 frame per interrupt */

/*******************************************************************************
 * TFT LCD panel geometry. Panel is ILI9341-family, 240x320 native GRAM
 * (portrait), driven here in landscape via MADCTL (MV bit, see
 * lcd_bitbang.c's LCD_InitPanel). No longer tied to the camera buffer
 * size (the LCD only ever shows text status lines now, not the live
 * camera image - see main.c).
 ******************************************************************************/
#define DEMO_PANEL_WIDTH  240U
#define DEMO_PANEL_HEIGHT 320U

/*******************************************************************************
 * J8 FlexIO/LCD header - 8-bit parallel MIPI-DBI/8080 bus (panel only has
 * LCD_D0..D7), driven by the FlexIO peripheral. Bus width
 * (FLEXIO_MCULCD_DATA_BUS_WIDTH=8) is set in CMakeLists.txt.
 ******************************************************************************/
#define DEMO_FLEXIO              FLEXIO0
#define DEMO_FLEXIO_CLOCK_FREQ   CLOCK_GetFlexioClkFreq()
/* Total bus bit rate (driver divides by the 8-bit width internally, so
 * this is 8x the per-pin WR toggle rate). This panel's controller can't
 * keep up with NXP's reference 20 MHz/pin - dropped down here to a rate
 * that latches cleanly. See WORKLOG.md before changing. */
#define DEMO_FLEXIO_BAUDRATE_BPS 800000U /* = 100 kHz/pin @ 8-bit width, with the /4 FlexIO clock divider */

#define DEMO_FLEXIO_WR_PIN           1U  /* FLEXIO0_D1 = P0_9 */
#define DEMO_FLEXIO_RD_PIN           0U  /* FLEXIO0_D0 = P0_8 */
#define DEMO_FLEXIO_DATA_PIN_START   16U /* LCD_D0..D7 = FLEXIO0_D16..D23 (8-bit bus) */
#define DEMO_FLEXIO_TX_START_SHIFTER 0U
#define DEMO_FLEXIO_RX_START_SHIFTER 0U
#define DEMO_FLEXIO_TX_END_SHIFTER   7U
#define DEMO_FLEXIO_RX_END_SHIFTER   7U
#define DEMO_FLEXIO_TIMER            0U

#if DEMO_LCD_ARDUINO_HEADER
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


#else /* !DEMO_LCD_ARDUINO_HEADER: J8 pin set (FlexIO or J8 bit-bang) */

/* Plain-GPIO LCD control signals (not on the FlexIO data/WR/RD pins above). */
#define DEMO_LCD_RST_GPIO GPIO4
#define DEMO_LCD_RST_PIN  7U /* P4_7 */
#define DEMO_LCD_CS_GPIO  GPIO0
#define DEMO_LCD_CS_PIN   12U /* P0_12 */
#define DEMO_LCD_RS_GPIO  GPIO0
#define DEMO_LCD_RS_PIN   7U /* P0_7 */

/* Backlight enable (J8 pin labeled LCD_BLK). Driven high in LCD_Init(). */
#define DEMO_LCD_BLK_GPIO GPIO4
#define DEMO_LCD_BLK_PIN  6U /* P4_6 */

/* Same WR/RD pins whether the backend is FlexIO or J8 bit-bang, plus GPIO
 * port/pin pairs for the 8 data lines (FlexIO addresses these by
 * FLEXIO0_D16..D23 instead, when active). */
#define DEMO_LCD_WR_GPIO GPIO0
#define DEMO_LCD_WR_PIN  9U /* P0_9 */
#define DEMO_LCD_RD_GPIO GPIO0
#define DEMO_LCD_RD_PIN  8U /* P0_8 */

#define DEMO_LCD_D0_GPIO GPIO2
#define DEMO_LCD_D0_PIN  8U /* P2_8 */
#define DEMO_LCD_D1_GPIO GPIO2
#define DEMO_LCD_D1_PIN  9U /* P2_9 */
#define DEMO_LCD_D2_GPIO GPIO2
#define DEMO_LCD_D2_PIN  10U /* P2_10 */
#define DEMO_LCD_D3_GPIO GPIO2
#define DEMO_LCD_D3_PIN  11U /* P2_11 */
#define DEMO_LCD_D4_GPIO GPIO4
#define DEMO_LCD_D4_PIN  12U /* P4_12 */
#define DEMO_LCD_D5_GPIO GPIO4
#define DEMO_LCD_D5_PIN  13U /* P4_13 */
#define DEMO_LCD_D6_GPIO GPIO4
#define DEMO_LCD_D6_PIN  14U /* P4_14 */
#define DEMO_LCD_D7_GPIO GPIO4
#define DEMO_LCD_D7_PIN  15U /* P4_15 */

#endif /* DEMO_LCD_ARDUINO_HEADER */

/*******************************************************************************
 * Dual-core RTOS build only (-DDUALCORE_RTOS=ON). CORE1_BOOT_ADDRESS is
 * where core0 copies core1's incbin'd image before releasing it via
 * MCMGR_StartCore() - must match cm33_core1's linker script ORIGIN
 * exactly. See WORKLOG.md for the two-step build (core1 built first, core0
 * embeds its .bin).
 ******************************************************************************/
#define CORE1_BOOT_ADDRESS 0x04001000

#if defined(__GNUC__)
extern const char core1_image_start[];
extern const char *core1_image_end;
extern uint32_t core1_image_size;
#define CORE1_IMAGE_START ((void *)core1_image_start)
#endif

/*******************************************************************************
 * Prototypes
 ******************************************************************************/
void BOARD_InitHardware(void);

#ifdef CORE1_IMAGE_COPY_TO_RAM
uint32_t get_core1_image_size(void);
#endif

/* SmartDMA camera capture only works with DCDC at Mid; USB HS PHY needs
 * Overdrive - see hardware_init.c and WORKLOG.md. These just flip the
 * regulator level (no clock/camera re-init) - main() switches back and
 * forth on its periodic refresh cycle. */
void BOARD_SetRegulatorsMidVoltage(void);
void BOARD_SetRegulatorsOverdriveVoltage(void);

#endif /* _APP_H_ */
