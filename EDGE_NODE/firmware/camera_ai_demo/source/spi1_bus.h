/*
 * spi1_bus.h - shared hardware LPSPI1 bus, Arduino header D10..D13.
 *
 * SCK/MOSI/MISO (D13/D11/D12) are physically shared by three devices on
 * the 2.4" SPI TFT module: the microSD slot (hardware PCS0 on D10), the
 * LCD panel, and the touch controller. Only the microSD slot uses the
 * peripheral's own hardware chip-select - SDSPI_Init() needs to flip its
 * active polarity at runtime, which only works through real PCS hardware.
 * LCD and touch each use a plain GPIO pin for CS instead, and pass
 * kLPSPI_MasterPcs1 as their transfer's PCS flag - a "don't care" value,
 * since PCS1 is never muxed to a physical pin on this board.
 *
 * Because the bus is shared, baud rate is not a one-time setting - every
 * driver must reclaim its own rate via SPI1_BUS_SetBaudRate() immediately
 * before its own transfers, not just once at init. That call also
 * recomputes the PCS/delay timing registers every time, not just the SCK
 * divider - confirmed on real hardware that leaving those stale after a
 * baud change costs ~1.25us of dead time per byte (see WORKLOG.md).
 */
#ifndef _SPI1_BUS_H_
#define _SPI1_BUS_H_

#include <stdint.h>
#include "fsl_common.h"

/*! @brief One-time LPSPI1 bring-up (mode 0, MSB-first, 8-bit frames).
 *  Safe to call from every driver's own Init() - a no-op after the
 *  first call. */
void SPI1_BUS_Init(void);

/*! @brief Reclaim the bus at the given baud rate - call this right before
 *  every transfer, not just once at init. Returns the actual achieved
 *  baud rate (0 on failure). */
uint32_t SPI1_BUS_SetBaudRate(uint32_t baudRate_Bps);

/*! @brief LPSPI1's current source clock frequency - diagnostic, lets a
 *  caller compute what rate a transfer actually ran at. */
uint32_t SPI1_BUS_GetSourceClockFreq(void);

/*! @brief Blocking transfer of `size` bytes. `rxData` may be NULL for a
 *  write-only transfer (LCD); `txData` may be NULL for a read-only one
 *  (touch ADC reads). `pcs` is a kLPSPI_MasterPcsN flag - PCS1 for
 *  LCD/touch (unrouted), PCS0 for the SD slot (real hardware CS). */
status_t SPI1_BUS_TransferBlocking(const uint8_t *txData, uint8_t *rxData, uint32_t size, uint32_t pcs);

/*! @brief One-time-per-frame setup for the eDMA pixel-push path below -
 *  call ONCE before a run of SPI1_BUS_TransferBytesDMA() calls, not once
 *  per chunk. `pcs` - same meaning as SPI1_BUS_TransferBlocking()'s.
 *  Unconditionally clears TCR.RXMSK/TXMSK, which every prior write-only
 *  blocking transfer leaves set - required or the RX eDMA channel this
 *  path waits on never completes (confirmed via live register trace, see
 *  WORKLOG.md). */
status_t SPI1_BUS_PrepareDMA(uint32_t pcs);

/*! @brief Write-only transfer of `size` bytes via eDMA instead of the
 *  CPU-polled path above. Stays on the shared bus's normal 8-bit frames.
 *  SPI1_BUS_PrepareDMA() must already have been called. Callers needing
 *  MSB-first wire order from a little-endian buffer must byte-swap into
 *  `data` themselves first. Blocking - waits for eDMA completion before
 *  returning. Only for LCD pixel data; SD/touch use the polled path. */
status_t SPI1_BUS_TransferBytesDMA(const uint8_t *data, uint32_t size);

#ifdef DUALCORE_RTOS
/*! @brief Dual-core build only: the LCD and SD card tasks both touch this
 *  bus from separate, preemptible FreeRTOS tasks. Without this, SD file
 *  creation fails intermittently when a concurrent LCD push interleaves
 *  bus traffic mid-command (confirmed on real hardware). Call
 *  SPI1_BUS_CreateLock() once from main() before starting the scheduler;
 *  wrap every complete logical transaction (not just one low-level call)
 *  with Lock/Unlock. */
void SPI1_BUS_CreateLock(void);
void SPI1_BUS_Lock(void);
void SPI1_BUS_Unlock(void);

/*! @brief Dual-core build only: the plain mutex above only stops another
 *  task from touching the bus - it doesn't stop the scheduler (or the
 *  core0<->core1 MCMGR doorbell interrupt, which runs at the same
 *  priority as a syscall) from preempting the lock holder mid-transaction,
 *  which produced real LCD tearing on hardware. Uses
 *  taskENTER_CRITICAL()/EXIT_CRITICAL() to mask both task switches and
 *  that interrupt for the duration - see WORKLOG.md. Wrap the same
 *  complete-transaction scope Lock/Unlock wraps (LCD_Init()'s panel init,
 *  LCD_DrawImage()) with these instead. */
void SPI1_BUS_LockNoPreempt(void);
void SPI1_BUS_UnlockNoPreempt(void);
#endif

#endif /* _SPI1_BUS_H_ */
