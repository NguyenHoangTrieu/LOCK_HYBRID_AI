/*
 * spi1_bus.c - see spi1_bus.h.
 *
 * SPI1_BUS_TransferBytesDMA() exists because fsl_lpspi.c's blocking path
 * pushes data one byte at a time into the TX FIFO - 153,600 register
 * accesses for one 320x240 frame, a bus-bridge latency cost no amount of
 * tuning removes. eDMA moves that into hardware instead. See
 * SPI1_BUS_PrepareDMA()'s comment for a real hang this hit and fixed.
 */

#include "spi1_bus.h"
#include "fsl_clock.h"
#include "fsl_common.h" /* DWT, SystemCoreClock - SPI1_BUS_TransferPixelsDMA()'s completion-wait timeout. */
#include "fsl_edma.h"
#include "fsl_edma_soc.h" /* kDma0RequestMuxLpFlexcomm1Tx/Rx */
#include "fsl_lpspi.h"
#include "fsl_lpspi_edma.h"
#include <stdbool.h>

#ifdef DUALCORE_RTOS
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

static SemaphoreHandle_t s_busMutex;

/* Plain mutex, not recursive - only one call site ever holds it across a
 * nested disk_*() call, so recursion isn't needed. A recursive mutex was
 * tried once and broke SD card mount for reasons never fully understood -
 * reverted rather than ship an unexplained fix (see WORKLOG.md). */
void SPI1_BUS_CreateLock(void)
{
    s_busMutex = xSemaphoreCreateMutex();
    configASSERT(s_busMutex != NULL);
}

void SPI1_BUS_Lock(void)
{
    (void)xSemaphoreTake(s_busMutex, portMAX_DELAY);
}

void SPI1_BUS_Unlock(void)
{
    (void)xSemaphoreGive(s_busMutex);
}

/* See spi1_bus.h - the plain mutex above only stops another task from
 * touching the bus, not the scheduler or an ISR from preempting the lock
 * holder mid-transaction. Confirmed on real hardware that
 * vTaskSuspendAll() (blocks task switches only) wasn't enough once the
 * core0<->core1 MCMGR mailbox interrupt was added - it runs at exactly
 * FreeRTOS's maskable priority threshold and can fire mid-LCD-transfer.
 * taskENTER_CRITICAL()/EXIT_CRITICAL() mask that interrupt and block task
 * switches in one primitive, fully replacing vTaskSuspendAll() rather
 * than layering both. Mutex first, then critical section - xSemaphoreTake()
 * can block/yield and must never run inside a critical section. */
void SPI1_BUS_LockNoPreempt(void)
{
    SPI1_BUS_Lock();
    taskENTER_CRITICAL();
}

void SPI1_BUS_UnlockNoPreempt(void)
{
    taskEXIT_CRITICAL();
    SPI1_BUS_Unlock();
}
#endif

#define SPI1_BUS_BASEADDR LPSPI1
#define SPI1_BUS_CLK_FREQ CLOCK_GetLPFlexCommClkFreq(1u) /* FRO_HF/1 = 48MHz - see hardware_init.c. */

/* DMA0 channels 0/1 - fixed, confirmed unused elsewhere (camera capture
 * uses the separate SmartDMA peripheral). Matches mcuxsdk's own
 * frdmmcxn947 lpspi/edma_b2b_transfer example for this LPSPI1 instance. */
#define SPI1_BUS_DMA_BASEADDR   DMA0
#define SPI1_BUS_DMA_RX_CHANNEL 0U
#define SPI1_BUS_DMA_TX_CHANNEL 1U

static bool s_spi1BusInitialized = false;

static edma_handle_t s_dmaRxHandle;
static edma_handle_t s_dmaTxHandle;
static lpspi_master_edma_handle_t s_lpspiDmaHandle;
static volatile bool s_dmaTransferDone = false;

static void SPI1_BUS_DmaCallback(LPSPI_Type *base, lpspi_master_edma_handle_t *handle, status_t status, void *userData)
{
    (void)base;
    (void)handle;
    (void)status;
    (void)userData;
    s_dmaTransferDone = true;
}

void SPI1_BUS_Init(void)
{
    if (s_spi1BusInitialized)
    {
        return;
    }

    lpspi_master_config_t masterConfig;
    LPSPI_MasterGetDefaultConfig(&masterConfig);
    /* baudRate here is a throwaway baseline - every real transfer
     * reclaims its own rate via SPI1_BUS_SetBaudRate(). Defaults already
     * match what every device on this bus needs: 8 bits/frame, mode 0. */
    masterConfig.baudRate = 400000U;
    masterConfig.whichPcs = kLPSPI_Pcs0;
    LPSPI_MasterInit(SPI1_BUS_BASEADDR, &masterConfig, SPI1_BUS_CLK_FREQ);

    edma_config_t edmaConfig;
    EDMA_GetDefaultConfig(&edmaConfig);
    EDMA_Init(SPI1_BUS_DMA_BASEADDR, &edmaConfig);

    EDMA_CreateHandle(&s_dmaRxHandle, SPI1_BUS_DMA_BASEADDR, SPI1_BUS_DMA_RX_CHANNEL);
    EDMA_CreateHandle(&s_dmaTxHandle, SPI1_BUS_DMA_BASEADDR, SPI1_BUS_DMA_TX_CHANNEL);
    EDMA_SetChannelMux(SPI1_BUS_DMA_BASEADDR, SPI1_BUS_DMA_RX_CHANNEL, kDma0RequestMuxLpFlexcomm1Rx);
    EDMA_SetChannelMux(SPI1_BUS_DMA_BASEADDR, SPI1_BUS_DMA_TX_CHANNEL, kDma0RequestMuxLpFlexcomm1Tx);

    LPSPI_MasterTransferCreateHandleEDMA(SPI1_BUS_BASEADDR, &s_lpspiDmaHandle, SPI1_BUS_DmaCallback, NULL,
                                         &s_dmaRxHandle, &s_dmaTxHandle);

    s_spi1BusInitialized = true;
}

uint32_t SPI1_BUS_SetBaudRate(uint32_t baudRate_Bps)
{
    uint32_t prescaler; /* out-only, LPSPI_MasterSetBaudRate() asserts this is non-NULL. */
    uint32_t actualBaud;

    /* LPSPI_MasterSetBaudRate() silently returns 0 (failure) unless the
     * peripheral is disabled first. */
    LPSPI_Enable(SPI1_BUS_BASEADDR, false);
    actualBaud = LPSPI_MasterSetBaudRate(SPI1_BUS_BASEADDR, baudRate_Bps, SPI1_BUS_CLK_FREQ, &prescaler);

    /* LPSPI_MasterSetBaudRate() only updates the SCK divider, not the
     * PCS/between-transfer delay registers - those stay baked in from
     * SPI1_BUS_Init()'s throwaway 400kHz baseline unless recomputed here.
     * Confirmed on real hardware this cost ~1.25us of stale delay per
     * byte, dominating fps at higher rates (see WORKLOG.md). Recompute
     * every time any device on this shared bus changes rate. */
    if (actualBaud != 0U)
    {
        uint32_t delayNs = (1000000000U / actualBaud) / 2U;
        (void)LPSPI_MasterSetDelayTimes(SPI1_BUS_BASEADDR, delayNs, kLPSPI_PcsToSck, SPI1_BUS_CLK_FREQ);
        (void)LPSPI_MasterSetDelayTimes(SPI1_BUS_BASEADDR, delayNs, kLPSPI_LastSckToPcs, SPI1_BUS_CLK_FREQ);
        (void)LPSPI_MasterSetDelayTimes(SPI1_BUS_BASEADDR, delayNs, kLPSPI_BetweenTransfer, SPI1_BUS_CLK_FREQ);
    }

    LPSPI_Enable(SPI1_BUS_BASEADDR, true);

    return actualBaud;
}

uint32_t SPI1_BUS_GetSourceClockFreq(void)
{
    return SPI1_BUS_CLK_FREQ;
}

status_t SPI1_BUS_TransferBlocking(const uint8_t *txData, uint8_t *rxData, uint32_t size, uint32_t pcs)
{
    lpspi_transfer_t transfer = {
        .txData      = txData,
        .rxData      = rxData,
        .dataSize    = size,
        .configFlags = pcs,
    };

    return LPSPI_MasterTransferBlocking(SPI1_BUS_BASEADDR, &transfer);
}

/* Generous, not tight - a full frame completes in low tens of ms over DMA
 * at any baud this bus runs at. Bounds the wait in case a transfer never
 * completes, same "fail loud, don't hang" pattern as sd_spi_disk.c. */
#define SPI1_BUS_DMA_TIMEOUT_MS 200U

status_t SPI1_BUS_PrepareDMA(uint32_t pcs)
{
    status_t status = LPSPI_MasterTransferPrepareEDMALite(SPI1_BUS_BASEADDR, &s_lpspiDmaHandle, pcs);

    /* Root cause of a real eDMA hang (confirmed via live register trace,
     * see WORKLOG.md): LPSPI_MasterTransferBlocking() sets TCR.RXMSK=1
     * whenever called with rxData==NULL (every LCD command byte on this
     * shared bus), telling the hardware to discard incoming data instead
     * of storing it - and nothing clears it back. PrepareEDMALite() never
     * touches RXMSK either, so it silently inherits that mask, meaning
     * the RX FIFO could never fill and the RX eDMA channel this transfer
     * waits on would never complete. Clear both mask bits explicitly. */
    if (status == kStatus_Success)
    {
        SPI1_BUS_BASEADDR->TCR &= ~(LPSPI_TCR_RXMSK_MASK | LPSPI_TCR_TXMSK_MASK);
    }

    return status;
}

status_t SPI1_BUS_TransferBytesDMA(const uint8_t *data, uint32_t size)
{
    /* Caller must have already called SPI1_BUS_PrepareDMA() once, not
     * per-chunk - confirmed on real hardware that repeating it per chunk
     * re-does a real per-call setup cost that otherwise dominates. */
    lpspi_transfer_t transfer = {
        .txData      = data,
        .rxData      = NULL,
        .dataSize    = size,
        .configFlags = 0U, /* Unused by the *Lite variant - config already applied by Prepare(). */
    };

    s_dmaTransferDone = false;
    status_t status   = LPSPI_MasterTransferEDMALite(SPI1_BUS_BASEADDR, &s_lpspiDmaHandle, &transfer);
    if (status == kStatus_Success)
    {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
        uint32_t deadlineCycle = DWT->CYCCNT + (SystemCoreClock / 1000U) * SPI1_BUS_DMA_TIMEOUT_MS;

        while (!s_dmaTransferDone)
        {
            if ((int32_t)(DWT->CYCCNT - deadlineCycle) >= 0)
            {
                /* Reset the handle back to idle - without this, a single
                 * timeout permanently wedges every future DMA transfer
                 * (confirmed on real hardware - see WORKLOG.md). */
                LPSPI_MasterTransferAbortEDMA(SPI1_BUS_BASEADDR, &s_lpspiDmaHandle);
                status = kStatus_Timeout;
                break;
            }
        }
    }

    return status;
}
