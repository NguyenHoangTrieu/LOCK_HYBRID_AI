/*
 * lcd_flexio_mculcd.c - see lcd_flexio_mculcd.h
 *
 * Uses the SDK's FLEXIO_MCULCD driver but NOT its ST7796S panel driver on
 * top: the panel in hand isn't actually ST7796S silicon (its chip-specific
 * init produced backlight-on-but-no-image). This talks to the panel
 * directly with a generic MIPI-DCS init sequence instead, over FlexIO.
 * Abandoned path (signal-integrity issues) - see WORKLOG.md.
 */

#include "lcd_flexio_mculcd.h"
#include "app.h"
#include "board.h"
#include "fsl_flexio_mculcd.h"
#include "fsl_gpio.h"
#include "fsl_debug_console.h"

static void LCD_SetCSPin(bool set)
{
    GPIO_PinWrite(DEMO_LCD_CS_GPIO, DEMO_LCD_CS_PIN, set ? 1U : 0U);
}

static void LCD_SetRSPin(bool set)
{
    GPIO_PinWrite(DEMO_LCD_RS_GPIO, DEMO_LCD_RS_PIN, set ? 1U : 0U);
}

static void LCD_SetResetPin(bool set)
{
    GPIO_PinWrite(DEMO_LCD_RST_GPIO, DEMO_LCD_RST_PIN, set ? 1U : 0U);
}

static void LCD_SetBacklight(bool on)
{
    GPIO_PinWrite(DEMO_LCD_BLK_GPIO, DEMO_LCD_BLK_PIN, on ? 1U : 0U);
}

static FLEXIO_MCULCD_Type s_flexioLcdDev = {
    .flexioBase          = DEMO_FLEXIO,
    .busType             = kFLEXIO_MCULCD_8080,
    .dataPinStartIndex   = DEMO_FLEXIO_DATA_PIN_START,
    .ENWRPinIndex        = DEMO_FLEXIO_WR_PIN,
    .RDPinIndex          = DEMO_FLEXIO_RD_PIN, /* Unused - RD is a plain GPIO now, see below. */
    .txShifterStartIndex = DEMO_FLEXIO_TX_START_SHIFTER,
    .txShifterEndIndex   = DEMO_FLEXIO_TX_END_SHIFTER,
    .rxShifterStartIndex = DEMO_FLEXIO_RX_START_SHIFTER,
    .rxShifterEndIndex   = DEMO_FLEXIO_RX_END_SHIFTER,
    .timerIndex          = DEMO_FLEXIO_TIMER,
    .setCSPin            = LCD_SetCSPin,
    .setRSPin             = LCD_SetRSPin,
    .setRDWRPin          = NULL, /* Not used in 8080 mode. */
};

static void LCD_InitFlexioMcuLcd(void)
{
    flexio_mculcd_config_t flexioMcuLcdConfig;
    const gpio_pin_config_t pinConfig = {.pinDirection = kGPIO_DigitalOutput, .outputLogic = 1};

    GPIO_PinInit(DEMO_LCD_RST_GPIO, DEMO_LCD_RST_PIN, &pinConfig);
    GPIO_PinInit(DEMO_LCD_CS_GPIO, DEMO_LCD_CS_PIN, &pinConfig);
    GPIO_PinInit(DEMO_LCD_RS_GPIO, DEMO_LCD_RS_PIN, &pinConfig);

    /* LCD_RD held high (inactive) as a plain GPIO output, NOT routed
     * through FlexIO - the SDK's FLEXIO_MCULCD driver only drives RD
     * during an explicit read transfer, floating it between reads (i.e.
     * during every write). A floating RD line during writes was a
     * suspected contributor to the "solid white, no image" bring-up bug -
     * see WORKLOG.md. */
    GPIO_PinInit(DEMO_LCD_RD_GPIO, DEMO_LCD_RD_PIN, &pinConfig);

    GPIO_PinInit(DEMO_LCD_BLK_GPIO, DEMO_LCD_BLK_PIN, &pinConfig);
    LCD_SetBacklight(true);

    FLEXIO_MCULCD_GetDefaultConfig(&flexioMcuLcdConfig);
    flexioMcuLcdConfig.baudRate_Bps = DEMO_FLEXIO_BAUDRATE_BPS;

    if (FLEXIO_MCULCD_Init(&s_flexioLcdDev, &flexioMcuLcdConfig, DEMO_FLEXIO_CLOCK_FREQ) != kStatus_Success)
    {
        PRINTF("LCD: FlexIO MCULCD init failed.\r\n");
        while (1)
        {
        }
    }
}

/* Write one command byte with no data, in its own CS-bracketed transfer. */
static void LCD_WriteCommand(uint8_t command)
{
    FLEXIO_MCULCD_StartTransfer(&s_flexioLcdDev);
    FLEXIO_MCULCD_WriteCommandBlocking(&s_flexioLcdDev, command);
    FLEXIO_MCULCD_StopTransfer(&s_flexioLcdDev);
}

/* Write one command byte followed by 1-4 data bytes, in one CS-bracketed transfer. */
static void LCD_WriteCommandData(uint8_t command, const uint8_t *data, uint32_t len)
{
    FLEXIO_MCULCD_StartTransfer(&s_flexioLcdDev);
    FLEXIO_MCULCD_WriteCommandBlocking(&s_flexioLcdDev, command);
    FLEXIO_MCULCD_WriteDataArrayBlocking(&s_flexioLcdDev, (void *)data, len);
    FLEXIO_MCULCD_StopTransfer(&s_flexioLcdDev);
}

static void LCD_InitPanel(void)
{
    LCD_SetResetPin(false);
    SDK_DelayAtLeastUs(20000, SystemCoreClock);
    LCD_SetResetPin(true);
    SDK_DelayAtLeastUs(150000, SystemCoreClock);

    LCD_WriteCommand(0x01U); /* Software reset */
    SDK_DelayAtLeastUs(20000, SystemCoreClock);

    LCD_WriteCommand(0x11U); /* Sleep out */
    SDK_DelayAtLeastUs(150000, SystemCoreClock);

    /* MADCTL: memory access control. BGR cleared (0x60, not 0x68) since
     * the OV7670 camera buffer is RGB565 and LCD_PushPixels() sends it
     * unmodified - see README.md "LCD bring-up tips" if colors are
     * swapped/rotated once something is on screen. */
    LCD_WriteCommandData(0x36U, (const uint8_t[]){0x60U}, 1U);

    LCD_WriteCommandData(0x3AU, (const uint8_t[]){0x55U}, 1U); /* Pixel format: 16bpp RGB565 */

    LCD_WriteCommand(0x29U); /* Display ON */
    SDK_DelayAtLeastUs(50000, SystemCoreClock);
}

void LCD_Init(void)
{
    LCD_InitFlexioMcuLcd();
    LCD_InitPanel();
}

void LCD_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    const uint8_t colBuf[4] = {(uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFFU), (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFFU)};
    const uint8_t rowBuf[4] = {(uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFFU), (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFFU)};

    FLEXIO_MCULCD_StartTransfer(&s_flexioLcdDev);
    FLEXIO_MCULCD_WriteCommandBlocking(&s_flexioLcdDev, 0x2AU); /* Column address set */
    FLEXIO_MCULCD_WriteDataArrayBlocking(&s_flexioLcdDev, (void *)colBuf, sizeof(colBuf));
    FLEXIO_MCULCD_WriteCommandBlocking(&s_flexioLcdDev, 0x2BU); /* Page (row) address set */
    FLEXIO_MCULCD_WriteDataArrayBlocking(&s_flexioLcdDev, (void *)rowBuf, sizeof(rowBuf));
    FLEXIO_MCULCD_WriteCommandBlocking(&s_flexioLcdDev, 0x2CU); /* Memory write - following bytes are pixels */
    /* Transfer stays open (CS asserted) - LCD_PushPixels() closes it. */
}

void LCD_PushPixelsOpen(const uint16_t *pixels, uint32_t count)
{
    /* Sends raw bytes with no endianness handling, so each RGB565 pixel
     * needs pre-swapping to wire order (high byte first). Batched into
     * chunks so this doesn't need a full-frame temp buffer. */
    uint8_t chunk[128]; /* 64 pixels per chunk */

    while (count > 0U)
    {
        uint32_t n = (count > 64U) ? 64U : count;

        for (uint32_t i = 0; i < n; i++)
        {
            chunk[(i * 2U) + 0U] = (uint8_t)(pixels[i] >> 8);
            chunk[(i * 2U) + 1U] = (uint8_t)(pixels[i] & 0xFFU);
        }

        FLEXIO_MCULCD_WriteDataArrayBlocking(&s_flexioLcdDev, chunk, n * 2U);

        pixels += n;
        count -= n;
    }
}

void LCD_EndWindow(void)
{
    FLEXIO_MCULCD_StopTransfer(&s_flexioLcdDev);
}

void LCD_PushPixels(const uint16_t *pixels, uint32_t count)
{
    LCD_PushPixelsOpen(pixels, count);
    LCD_EndWindow();
}

void LCD_DrawImage(uint16_t x0, uint16_t y0, uint16_t width, uint16_t height, const uint16_t *pixels)
{
    LCD_SetWindow(x0, y0, (uint16_t)(x0 + width - 1U), (uint16_t)(y0 + height - 1U));
    LCD_PushPixels(pixels, (uint32_t)width * height);
}
