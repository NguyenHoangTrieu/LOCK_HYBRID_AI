/*
 * ffconf.h - FatFs configuration for this project.
 *
 * Hand-written, NOT the SDK's Kconfig-generated ffconf_gen.h - the SDK's
 * only pre-built SD-over-SPI glue (middleware/fatfs/source/fsl_sdspi_disk/)
 * is hardcoded to the DSPI peripheral, which doesn't exist on the MCXN947
 * (LPSPI-family chip) - see sd_spi_disk.c for the replacement glue this
 * project uses instead. Values below are tuned for exactly this project's
 * use case (write-only snapshot BMPs, single card, one file open at a
 * time, no RTC) to keep FatFs's static RAM footprint as small as
 * possible - see FF_FS_TINY/FF_USE_LFN/FF_FS_RPATH below.
 */
#ifndef _FFCONF_H_
#define _FFCONF_H_

#define FFCONF_DEF 80386

/*---------------------------------------------------------------------------/
/ Function Configurations
/---------------------------------------------------------------------------*/
#define FF_FS_READONLY 0 /* Need f_write() for snapshot BMPs. */
#define FF_FS_MINIMIZE  0
#define FF_USE_FIND     0
#define FF_USE_MKFS     0 /* Card is assumed pre-formatted FAT - not this firmware's job. */
#define FF_USE_FASTSEEK 0
#define FF_USE_EXPAND   0
#define FF_USE_CHMOD    0
#define FF_USE_LABEL    0
#define FF_USE_FORWARD  0
#define FF_USE_STRFUNC  0
#define FF_PRINT_LLI    0
#define FF_PRINT_FLOAT  0
#define FF_STRF_ENCODE  0

/*---------------------------------------------------------------------------/
/ Locale and Namespace Configurations
/---------------------------------------------------------------------------*/
#define FF_CODE_PAGE 437 /* U.S. - filenames here are plain ASCII (FACExxxx.BMP). */

/* Long filenames need a dedicated (FF_MAX_LFN+1)*2-byte working buffer -
 * not worth it for fixed 8.3 auto-generated names (FACE0001.BMP etc). */
#define FF_USE_LFN      0
#define FF_MAX_LFN      255
#define FF_LFN_UNICODE  0
#define FF_LFN_BUF      255
#define FF_SFN_BUF      12

#define FF_FS_RPATH 0 /* No subdirectories/chdir needed - flat snapshot folder only. */

/*---------------------------------------------------------------------------/
/ Drive/Volume Configurations
/---------------------------------------------------------------------------*/
#define FF_VOLUMES        1 /* Exactly one card, no other logical drives. */
#define FF_STR_VOLUME_ID  0
#define FF_MULTI_PARTITION 0

#define FF_MIN_SS 512
#define FF_MAX_SS 512 /* Fixed 512B sectors - matches every real SD card. */

#define FF_LBA64    0
#define FF_MIN_GPT  0x10000000
#define FF_USE_TRIM 0

/*---------------------------------------------------------------------------/
/ System Configurations
/---------------------------------------------------------------------------*/
/* TINY=1 removes FIL's own private 512B sector buffer, sharing the
 * FATFS object's single window buffer for file data transfer instead -
 * the RAM-saving option this project actually needs (see WORKLOG.md/
 * README.md - m_data is already >90% used). Costs nothing here since
 * snapshot.c only ever has one file open at a time, never concurrently
 * reads/writes two files. */
#define FF_FS_TINY 1

#define FF_FS_EXFAT 0 /* Plain FAT16/FAT32 only - exFAT needs LFN, not worth the RAM. */

/* No RTC on this board - every file gets a fixed fake timestamp instead
 * of needing get_fattime() wired to real hardware. */
#define FF_FS_NORTC   1
#define FF_NORTC_MON  1
#define FF_NORTC_MDAY 1
#define FF_NORTC_YEAR 2026

#define FF_FS_NOFSINFO 0
#define FF_FS_LOCK     0 /* No concurrent file access to guard against - single bare-metal loop. */
#define FF_FS_REENTRANT 0 /* No RTOS/multiple contexts touching the filesystem. */
#define FF_FS_TIMEOUT   1000

#endif /* _FFCONF_H_ */
