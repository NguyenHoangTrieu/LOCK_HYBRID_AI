/*
 * sd_spi_disk.h - SD card over SPI (LPSPI1, Arduino D10..D13 - see
 * BOARD_InitSdCardPins() in pin_mux.c; that bus is now shared with the LCD
 * and touch controller, see ../spi1_bus.h), wired into FatFs as physical
 * drive 0 via the standard diskio.h API (disk_initialize/status/read/
 * write/ioctl, defined in sd_spi_disk.c - called by ff.c, not meant to be
 * called directly).
 *
 * Replaces the SDK's own middleware/fatfs/source/fsl_sdspi_disk/ glue,
 * which is hardcoded to the DSPI peripheral (Kinetis-family SPI) that
 * doesn't exist on the MCXN947 (LPSPI-family chip) - see sd_spi_disk.c
 * for the LPSPI1-based replacement.
 */
#ifndef _SD_SPI_DISK_H_
#define _SD_SPI_DISK_H_

#include <stdbool.h>
#include <stdint.h>

/*! @brief True once SDSPI_Init() has completed successfully - snapshot.c
 *  checks this before attempting f_mount()/f_open() so a missing/dead
 *  card just skips snapshots instead of stalling the main loop. Becomes
 *  true lazily, on the first FatFs call that triggers disk_initialize()
 *  (normally the first f_mount()). */
bool SDCARD_DISK_IsReady(void);

/*! @brief Raw capacity the SD-over-SPI driver itself detected (from the
 *  card's CSD register, via SDSPI_Init() - see fsl_sdspi.c's
 *  SDSPI_DecodeCsd()), in bytes. Only valid once SDCARD_DISK_IsReady()
 *  is true. Diagnostic: compare against the card's real, known capacity
 *  to rule out a CSD-decode/capacity-detection mismatch as the cause of
 *  writes failing while small reads (f_getfree()) succeed - see
 *  WORKLOG.md's SD write-failure investigation. */
uint64_t SDCARD_DISK_GetCapacityBytes(void);

#endif /* _SD_SPI_DISK_H_ */
