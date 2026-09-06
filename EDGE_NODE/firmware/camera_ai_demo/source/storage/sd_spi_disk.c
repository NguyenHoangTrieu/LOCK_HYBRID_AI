/*
 * sd_spi_disk.c - see sd_spi_disk.h.
 *
 * Two things live in this one file, same as the SDK's own (DSPI-based,
 * unusable on this chip - see sd_spi_disk.h) reference glue:
 *  1. An sdspi_host_t implementation (SDCARD_SPI_*) driving LPSPI1 in
 *     hardware - the 4 callbacks fsl_sdspi.c needs to talk SD-over-SPI
 *     without knowing which SPI peripheral is underneath.
 *  2. The 5 diskio.h functions ff.c calls directly - single hardcoded
 *     physical drive 0 (this project only ever has one card).
 *
 * LPSPI1 is a shared bus (see ../spi1_bus.h) - the SD card is the only
 * device still using the peripheral's real hardware PCS0 (D10), since
 * SDSPI_Init() needs to flip CS polarity at runtime for the card's
 * power-up sequence, which only works through real PCS hardware (the
 * LCD/touch use plain GPIO CS instead). disk_read()/disk_write() reclaim
 * the SD card's baud rate before every transfer since the LCD/touch
 * driver may have left the bus at a different rate.
 *
 * Confirmed on real hardware: if the card/wiring is bad enough that every
 * SPI response byte comes back wrong, SDSPI_Init()'s own nested retry
 * loops can multiply out to minutes, even though each one is individually
 * bounded. SDCARD_SPI_Exchange() below enforces its own short wall-clock
 * deadline across the whole init attempt so a bad card fails fast
 * regardless (see WORKLOG.md).
 */

#include "sd_spi_disk.h"
#include "ff.h" /* Must come before diskio.h - defines BYTE/UINT/LBA_t/... that diskio.h's prototypes need. */
#include "diskio.h"
#include "fsl_common.h" /* DWT, SystemCoreClock - see SDCARD_SPI_Exchange()'s deadline check below. */
#include "fsl_debug_console.h"
#include "fsl_lpspi.h"
#include "fsl_sdspi.h"
#include "spi1_bus.h"

/* Only needed here for SDCARD_SPI_CsActivePolarity()'s
 * LPSPI_SetAllPcsPolarity() call - everything else routes through
 * spi1_bus.h, shared with the LCD/touch controller. */
#define SD_SPI_BASEADDR LPSPI1

/* Operating-speed cap, not the mandatory 400kHz identification speed
 * (set separately inside SDSPI_Init() itself). Confirmed on real
 * hardware that leaving this at the identification-phase value pins
 * every transfer at 400kHz forever (a 150KB BMP took ~3.3s to write).
 * 8MHz is a conservative middle ground given this card/shield needed a
 * pull-up fix to work at all - not pushed to the driver's 25MHz ceiling
 * without confirming that's stable too. */
#define SD_SPI_OPERATING_BAUDRATE 8000000U

/* Real SD-over-SPI init normally completes in well under 1 second - 2
 * seconds is generous headroom, not a tight budget. */
#define SD_SPI_INIT_TIMEOUT_MS 2000U

static sdspi_host_t s_host;
static sdspi_card_t s_card;
static bool s_cardReady = false;
static uint32_t s_initDeadlineCycle;
static bool s_initTimedOut;
/* True only while disk_initialize() -> SDSPI_Init() is actually running -
 * see SDCARD_SPI_Exchange()'s comment for why this matters. */
static bool s_initInProgress;

/* -------------------------------------------------------------------- */
/* sdspi_host_t callbacks - LPSPI1 hardware SPI, see the file comment.   */
/* -------------------------------------------------------------------- */

static void SDCARD_SPI_Init(void)
{
    /* Idempotent - a no-op if the LCD or touch driver already brought up
     * the bus. SDSPI_Init() immediately calls setFrequency() anyway, so
     * the shared module's own baseline baud rate doesn't matter here. */
    SPI1_BUS_Init();

    /* DWT is already enabled elsewhere (AI_MODEL_Init()), but enable it
     * defensively here too so this file doesn't depend on that ordering -
     * doesn't reset CYCCNT, just ensures it's running. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    s_initDeadlineCycle = DWT->CYCCNT + (SystemCoreClock / 1000U) * SD_SPI_INIT_TIMEOUT_MS;
    s_initTimedOut       = false;
}

static status_t SDCARD_SPI_SetFrequency(uint32_t frequency)
{
    return (SPI1_BUS_SetBaudRate(frequency) == 0U) ? kStatus_Fail : kStatus_Success;
}

static status_t SDCARD_SPI_Exchange(uint8_t *in, uint8_t *out, uint32_t size)
{
    /* fsl_sdspi.c's own retry loops bail out the moment exchange() itself
     * reports failure, so failing fast here once bounds the whole init
     * attempt to SD_SPI_INIT_TIMEOUT_MS regardless of how deep its
     * internal retries are. Gated on s_initInProgress - this check used
     * to run unconditionally, which made the very first real read/write
     * after boot fail instantly even on a healthy card, since exchange()
     * is also called during normal file I/O long after the init deadline
     * had passed (confirmed on real hardware - see WORKLOG.md). */
    if (s_initInProgress &&
        (s_initTimedOut || ((int32_t)(DWT->CYCCNT - s_initDeadlineCycle) >= 0)))
    {
        if (!s_initTimedOut)
        {
            PRINTF("Snapshot: SD card init timed out after %ums (no valid response) - giving up.\r\n",
                   SD_SPI_INIT_TIMEOUT_MS);
        }
        s_initTimedOut = true;
        return kStatus_Fail;
    }

    return SPI1_BUS_TransferBlocking(in, out, size, kLPSPI_MasterPcs0 | kLPSPI_MasterPcsContinuous);
}

static void SDCARD_SPI_CsActivePolarity(sdspi_cs_active_polarity_t polarity)
{
    LPSPI_SetAllPcsPolarity(SD_SPI_BASEADDR, (polarity == kSDSPI_CsActivePolarityLow) ? kLPSPI_Pcs0ActiveLow : 0U);
}

/* -------------------------------------------------------------------- */
/* diskio.h - single physical drive 0, see the file comment.            */
/* -------------------------------------------------------------------- */

bool SDCARD_DISK_IsReady(void)
{
    return s_cardReady;
}

uint64_t SDCARD_DISK_GetCapacityBytes(void)
{
    return (uint64_t)s_card.blockCount * (uint64_t)s_card.blockSize;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0U)
    {
        return STA_NOINIT;
    }

    s_host.busBaudRate      = SD_SPI_OPERATING_BAUDRATE;
    s_host.setFrequency     = SDCARD_SPI_SetFrequency;
    s_host.exchange         = SDCARD_SPI_Exchange;
    s_host.init             = SDCARD_SPI_Init;
    s_host.csActivePolarity = SDCARD_SPI_CsActivePolarity;
    s_card.host             = &s_host;

    /* Dual-core build only - tight lock around just this one call, not
     * the whole SNAPSHOT_Init()/OnFrame() sequence, and must be the plain
     * mutex, not SPI1_BUS_LockNoPreempt(): both a wider lock scope and
     * the no-preempt variant were each confirmed on real hardware to
     * break SD mount (ACMD41 handshake failure) for reasons not fully
     * pinned down - reverting to this exact narrow scope reliably fixes
     * it (see WORKLOG.md). Don't retry either alternative without new
     * evidence. */
    s_initInProgress = true;
#ifdef DUALCORE_RTOS
    SPI1_BUS_Lock();
#endif
    s_cardReady      = (SDSPI_Init(&s_card) == kStatus_Success);
#ifdef DUALCORE_RTOS
    SPI1_BUS_Unlock();
#endif
    s_initInProgress = false;
    return s_cardReady ? 0U : STA_NOINIT;
}

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0U)
    {
        return STA_NOINIT;
    }
    return s_cardReady ? 0U : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if ((pdrv != 0U) || !s_cardReady)
    {
        return RES_PARERR;
    }
    /* Reclaim the SD card's baud rate - the LCD/touch driver may have
     * left the shared bus at a different rate since the last SD access;
     * unlike disk_initialize(), plain reads/writes don't call
     * setFrequency() again on their own. */
#ifdef DUALCORE_RTOS
    SPI1_BUS_Lock(); /* See disk_initialize()'s comment above. */
#endif
    (void)SPI1_BUS_SetBaudRate(SD_SPI_OPERATING_BAUDRATE);
    DRESULT result = (SDSPI_ReadBlocks(&s_card, buff, sector, count) == kStatus_Success) ? RES_OK : RES_ERROR;
#ifdef DUALCORE_RTOS
    SPI1_BUS_Unlock();
#endif
    return result;
}

#if FF_FS_READONLY == 0
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if ((pdrv != 0U) || !s_cardReady)
    {
        return RES_PARERR;
    }
#ifdef DUALCORE_RTOS
    SPI1_BUS_Lock(); /* See disk_initialize()'s comment above. */
#endif
    (void)SPI1_BUS_SetBaudRate(SD_SPI_OPERATING_BAUDRATE); /* See disk_read()'s comment above. */
    DRESULT result = (SDSPI_WriteBlocks(&s_card, (uint8_t *)buff, sector, count) == kStatus_Success) ? RES_OK : RES_ERROR;
#ifdef DUALCORE_RTOS
    SPI1_BUS_Unlock();
#endif
    return result;
}
#endif

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if ((pdrv != 0U) || !s_cardReady)
    {
        return RES_PARERR;
    }

    switch (cmd)
    {
        case GET_SECTOR_COUNT:
            *(uint32_t *)buff = s_card.blockCount;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD *)buff = (WORD)s_card.blockSize;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *(uint32_t *)buff = s_card.csd.eraseSectorSize;
            return RES_OK;
        case CTRL_SYNC:
            return RES_OK;
        default:
            return RES_PARERR;
    }
}
