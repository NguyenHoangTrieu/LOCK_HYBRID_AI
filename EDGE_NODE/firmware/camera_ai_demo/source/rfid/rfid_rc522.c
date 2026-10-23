/*
 * rfid_rc522.c - see rfid_rc522.h.
 *
 * Register map and command sequence are the standard NXP MFRC522 ones
 * (datasheet "MFRC522 - Standard performance MIFARE and NTAG frontend",
 * Rev 3.9), same register addresses/command codes every open MFRC522
 * library uses - PCD_Reset -> TxASK (100% ASK modulation, required by the
 * ISO14443A spec) -> antenna on -> REQA -> anticollision -> select.
 *
 * Only single-size (4-byte) UIDs are decoded (cascade level 1) - this
 * repo's RFID cards (per README) are standard Mifare Classic/NTAG,
 * 4-byte UID. A 7-byte double-size UID would reply 0x88 as the first
 * anticollision byte (cascade tag) - not handled, RFID_Poll() just
 * reports failure for those cards rather than guessing at a 2-level
 * select it was never tested against.
 */

#include "rfid_rc522.h"
#include "app.h"
#include "fsl_debug_console.h"
#include "fsl_gpio.h"
#include "fsl_lpspi.h"
#include "spi1_bus.h"
#include <string.h>

/* MFRC522 register addresses (datasheet Table 24), left-shifted 1 and
 * ORed with 0x80 for a read, per the SPI frame format (datasheet 8.1.2.3):
 * byte0 = (address << 1) | (read ? 0x80 : 0x00), MSB always 0. */
#define RC522_REG_COMMAND      0x01U
#define RC522_REG_COM_IEN      0x02U
#define RC522_REG_COM_IRQ      0x04U
#define RC522_REG_DIV_IRQ      0x05U
#define RC522_REG_ERROR        0x06U
#define RC522_REG_STATUS2      0x08U
#define RC522_REG_FIFO_DATA    0x09U
#define RC522_REG_FIFO_LEVEL   0x0AU
#define RC522_REG_CONTROL      0x0CU
#define RC522_REG_BIT_FRAMING  0x0DU
#define RC522_REG_COLL         0x0EU
#define RC522_REG_MODE         0x11U
#define RC522_REG_TX_CONTROL   0x14U
#define RC522_REG_TX_ASK       0x15U
#define RC522_REG_T_MODE       0x2AU
#define RC522_REG_T_PRESCALER  0x2BU
#define RC522_REG_T_RELOAD_HI  0x2CU
#define RC522_REG_T_RELOAD_LO  0x2DU
#define RC522_REG_VERSION      0x37U

#define RC522_CMD_IDLE       0x00U
#define RC522_CMD_TRANSCEIVE 0x0CU
#define RC522_CMD_SOFT_RESET 0x0FU

#define PICC_CMD_REQA     0x26U
#define PICC_CMD_ANTICOLL 0x93U /* cascade level 1 select code */
#define PICC_CMD_HALT_A   0x50U

#define RC522_SPI_BAUDRATE_HZ 1000000U /* MFRC522 max SPI clock is 10MHz; 1MHz is a conservative, untested-on-hw default */

static void RFID_CsLow(void) { GPIO_PinWrite(DEMO_RFID_CS_GPIO, DEMO_RFID_CS_PIN, 0U); }
static void RFID_CsHigh(void) { GPIO_PinWrite(DEMO_RFID_CS_GPIO, DEMO_RFID_CS_PIN, 1U); }

static uint8_t RFID_ReadReg(uint8_t reg)
{
    uint8_t tx[2] = {(uint8_t)(((reg << 1U) & 0x7EU) | 0x80U), 0x00U};
    uint8_t rx[2] = {0U, 0U};

    (void)SPI1_BUS_SetBaudRate(RC522_SPI_BAUDRATE_HZ);
    RFID_CsLow();
    (void)SPI1_BUS_TransferBlocking(tx, rx, 2U, kLPSPI_MasterPcs1);
    RFID_CsHigh();

    return rx[1];
}

static void RFID_WriteReg(uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = {(uint8_t)((reg << 1U) & 0x7EU), value};

    (void)SPI1_BUS_SetBaudRate(RC522_SPI_BAUDRATE_HZ);
    RFID_CsLow();
    (void)SPI1_BUS_TransferBlocking(tx, NULL, 2U, kLPSPI_MasterPcs1);
    RFID_CsHigh();
}

static void RFID_SetBits(uint8_t reg, uint8_t mask) { RFID_WriteReg(reg, (uint8_t)(RFID_ReadReg(reg) | mask)); }
static void RFID_ClearBits(uint8_t reg, uint8_t mask) { RFID_WriteReg(reg, (uint8_t)(RFID_ReadReg(reg) & (uint8_t)~mask)); }

/* Hardware reset via RST pin (active low, per MFRC522 datasheet 8.5) -
 * pin already idle-high from BOARD_InitBaseboardPeripheralPins(). Delay
 * values are generous (not measured on real hardware) since this path
 * only runs once at boot. */
static void RFID_HardReset(void)
{
    GPIO_PinWrite(DEMO_RFID_RST_GPIO, DEMO_RFID_RST_PIN, 0U);
    SDK_DelayAtLeastUs(2U, SDK_DEVICE_MAXIMUM_CPU_CLOCK_FREQUENCY);
    GPIO_PinWrite(DEMO_RFID_RST_GPIO, DEMO_RFID_RST_PIN, 1U);
    SDK_DelayAtLeastUs(50U * 1000U, SDK_DEVICE_MAXIMUM_CPU_CLOCK_FREQUENCY); /* datasheet: 37.74us min after reset, generous margin */
}

/* Transceive `txLen` bytes, collect up to `*rxLen` response bytes (FIFO).
 * Blocking, short fixed timeout - a non-responding/absent card is the
 * expected common case (REQA with nothing in the field), not an error to
 * dwell on. Returns false on timeout, CRC/parity error, or collision
 * (collision is expected during anticollision and handled by the caller,
 * which only wants the easy single-card case). */
static bool RFID_Transceive(const uint8_t *tx, uint8_t txLen, uint8_t *rx, uint8_t *rxLen, uint8_t txLastBits)
{
    RFID_WriteReg(RC522_REG_COMMAND, RC522_CMD_IDLE);
    RFID_WriteReg(RC522_REG_COM_IRQ, 0x7FU); /* clear all IRQ bits */
    RFID_WriteReg(RC522_REG_FIFO_LEVEL, 0x80U); /* flush FIFO */

    for (uint8_t i = 0U; i < txLen; i++)
    {
        RFID_WriteReg(RC522_REG_FIFO_DATA, tx[i]);
    }

    RFID_WriteReg(RC522_REG_BIT_FRAMING, txLastBits); /* bits 0..2: TxLastBits, bit7 StartSend set below */
    RFID_WriteReg(RC522_REG_COMMAND, RC522_CMD_TRANSCEIVE);
    RFID_SetBits(RC522_REG_BIT_FRAMING, 0x80U); /* StartSend */

    /* ~25ms budget: generous for a single ISO14443A frame (sub-ms on real
     * hardware), bounded so an absent/dead card fails fast every poll
     * cycle rather than stalling the main loop. */
    bool done = false;
    for (uint32_t i = 0U; i < 2500U; i++)
    {
        uint8_t irq = RFID_ReadReg(RC522_REG_COM_IRQ);
        if ((irq & 0x30U) != 0U) /* RxIRq or IdleIRq */
        {
            done = true;
            break;
        }
        if ((irq & 0x01U) != 0U) /* TimerIRq - the MFRC522's own internal timeout */
        {
            break;
        }
        SDK_DelayAtLeastUs(10U, SDK_DEVICE_MAXIMUM_CPU_CLOCK_FREQUENCY);
    }

    RFID_ClearBits(RC522_REG_BIT_FRAMING, 0x80U);

    if (!done)
    {
        return false;
    }

    uint8_t err = RFID_ReadReg(RC522_REG_ERROR);
    if ((err & 0x1BU) != 0U) /* BufferOvfl, ParityErr, ProtocolErr, CollErr */
    {
        return false;
    }

    uint8_t fifoLen = RFID_ReadReg(RC522_REG_FIFO_LEVEL);
    if (fifoLen > *rxLen)
    {
        fifoLen = *rxLen;
    }
    for (uint8_t i = 0U; i < fifoLen; i++)
    {
        rx[i] = RFID_ReadReg(RC522_REG_FIFO_DATA);
    }
    *rxLen = fifoLen;

    return true;
}

bool RFID_Init(void)
{
    SPI1_BUS_Init();
    RFID_HardReset();

    RFID_WriteReg(RC522_REG_COMMAND, RC522_CMD_SOFT_RESET);
    SDK_DelayAtLeastUs(50U * 1000U, SDK_DEVICE_MAXIMUM_CPU_CLOCK_FREQUENCY);

    /* Timer: used internally by Transceive's TimerIRq as a hardware
     * timeout backstop (datasheet 8.7) - 40kHz tick, ~25ms reload,
     * independent of the software deadline loop above which is the one
     * actually bounding RFID_Poll()'s worst case. */
    RFID_WriteReg(RC522_REG_T_MODE, 0x80U);
    RFID_WriteReg(RC522_REG_T_PRESCALER, 0xA9U);
    RFID_WriteReg(RC522_REG_T_RELOAD_HI, 0x03U);
    RFID_WriteReg(RC522_REG_T_RELOAD_LO, 0xE8U);

    RFID_WriteReg(RC522_REG_TX_ASK, 0x40U); /* Force100ASK - required by ISO14443A, every MFRC522 reference driver sets this */
    RFID_WriteReg(RC522_REG_MODE, 0x3DU);   /* TxWaitRF + CRC preset 0x6363 (datasheet default for ISO14443A) */

    RFID_SetBits(RC522_REG_TX_CONTROL, 0x03U); /* antenna on (Tx1RFEn | Tx2RFEn) */

    uint8_t version = RFID_ReadReg(RC522_REG_VERSION);
    /* Known silicon revisions: 0x88 (clone), 0x90/0x91/0x92 (genuine NXP) -
     * datasheet 9.3.4.8. 0x00/0xFF means the SPI line is stuck (no
     * module, bad wiring, or CS/clock fault) - not a protocol error. */
    bool present = (version != 0x00U) && (version != 0xFFU);
    if (!present)
    {
        PRINTF("RFID: MFRC522 not responding (VersionReg=0x%02X) - check wiring, continuing without RFID\r\n",
               version);
    }
    else
    {
        PRINTF("RFID: MFRC522 ready (VersionReg=0x%02X)\r\n", version);
    }

    return present;
}

bool RFID_Poll(rfid_uid_t *outUid)
{
    memset(outUid, 0, sizeof(*outUid));

    /* REQA (7-bit short frame) - only IDLE cards answer; a card left
     * HALTed from a previous read stays silent until removed/re-powered,
     * which is what keeps a held card from re-triggering every poll. */
    uint8_t reqa    = PICC_CMD_REQA;
    uint8_t atqa[2] = {0};
    uint8_t atqaLen = (uint8_t)sizeof(atqa);
    if (!RFID_Transceive(&reqa, 1U, atqa, &atqaLen, 0x07U))
    {
        return false; /* no card in field - the expected common case */
    }

    /* Anticollision, cascade level 1: send NVB=0x20 (ask every card to
     * reply with its full UID) - works as-is only when exactly one card
     * is in the field, which matches ASM-4 (one attendee at a time) and
     * this driver's single-card scope. */
    uint8_t anticoll[2]  = {PICC_CMD_ANTICOLL, 0x20U};
    uint8_t uidResp[5]   = {0}; /* 4 UID bytes + 1 BCC */
    uint8_t uidRespLen   = (uint8_t)sizeof(uidResp);
    if (!RFID_Transceive(anticoll, 2U, uidResp, &uidRespLen, 0x00U) || uidRespLen != 5U)
    {
        return false;
    }

    if (uidResp[0] == 0x88U)
    {
        /* Cascade tag - double/triple-size UID, not decoded (see file
         * header comment). Halt whatever's there so it doesn't keep
         * answering REQA and send the card to sleep cleanly. */
        uint8_t haltCmd    = PICC_CMD_HALT_A;
        uint8_t haltRsp[2] = {0};
        uint8_t haltLen    = (uint8_t)sizeof(haltRsp);
        (void)RFID_Transceive(&haltCmd, 1U, haltRsp, &haltLen, 0x00U);
        return false;
    }

    uint8_t bcc = (uint8_t)(uidResp[0] ^ uidResp[1] ^ uidResp[2] ^ uidResp[3]);
    if (bcc != uidResp[4])
    {
        return false; /* corrupt read - BCC mismatch */
    }

    /* Select (cascade level 1, full 4-byte UID + BCC, then 2-byte CRC).
     * CRC is computed by the MFRC522's own CRC coprocessor rather than in
     * software here - simpler and avoids shipping a second CRC
     * implementation that has to match the reader's exactly. */
    uint8_t selectCmd[7] = {PICC_CMD_ANTICOLL, 0x70U, uidResp[0], uidResp[1], uidResp[2], uidResp[3], uidResp[4]};
    RFID_WriteReg(RC522_REG_COMMAND, RC522_CMD_IDLE);
    RFID_WriteReg(RC522_REG_DIV_IRQ, 0x04U);
    RFID_WriteReg(RC522_REG_FIFO_LEVEL, 0x80U);
    for (uint8_t i = 0U; i < 7U; i++)
    {
        RFID_WriteReg(RC522_REG_FIFO_DATA, selectCmd[i]);
    }
    RFID_WriteReg(RC522_REG_COMMAND, 0x03U); /* CalcCRC */
    bool crcDone = false;
    for (uint32_t i = 0U; i < 1000U; i++)
    {
        if ((RFID_ReadReg(RC522_REG_DIV_IRQ) & 0x04U) != 0U)
        {
            crcDone = true;
            break;
        }
        SDK_DelayAtLeastUs(10U, SDK_DEVICE_MAXIMUM_CPU_CLOCK_FREQUENCY);
    }
    RFID_WriteReg(RC522_REG_COMMAND, RC522_CMD_IDLE);
    if (!crcDone)
    {
        return false;
    }
    uint8_t crcLo = RFID_ReadReg(0x22U); /* CRCResultRegL */
    uint8_t crcHi = RFID_ReadReg(0x21U); /* CRCResultRegH */

    uint8_t fullSelect[9];
    memcpy(fullSelect, selectCmd, 7U);
    fullSelect[7] = crcLo;
    fullSelect[8] = crcHi;

    uint8_t sak[3] = {0};
    uint8_t sakLen = (uint8_t)sizeof(sak);
    if (!RFID_Transceive(fullSelect, 9U, sak, &sakLen, 0x00U) || sakLen == 0U)
    {
        return false;
    }

    outUid->length = 4U;
    memcpy(outUid->uid, uidResp, 4U);

    /* Halt the card (FR-9 just reports the UID once per presentation;
     * halting is what makes "once per presentation" true - see
     * rfid_rc522.h). Best-effort: a failure here doesn't invalidate the
     * UID already read. */
    uint8_t haltCmd    = PICC_CMD_HALT_A;
    uint8_t haltRsp[2] = {0};
    uint8_t haltLen    = (uint8_t)sizeof(haltRsp);
    (void)RFID_Transceive(&haltCmd, 1U, haltRsp, &haltLen, 0x00U);

    return true;
}
