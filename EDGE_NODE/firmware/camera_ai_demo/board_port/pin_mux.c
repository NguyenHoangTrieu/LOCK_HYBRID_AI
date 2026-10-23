/*
 * pin_mux.c - Camera_AI_Test1 (FRDM-MCXN947)
 *
 * See pin_mux.h and ../../../README.md. Five groups of pins are configured
 * here: debug UART, OV7670 camera (J9, copied from NXP's
 * smartdma_camera_flexio_mculcd example), J8 FlexIO/LCD header (abandoned,
 * kept for reference), Arduino header LCD (current default), and the TFT
 * shield's onboard microSD slot (Arduino D10..D13, hardware LPSPI1).
 */

#include "fsl_common.h"
#include "fsl_port.h"
#include "fsl_gpio.h"
#include "pin_mux.h"
#include "app.h"

/* ---------------------------------------------------------------------- */
/* 1. Debug UART pins (P1_8 = FC4_P0 RX, P1_9 = FC4_P1 TX)                 */
/* ---------------------------------------------------------------------- */

void BOARD_InitBootPins(void)
{
    BOARD_InitDebugUartPins();
    BOARD_InitButtonPins();
}

void BOARD_InitButtonPins(void)
{
    CLOCK_EnableClock(kCLOCK_Port0);
    const port_pin_config_t sw3Config = {
        .pullSelect          = kPORT_PullUp,
        .mux                 = kPORT_MuxAlt0,
        .inputBuffer         = kPORT_InputBufferEnable,
        .lockRegister        = kPORT_UnlockRegister,
    };
    PORT_SetPinConfig(BOARD_SW3_PORT, BOARD_SW3_PIN, &sw3Config);
    const gpio_pin_config_t inputConfig = {.pinDirection = kGPIO_DigitalInput, .outputLogic = 0U};
    GPIO_PinInit(BOARD_SW3_GPIO, BOARD_SW3_PIN, &inputConfig);
}

void BOARD_InitDebugUartPins(void)
{
    CLOCK_EnableClock(kCLOCK_Port1);

    const port_pin_config_t debugUartRx = {
        .pullSelect          = kPORT_PullUp,
        .pullValueSelect     = kPORT_LowPullResistor,
        .slewRate            = kPORT_FastSlewRate,
        .passiveFilterEnable = kPORT_PassiveFilterDisable,
        .openDrainEnable     = kPORT_OpenDrainDisable,
        .driveStrength       = kPORT_LowDriveStrength,
        .mux                 = kPORT_MuxAlt2, /* FC4_P0 */
        .inputBuffer         = kPORT_InputBufferEnable,
        .invertInput         = kPORT_InputNormal,
        .lockRegister        = kPORT_UnlockRegister};
    PORT_SetPinConfig(PORT1, 8U, &debugUartRx);

    const port_pin_config_t debugUartTx = {
        .pullSelect          = kPORT_PullDisable,
        .pullValueSelect     = kPORT_LowPullResistor,
        .slewRate            = kPORT_FastSlewRate,
        .passiveFilterEnable = kPORT_PassiveFilterDisable,
        .openDrainEnable     = kPORT_OpenDrainDisable,
        .driveStrength       = kPORT_LowDriveStrength,
        .mux                 = kPORT_MuxAlt2, /* FC4_P1 */
        .inputBuffer         = kPORT_InputBufferEnable,
        .invertInput         = kPORT_InputNormal,
        .lockRegister        = kPORT_UnlockRegister};
    PORT_SetPinConfig(PORT1, 9U, &debugUartTx);
}

/* ---------------------------------------------------------------------- */
/* 2. OV7670 camera pins - J9 SmartDMA/Camera header                       */
/* ---------------------------------------------------------------------- */

void BOARD_InitCameraPins(void)
{
    CLOCK_EnableClock(kCLOCK_Port0);
    CLOCK_EnableClock(kCLOCK_Port1);
    CLOCK_EnableClock(kCLOCK_Port2);
    CLOCK_EnableClock(kCLOCK_Port3);

    /* PORT0_11 = PIO0_11, plain GPIO input -> camera HSYNC/HREF. */
    PORT_SetPinMux(PORT0, 11U, kPORT_MuxAlt0);
    PORT0->PCR[11] = ((PORT0->PCR[11] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));

    /* PORT0_4 = PIO0_4, plain GPIO input -> camera VSYNC. */
    PORT_SetPinMux(PORT0, 4U, kPORT_MuxAlt0);
    PORT0->PCR[4] = ((PORT0->PCR[4] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));

    /* PORT0_5 = PIO0_5, plain GPIO input -> camera PCLK. */
    PORT_SetPinMux(PORT0, 5U, kPORT_MuxAlt0);
    PORT0->PCR[5] = ((PORT0->PCR[5] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));

    /* Camera 8-bit data bus D0..D7, routed to EZH/SmartDMA GPIO (ALT7). */
    PORT_SetPinMux(PORT1, 4U, kPORT_MuxAlt7);  /* P1_4  = D0 */
    PORT1->PCR[4] = ((PORT1->PCR[4] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
    PORT_SetPinMux(PORT1, 5U, kPORT_MuxAlt7);  /* P1_5  = D1 */
    PORT1->PCR[5] = ((PORT1->PCR[5] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
    PORT_SetPinMux(PORT1, 6U, kPORT_MuxAlt7);  /* P1_6  = D2 */
    PORT1->PCR[6] = ((PORT1->PCR[6] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
    PORT_SetPinMux(PORT1, 7U, kPORT_MuxAlt7);  /* P1_7  = D3 */
    PORT1->PCR[7] = ((PORT1->PCR[7] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
    PORT_SetPinMux(PORT3, 4U, kPORT_MuxAlt7);  /* P3_4  = D4 */
    PORT3->PCR[4] = ((PORT3->PCR[4] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
    PORT_SetPinMux(PORT3, 5U, kPORT_MuxAlt7);  /* P3_5  = D5 */
    PORT3->PCR[5] = ((PORT3->PCR[5] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
    PORT_SetPinMux(PORT1, 10U, kPORT_MuxAlt7); /* P1_10 = D6 */
    PORT1->PCR[10] = ((PORT1->PCR[10] & ~(PORT_PCR_SRE_MASK | PORT_PCR_IBE_MASK)) |
                       PORT_PCR_SRE(0U) | PORT_PCR_IBE(1U));
    PORT_SetPinMux(PORT1, 11U, kPORT_MuxAlt7); /* P1_11 = D7 */
    PORT1->PCR[11] = ((PORT1->PCR[11] & ~(PORT_PCR_SRE_MASK | PORT_PCR_IBE_MASK)) |
                       PORT_PCR_SRE(0U) | PORT_PCR_IBE(1U));

    /* PORT2_2 = CLKOUT -> camera XCLK. */
    PORT_SetPinMux(PORT2, 2U, kPORT_MuxAlt1);
    PORT2->PCR[2] = ((PORT2->PCR[2] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));

    /* PORT3_2/PORT3_3 = FC7_P0/P1 -> camera SCCB (I2C-like) SDA/SCL. */
    PORT_SetPinMux(PORT3, 2U, kPORT_MuxAlt2);
    PORT3->PCR[2] = ((PORT3->PCR[2] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
    PORT_SetPinMux(PORT3, 3U, kPORT_MuxAlt2);
    PORT3->PCR[3] = ((PORT3->PCR[3] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
}

/* ---------------------------------------------------------------------- */
/* 3. J8 FlexIO/LCD header - 8-bit parallel MIPI-DBI/8080 bus (abandoned)  */
/* ---------------------------------------------------------------------- */

void BOARD_InitFlexioPins(void)
{
    CLOCK_EnableClock(kCLOCK_Port0);
    CLOCK_EnableClock(kCLOCK_Port2);
    CLOCK_EnableClock(kCLOCK_Port4);

    /* LCD_CS (P0_12) and LCD_RS/DC (P0_7) - plain GPIO. */
    PORT_SetPinMux(PORT0, 12U, kPORT_MuxAlt0);
    PORT0->PCR[12] = ((PORT0->PCR[12] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
    PORT_SetPinMux(PORT0, 7U, kPORT_MuxAlt0);
    PORT0->PCR[7] = ((PORT0->PCR[7] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));

#if DEMO_LCD_BITBANG
    /* Diagnostic bit-bang mode (LCD_BITBANG_DIAGNOSTIC=ON): mux the same
     * 10 signals as plain GPIO (Alt0) instead of FlexIO (Alt6), so
     * lcd_bitbang.c can toggle them directly. */
    CLOCK_EnableClock(kCLOCK_Port2);

    PORT_SetPinMux(PORT0, 8U, kPORT_MuxAlt0); /* P0_8  = LCD_RD (GPIO) */
    PORT0->PCR[8] = ((PORT0->PCR[8] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
    PORT_SetPinMux(PORT0, 9U, kPORT_MuxAlt0); /* P0_9  = LCD_WR (GPIO) */
    PORT0->PCR[9] = ((PORT0->PCR[9] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));

    PORT_SetPinMux(PORT2, 8U, kPORT_MuxAlt0); /* P2_8  = LCD_D0 (GPIO) */
    PORT2->PCR[8] = ((PORT2->PCR[8] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
    PORT_SetPinMux(PORT2, 9U, kPORT_MuxAlt0); /* P2_9  = LCD_D1 (GPIO) */
    PORT2->PCR[9] = ((PORT2->PCR[9] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
    PORT_SetPinMux(PORT2, 10U, kPORT_MuxAlt0); /* P2_10 = LCD_D2 (GPIO) */
    PORT2->PCR[10] = ((PORT2->PCR[10] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
    PORT_SetPinMux(PORT2, 11U, kPORT_MuxAlt0); /* P2_11 = LCD_D3 (GPIO) */
    PORT2->PCR[11] = ((PORT2->PCR[11] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
    PORT_SetPinMux(PORT4, 12U, kPORT_MuxAlt0); /* P4_12 = LCD_D4 (GPIO) */
    PORT4->PCR[12] = ((PORT4->PCR[12] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
    PORT_SetPinMux(PORT4, 13U, kPORT_MuxAlt0); /* P4_13 = LCD_D5 (GPIO) */
    PORT4->PCR[13] = ((PORT4->PCR[13] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
    PORT_SetPinMux(PORT4, 14U, kPORT_MuxAlt0); /* P4_14 = LCD_D6 (GPIO) */
    PORT4->PCR[14] = ((PORT4->PCR[14] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
    PORT_SetPinMux(PORT4, 15U, kPORT_MuxAlt0); /* P4_15 = LCD_D7 (GPIO) */
    PORT4->PCR[15] = ((PORT4->PCR[15] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
#else
    /* LCD_RD (P0_8) is plain GPIO here, NOT FlexIO0_D0 - the SDK's
     * FLEXIO_MCULCD driver only drives RD during a read transfer, leaving
     * it floating between writes, which was a suspected cause of the
     * "solid white, no image" bring-up bug. Held continuously high here
     * instead. */
    PORT_SetPinMux(PORT0, 8U, kPORT_MuxAlt0);
    PORT0->PCR[8] = ((PORT0->PCR[8] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));

    /* LCD_WR = FLEXIO0_D1 (P0_9). */
    PORT_SetPinMux(PORT0, 9U, kPORT_MuxAlt6);
    PORT0->PCR[9] = ((PORT0->PCR[9] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));

    /* 8-bit data bus LCD_D0..D7 = FLEXIO0_D16..D23. FLEXIO0_D24..D31
     * (LCD_D8..D15) are unused in 8-bit mode and not wired to this panel. */
    PORT_SetPinMux(PORT2, 8U, kPORT_MuxAlt6); /* P2_8  = FLEXIO0_D16 = LCD_D0 */
    PORT2->PCR[8] = ((PORT2->PCR[8] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
    PORT_SetPinMux(PORT2, 9U, kPORT_MuxAlt6); /* P2_9  = FLEXIO0_D17 = LCD_D1 */
    PORT2->PCR[9] = ((PORT2->PCR[9] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
    PORT_SetPinMux(PORT2, 10U, kPORT_MuxAlt6); /* P2_10 = FLEXIO0_D18 = LCD_D2 */
    PORT2->PCR[10] = ((PORT2->PCR[10] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
    PORT_SetPinMux(PORT2, 11U, kPORT_MuxAlt6); /* P2_11 = FLEXIO0_D19 = LCD_D3 */
    PORT2->PCR[11] = ((PORT2->PCR[11] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
    PORT_SetPinMux(PORT4, 12U, kPORT_MuxAlt6); /* P4_12 = FLEXIO0_D20 = LCD_D4 */
    PORT4->PCR[12] = ((PORT4->PCR[12] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
    PORT_SetPinMux(PORT4, 13U, kPORT_MuxAlt6); /* P4_13 = FLEXIO0_D21 = LCD_D5 */
    PORT4->PCR[13] = ((PORT4->PCR[13] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
    PORT_SetPinMux(PORT4, 14U, kPORT_MuxAlt6); /* P4_14 = FLEXIO0_D22 = LCD_D6 */
    PORT4->PCR[14] = ((PORT4->PCR[14] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
    PORT_SetPinMux(PORT4, 15U, kPORT_MuxAlt6); /* P4_15 = FLEXIO0_D23 = LCD_D7 */
    PORT4->PCR[15] = ((PORT4->PCR[15] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
#endif /* DEMO_LCD_BITBANG */

    /* LCD_RST (P4_7) - plain GPIO. */
    PORT_SetPinMux(PORT4, 7U, kPORT_MuxAlt0);
    PORT4->PCR[7] = ((PORT4->PCR[7] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));

    /* LCD_BLK / backlight enable (P4_6) - plain GPIO, driven high in LCD_Init(). */
    PORT_SetPinMux(PORT4, 6U, kPORT_MuxAlt0);
    PORT4->PCR[6] = ((PORT4->PCR[6] & ~PORT_PCR_IBE_MASK) | PORT_PCR_IBE(1U));
}

/* ---------------------------------------------------------------------- */
/* 4. Arduino header - LCD control pins (CS/DC/RST/BLK), current default   */
/* ---------------------------------------------------------------------- */

/* Only the LCD's own control pins (CS/DC/RST/BLK, Arduino A2..A5) are
 * here as plain GPIO - SCK/SDI/SDO ride shared hardware LPSPI1 instead,
 * see BOARD_InitSdCardPins() below and spi1_bus.h. Guarded by
 * DEMO_LCD_ARDUINO_HEADER since the J8 build's app.h has no equivalent
 * DEMO_LCD_DC_* macros. */
#if DEMO_LCD_ARDUINO_HEADER
void BOARD_InitArduinoLcdPins(void)
{
    const gpio_pin_config_t outputConfig   = {.pinDirection = kGPIO_DigitalOutput, .outputLogic = 0U};
    const gpio_pin_config_t idleHighConfig = {.pinDirection = kGPIO_DigitalOutput, .outputLogic = 1U};

    CLOCK_EnableClock(kCLOCK_Port0);
    CLOCK_EnableClock(kCLOCK_Port1);

    /* DC on PORT1 (Arduino D5, P1_21) */
    PORT_SetPinMux(DEMO_LCD_DC_PORT, DEMO_LCD_DC_PIN, kPORT_MuxAlt0);
    GPIO_PinInit(DEMO_LCD_DC_GPIO, DEMO_LCD_DC_PIN, &outputConfig);

    /* CS on PORT0 (Arduino D10, P0_27) */
    PORT_SetPinMux(DEMO_LCD_CS_PORT, DEMO_LCD_CS_PIN, kPORT_MuxAlt0);
    GPIO_PinInit(DEMO_LCD_CS_GPIO, DEMO_LCD_CS_PIN, &idleHighConfig);

    /* RST on PORT0 (Arduino D7, P0_31) */
    PORT_SetPinMux(DEMO_LCD_RST_PORT, DEMO_LCD_RST_PIN, kPORT_MuxAlt0);
    GPIO_PinInit(DEMO_LCD_RST_GPIO, DEMO_LCD_RST_PIN, &idleHighConfig);

#if defined(DEMO_LCD_HAS_BLK_PIN) && DEMO_LCD_HAS_BLK_PIN
    PORT_SetPinMux(DEMO_LCD_BLK_PORT, DEMO_LCD_BLK_PIN, kPORT_MuxAlt0);
    GPIO_PinInit(DEMO_LCD_BLK_GPIO, DEMO_LCD_BLK_PIN, &outputConfig);
#endif
}

/* ---------------------------------------------------------------------- */
/* 4b. Arduino header - touch controller (XPT2046) control pins            */
/* ---------------------------------------------------------------------- */

/* T_CLK/T_DIN/T_DO ride the shared LPSPI1 bus - only T_CS/T_IRQ need
 * dedicated pins. T_IRQ gets an internal pull-up (open-drain, active-low). */
void BOARD_InitTouchPins(void)
{
    const gpio_pin_config_t idleHighConfig = {.pinDirection = kGPIO_DigitalOutput, .outputLogic = 1U};

    CLOCK_EnableClock(kCLOCK_Port0);
    CLOCK_EnableClock(kCLOCK_Port1);

    /* Touch CS on PORT0 (Arduino D4, P0_30) */
    PORT_SetPinMux(DEMO_TOUCH_CS_PORT, DEMO_TOUCH_CS_PIN, kPORT_MuxAlt0);
    GPIO_PinInit(DEMO_TOUCH_CS_GPIO, DEMO_TOUCH_CS_PIN, &idleHighConfig);

    /* Touch IRQ on PORT1 (Arduino D3, P1_23) - input with pull-up */
    const port_pin_config_t irqPullUpConfig = {
        .pullSelect   = kPORT_PullUp,
        .mux          = kPORT_MuxAlt0,
        .inputBuffer  = kPORT_InputBufferEnable,
        .lockRegister = kPORT_UnlockRegister,
    };
    PORT_SetPinConfig(DEMO_TOUCH_IRQ_PORT, DEMO_TOUCH_IRQ_PIN, &irqPullUpConfig);
    const gpio_pin_config_t inputConfig = {.pinDirection = kGPIO_DigitalInput, .outputLogic = 0U};
    GPIO_PinInit(DEMO_TOUCH_IRQ_GPIO, DEMO_TOUCH_IRQ_PIN, &inputConfig);
}
#endif /* DEMO_LCD_ARDUINO_HEADER */

/* ---------------------------------------------------------------------- */
/* 5. Shared SPI bus (SCK/MOSI/MISO) + microSD slot CS - Arduino header    */
/*    hardware LPSPI1                                                      */
/* ---------------------------------------------------------------------- */

void BOARD_InitSdCardPins(void)
{
    CLOCK_EnableClock(kCLOCK_Port0);

    const port_pin_config_t spiOutConfig = {
        .slewRate     = kPORT_SlowSlewRate,
        .driveStrength= kPORT_LowDriveStrength,
        .mux          = kPORT_MuxAlt2,
        .lockRegister = kPORT_UnlockRegister,
    };
    PORT_SetPinConfig(PORT0, 24U, &spiOutConfig); /* P0_24 = FC1_P0 = LPSPI1 SDO (D11/SPI_MOSI) */
    PORT_SetPinConfig(PORT0, 25U, &spiOutConfig); /* P0_25 = FC1_P1 = LPSPI1 SCK (D13/SPI_SCK) */

    /* P0_26 = FC1_P2 = LPSPI1 SDI (D12/SPI_MISO) - required pull-up */
    const port_pin_config_t sdiPullUpConfig = {
        .pullSelect   = kPORT_PullUp,
        .mux          = kPORT_MuxAlt2,
        .inputBuffer  = kPORT_InputBufferEnable,
        .lockRegister = kPORT_UnlockRegister,
    };
    PORT_SetPinConfig(PORT0, 26U, &sdiPullUpConfig);

    /* SD Card CS on PORT0 (Arduino A3, P0_22) - GPIO output, idle high */
    const gpio_pin_config_t idleHighConfig = {.pinDirection = kGPIO_DigitalOutput, .outputLogic = 1U};
    PORT_SetPinMux(DEMO_SD_CS_PORT, DEMO_SD_CS_PIN, kPORT_MuxAlt0);
    GPIO_PinInit(DEMO_SD_CS_GPIO, DEMO_SD_CS_PIN, &idleHighConfig);
}

/* ---------------------------------------------------------------------- */
/* 6. Baseboard Peripherals: RFID, ESP32, INA228, Servo                    */
/* ---------------------------------------------------------------------- */
void BOARD_InitBaseboardPeripheralPins(void)
{
    const gpio_pin_config_t idleHighConfig = {.pinDirection = kGPIO_DigitalOutput, .outputLogic = 1U};
    const gpio_pin_config_t inputConfig    = {.pinDirection = kGPIO_DigitalInput, .outputLogic = 0U};
    const port_pin_config_t pullUpInputConfig = {
        .pullSelect   = kPORT_PullUp,
        .mux          = kPORT_MuxAlt0,
        .inputBuffer  = kPORT_InputBufferEnable,
        .lockRegister = kPORT_UnlockRegister,
    };

    CLOCK_EnableClock(kCLOCK_Port0);
    CLOCK_EnableClock(kCLOCK_Port1);
    CLOCK_EnableClock(kCLOCK_Port4);

    /* RFID-RC522: CS (P0_28 / D8) idle high, RST (P0_14 / A2) idle high, IRQ (P0_15 / A4) input pullup */
    PORT_SetPinMux(DEMO_RFID_CS_PORT, DEMO_RFID_CS_PIN, kPORT_MuxAlt0);
    GPIO_PinInit(DEMO_RFID_CS_GPIO, DEMO_RFID_CS_PIN, &idleHighConfig);

    PORT_SetPinMux(DEMO_RFID_RST_PORT, DEMO_RFID_RST_PIN, kPORT_MuxAlt0);
    GPIO_PinInit(DEMO_RFID_RST_GPIO, DEMO_RFID_RST_PIN, &idleHighConfig);

    PORT_SetPinConfig(DEMO_RFID_IRQ_PORT, DEMO_RFID_IRQ_PIN, &pullUpInputConfig);
    GPIO_PinInit(DEMO_RFID_IRQ_GPIO, DEMO_RFID_IRQ_PIN, &inputConfig);

    /* ESP32: CS (P0_23 / A5) idle high so it doesn't assert SPI while inactive.
     * Use High Drive Strength and Fast Slew Rate to drive past any debounce capacitance on P0_23 (SW2). */
    const port_pin_config_t esp32CsPortConfig = {
        .pullSelect   = kPORT_PullUp,
        .slewRate     = kPORT_FastSlewRate,
        .driveStrength= kPORT_HighDriveStrength,
        .mux          = kPORT_MuxAlt0,
        .inputBuffer  = kPORT_InputBufferDisable,
        .lockRegister = kPORT_UnlockRegister,
    };
    PORT_SetPinConfig(DEMO_ESP32_CS_PORT, DEMO_ESP32_CS_PIN, &esp32CsPortConfig);
    GPIO_PinInit(DEMO_ESP32_CS_GPIO, DEMO_ESP32_CS_PIN, &idleHighConfig);

    /* INA228: ALRT (P0_29 / D2) input pullup */
    PORT_SetPinConfig(DEMO_INA_ALRT_PORT, DEMO_INA_ALRT_PIN, &pullUpInputConfig);
    GPIO_PinInit(DEMO_INA_ALRT_GPIO, DEMO_INA_ALRT_PIN, &inputConfig);

    /* On-board RGB LED D2 (Active-Low: Red=P0_10, Green=P0_27, Blue=P1_2).
     * Drive all cathodes high (1) so the annoying on-board LED is completely turned OFF. */
    PORT_SetPinMux(PORT1, 2U, kPORT_MuxAlt0);
    GPIO_PinInit(GPIO1, 2U, &idleHighConfig);

    /* Servo Motor: PWM (P0_10 / D9) / Red LED cathode: idle high to keep Red LED off */
    PORT_SetPinMux(DEMO_SERVO_PWM_PORT, DEMO_SERVO_PWM_PIN, kPORT_MuxAlt0);
    GPIO_PinInit(DEMO_SERVO_PWM_GPIO, DEMO_SERVO_PWM_PIN, &idleHighConfig);
}

