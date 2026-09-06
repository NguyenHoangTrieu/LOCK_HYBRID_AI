/*
 * snapshot.c - see snapshot.h.
 */

#include "snapshot.h"
#include "bbox_overlay.h"
#include "sd_spi_disk.h"
#include "ff.h"
#include "fsl_common.h"
#include "fsl_debug_console.h"
#include <string.h>

/* RGB565 BMP: 14-byte BITMAPFILEHEADER + 40-byte BITMAPINFOHEADER +
 * 12-byte BI_BITFIELDS color masks - built by hand as a flat byte array
 * (not a packed struct) to sidestep struct-packing/alignment questions
 * entirely. Camera frame buffer is already RGB565, already little-endian
 * in memory (Cortex-M33), already stored top-to-bottom - a negative
 * biHeight (top-down) lets the raw frame buffer bytes be written to the
 * file completely unmodified, in one f_write() call, with zero extra RAM
 * for pixel format conversion or row reordering. */
#define BMP_HEADER_SIZE 66U

/* "FACE" + 4-digit index + ".BMP" + NUL = fits FF_USE_LFN=0's 8.3 limit. */
#define SNAPSHOT_NAME_LEN 13U
#define SNAPSHOT_MAX_INDEX 9999U

/* Minimum time between two captures - the hard "never twice in 1 sec"
 * requirement (see snapshot.h). */
#define SNAPSHOT_RATE_LIMIT_MS 1000U

/* How long the LCD's CAPTURE notice line stays lit after a save -
 * deliberately longer than SNAPSHOT_RATE_LIMIT_MS (1s was confirmed too
 * short for a person to notice and react - see WORKLOG.md). A new capture
 * can become possible again before this notice clears; that's fine, it's
 * a human-facing indicator, not a capture-in-progress flag. */
#define SNAPSHOT_NOTICE_DURATION_MS 4000U

static bool s_sdReady        = false;
static bool s_everCaptured   = false;
static uint32_t s_lastCaptureCycle = 0U;
static uint32_t s_nextIndex  = 0U; /* 0 = not yet determined this session, see SNAPSHOT_OpenNextFile(). */

static FATFS s_fs;

static void SNAPSHOT_PutU16LE(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void SNAPSHOT_PutU32LE(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void SNAPSHOT_BuildBmpHeader(uint8_t *hdr, uint16_t width, uint16_t height, uint32_t pixelBytes)
{
    memset(hdr, 0, BMP_HEADER_SIZE);

    /* BITMAPFILEHEADER */
    hdr[0] = 'B';
    hdr[1] = 'M';
    SNAPSHOT_PutU32LE(&hdr[2], BMP_HEADER_SIZE + pixelBytes); /* bfSize */
    SNAPSHOT_PutU32LE(&hdr[10], BMP_HEADER_SIZE);             /* bfOffBits */

    /* BITMAPINFOHEADER */
    SNAPSHOT_PutU32LE(&hdr[14], 40U);                                /* biSize */
    SNAPSHOT_PutU32LE(&hdr[18], (uint32_t)width);                    /* biWidth */
    SNAPSHOT_PutU32LE(&hdr[22], (uint32_t)(-(int32_t)height));       /* biHeight, negative = top-down */
    SNAPSHOT_PutU16LE(&hdr[26], 1U);                                 /* biPlanes */
    SNAPSHOT_PutU16LE(&hdr[28], 16U);                                /* biBitCount */
    SNAPSHOT_PutU32LE(&hdr[30], 3U);                                 /* biCompression = BI_BITFIELDS */
    SNAPSHOT_PutU32LE(&hdr[34], pixelBytes);                         /* biSizeImage */

    /* RGB565 channel masks, required right after the info header when
     * biCompression = BI_BITFIELDS. */
    SNAPSHOT_PutU32LE(&hdr[54], 0xF800U); /* red   */
    SNAPSHOT_PutU32LE(&hdr[58], 0x07E0U); /* green */
    SNAPSHOT_PutU32LE(&hdr[62], 0x001FU); /* blue  */
}

static void SNAPSHOT_FormatName(char *out, uint32_t index)
{
    memcpy(out, "FACE", 4U);
    out[4] = (char)('0' + (index / 1000U) % 10U);
    out[5] = (char)('0' + (index / 100U) % 10U);
    out[6] = (char)('0' + (index / 10U) % 10U);
    out[7] = (char)('0' + index % 10U);
    memcpy(&out[8], ".BMP", 5U); /* includes the NUL terminator */
}

/* First call this session: probe FACE0001.BMP, FACE0002.BMP, ... for the
 * first name that doesn't already exist (so a previous session's snapshots
 * are never overwritten), and remember the winning index. Every later
 * call just opens the next index directly - no more probing needed, since
 * every index from there on is already known to be free. */
static FRESULT SNAPSHOT_OpenNextFile(FIL *file, char *name)
{
    if (s_nextIndex == 0U)
    {
        for (uint32_t i = 1U; i <= SNAPSHOT_MAX_INDEX; i++)
        {
            SNAPSHOT_FormatName(name, i);
            FRESULT fr = f_open(file, name, FA_WRITE | FA_CREATE_NEW);
            if (fr == FR_OK)
            {
                s_nextIndex = i + 1U;
                return FR_OK;
            }
            if (fr != FR_EXIST)
            {
                PRINTF("Snapshot: f_open(%s, CREATE_NEW) failed with FRESULT=%d during first-call probing.\r\n", name,
                       (int)fr);
                return fr; /* real error (no card, full filesystem, ...) - stop probing. */
            }
        }
        return FR_DENIED; /* exhausted FACE0001..FACE9999. */
    }

    if (s_nextIndex > SNAPSHOT_MAX_INDEX)
    {
        return FR_DENIED;
    }
    SNAPSHOT_FormatName(name, s_nextIndex);
    FRESULT fr = f_open(file, name, FA_WRITE | FA_CREATE_NEW);
    if (fr == FR_OK)
    {
        s_nextIndex++;
    }
    return fr;
}

void SNAPSHOT_Init(void)
{
    /* Printed BEFORE the blocking call on purpose: if this is the last line
     * ever seen on a hang, it pinpoints the SD card init itself (SDSPI_Init()
     * via disk_initialize(), source/storage/sd_spi_disk.c) as the stuck
     * step, rather than leaving that ambiguous - see WORKLOG.md's SPI
     * boot-hang entry for why that distinction matters here. */
    PRINTF("Snapshot: initializing SD card (LPSPI1, D10..D13)...\r\n");

    /* f_mount() with opt=1 forces disk_initialize() (sd_spi_disk.c) right
     * now, so a missing/dead card is discovered once at startup instead of
     * silently failing on the first face detection later. */
    s_sdReady = (f_mount(&s_fs, "", 1) == FR_OK) && SDCARD_DISK_IsReady();
    if (!s_sdReady)
    {
        PRINTF("Snapshot: no usable SD card on the shield's slot (D10..D13) - snapshots disabled.\r\n");
    }
    else
    {
        PRINTF("Snapshot: SD card ready.\r\n");
        /* debug_console_lite's PRINTF doesn't reliably support %llu (see
         * WORKLOG.md) - print MB as a plain uint32_t instead, plenty of
         * range for comparing against a card's known real capacity. */
        PRINTF("Snapshot: driver-detected capacity = %u MB - compare against the card's real, known capacity\r\n"
               "  to rule out a CSD/capacity-detection mismatch.\r\n",
               (unsigned)(SDCARD_DISK_GetCapacityBytes() / (1024ULL * 1024ULL)));

        /* Prints free space directly so a future write failure can be
         * checked against "card is just full" instead of guessed at. */
        DWORD freeClusters;
        FATFS *fs;
        if (f_getfree("", &freeClusters, &fs) == FR_OK)
        {
            uint32_t freeBytes = (uint32_t)freeClusters * fs->csize * 512U;
            PRINTF("Snapshot: %u bytes free (%u KB) on the SD card.\r\n", (unsigned)freeBytes,
                   (unsigned)(freeBytes / 1024U));
        }
    }
}

bool SNAPSHOT_OnFrame(uint16_t *frame, uint16_t frameWidth, uint16_t frameHeight, const ai_model_result_t *aiResult,
                      uint16_t aiInputWidth, uint16_t aiInputHeight)
{
    if (!s_sdReady || !aiResult->valid)
    {
        return false;
    }

    bool sawFace = false;
    for (uint32_t i = 0; i < aiResult->boxCount; i++)
    {
        if (strcmp(aiResult->boxes[i].label, "face") == 0)
        {
            sawFace = true;
            break;
        }
    }
    if (!sawFace)
    {
        return false;
    }

    /* Rate limit: at most 1 capture/sec, no exceptions - see snapshot.h.
     * DWT->CYCCNT is a free-running 32-bit counter (enabled by
     * AI_MODEL_Init(), already called before this in main.c); unsigned
     * subtraction makes the comparison correct across its ~26s wraparound
     * at this chip's core clock. */
    uint32_t now = DWT->CYCCNT;
    uint32_t rateLimitCycles = (SystemCoreClock / 1000U) * SNAPSHOT_RATE_LIMIT_MS;
    if (s_everCaptured && ((now - s_lastCaptureCycle) < rateLimitCycles))
    {
        return false;
    }

    float scaleX = (float)frameWidth / (float)aiInputWidth;
    float scaleY = (float)frameHeight / (float)aiInputHeight;
    for (uint32_t i = 0; i < aiResult->boxCount; i++)
    {
        const ai_bbox_t *box = &aiResult->boxes[i];
        if (strcmp(box->label, "face") != 0)
        {
            continue;
        }
        BBOX_DrawRect(frame, frameWidth, frameHeight, (int)((float)box->x * scaleX), (int)((float)box->y * scaleY),
                      (int)((float)box->width * scaleX), (int)((float)box->height * scaleY), 0x07E0U /* green */);
    }

    /* Timed the same way AI_MODEL_RunInference() times itself (DWT cycle
     * counter) - covers file-create through f_close(), since the actual
     * SD write speed was otherwise a total unknown (see WORKLOG.md). */
    uint32_t writeCycStart = DWT->CYCCNT;

    FIL file;
    char name[SNAPSHOT_NAME_LEN];
    FRESULT openResult = SNAPSHOT_OpenNextFile(&file, name);
    if (openResult != FR_OK)
    {
        PRINTF("Snapshot: could not create a new file on the SD card (FRESULT=%d, next index tried=%u).\r\n",
               (int)openResult, (unsigned)s_nextIndex);
        return false;
    }

    uint32_t pixelBytes = (uint32_t)frameWidth * frameHeight * 2U;
    uint8_t header[BMP_HEADER_SIZE];
    SNAPSHOT_BuildBmpHeader(header, frameWidth, frameHeight, pixelBytes);

    UINT headerWritten = 0U;
    UINT pixelsWritten  = 0U;
    FRESULT headerResult = f_write(&file, header, BMP_HEADER_SIZE, &headerWritten);
    FRESULT pixelResult  = FR_OK;
    if (headerResult == FR_OK && headerWritten == BMP_HEADER_SIZE)
    {
        pixelResult = f_write(&file, frame, pixelBytes, &pixelsWritten);
    }
    bool ok = (headerResult == FR_OK) && (headerWritten == BMP_HEADER_SIZE) && (pixelResult == FR_OK) &&
              (pixelsWritten == pixelBytes);
    if (!ok)
    {
        PRINTF("Snapshot: write detail - header FRESULT=%d wrote %u/%u, pixel FRESULT=%d wrote %u/%u.\r\n",
               (int)headerResult, (unsigned)headerWritten, (unsigned)BMP_HEADER_SIZE, (int)pixelResult,
               (unsigned)pixelsWritten, (unsigned)pixelBytes);
    }
    f_close(&file);

    uint32_t writeElapsedUs =
        (uint32_t)(((uint64_t)(DWT->CYCCNT - writeCycStart) * 1000000ULL) / SystemCoreClock);

    if (!ok)
    {
        PRINTF("Snapshot: write failed (%s) after %uus (%ums).\r\n", name, (unsigned)writeElapsedUs,
               (unsigned)(writeElapsedUs / 1000U));
        return false;
    }

    PRINTF("Snapshot: saved %s (write took %uus, %ums)\r\n", name, (unsigned)writeElapsedUs,
           (unsigned)(writeElapsedUs / 1000U));
    s_everCaptured      = true;
    s_lastCaptureCycle  = now;
    return true;
}

bool SNAPSHOT_IsNoticeActive(void)
{
    if (!s_everCaptured)
    {
        return false;
    }
    uint32_t noticeCycles = (SystemCoreClock / 1000U) * SNAPSHOT_NOTICE_DURATION_MS;
    return (DWT->CYCCNT - s_lastCaptureCycle) < noticeCycles;
}
