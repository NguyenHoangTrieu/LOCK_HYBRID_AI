/*
 * card_store.c - see card_store.h.
 *
 * On-disk format (CARDS.DAT, written atomically via CARDS.TMP + f_rename -
 * FR-54): magic(4) + version(u32 LE) + count(u16 LE) + count * entry(36
 * bytes: uidLen(1)+uid[10]+nameLen(1)+name[24]) + crc32(u32 LE, over
 * every byte before it). All little-endian, since this is a local file
 * this MCU alone reads and writes - no wire-protocol endianness
 * convention to match (compare protocol.h's big-endian wire fields).
 */
#include "card_store.h"
#include "ff.h"
#include "fsl_debug_console.h"
#include "sd_spi_disk.h"
#include <string.h>

#define CARD_STORE_ENTRY_SIZE 36U /* 1 + 10 + 1 + 24, matches the on-disk layout above */

#define CARD_STORE_MAGIC "CST1"
#define CARD_STORE_MAGIC_LEN 4U
#define CARD_STORE_HEADER_LEN (CARD_STORE_MAGIC_LEN + 4U + 2U) /* magic + version + count */
#define CARD_STORE_FILE_NAME "CARDS.DAT"
#define CARD_STORE_TMP_NAME  "CARDS.TMP"

static card_store_entry_t s_entries[CARD_STORE_MAX_ENTRIES];
static uint16_t s_count   = 0U;
static uint32_t s_version = 0U; /* 0 = empty/unverified, SR-9 */

/* One shared scratch buffer for the serialized entry body - sized
 * conservatively (CARD_STORE_MAX_ENTRIES is kept well below protocol.h's
 * wire-level cap) because this build's actual free-RAM margin has not
 * been measured, and this chip has a real history of RAM-budget crashes
 * from "just make the buffer bigger" (see WORKLOG.md's dual-core
 * RAM-pressure findings). LoadFromDisk() and WriteAtomic() are never
 * called concurrently (both only run from the single bare-metal main
 * loop, never from an ISR), so sharing one buffer between them is safe. */
static uint8_t s_ioBuf[CARD_STORE_MAX_ENTRIES * CARD_STORE_ENTRY_SIZE];

static void PutU32LE(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t GetU32LE(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void PutU16LE(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t GetU16LE(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void EntryToBytes(const card_store_entry_t *e, uint8_t *out)
{
    out[0] = e->uidLen;
    memcpy(&out[1], e->uid, CARD_STORE_UID_MAX_LEN);
    out[11] = e->nameLen;
    memcpy(&out[12], e->name, CARD_STORE_NAME_MAX_LEN);
}

static void BytesToEntry(const uint8_t *in, card_store_entry_t *e)
{
    e->uidLen = in[0];
    memcpy(e->uid, &in[1], CARD_STORE_UID_MAX_LEN);
    e->nameLen = in[11];
    memcpy(e->name, &in[12], CARD_STORE_NAME_MAX_LEN);
}

/* Standard CRC-32 (polynomial 0xEDB88320, reflected), streamed across
 * multiple calls so the header and body spans never need to be copied
 * into one contiguous buffer just to checksum them - bit-by-bit, no
 * 256-entry table; the card store is at most a few KB and only
 * (re)written on a version change, so the extra cycles don't matter.
 * Pass CRC32_INIT the first call, chain the returned value into the
 * next, and invert the final result (CRC32_FINISH) before comparing. */
#define CRC32_INIT 0xFFFFFFFFU
#define CRC32_FINISH(crc) (~(crc))

static uint32_t Crc32Update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0U; i < len; i++)
    {
        crc ^= data[i];
        for (uint32_t bit = 0U; bit < 8U; bit++)
        {
            crc = (crc & 1U) ? (crc >> 1) ^ 0xEDB88320U : (crc >> 1);
        }
    }
    return crc;
}

/* Loads+verifies CARD_STORE_FILE_NAME into s_entries/s_count/s_version.
 * Any failure (missing file, short read, bad magic, bad CRC, more
 * entries than this build's RAM budget) leaves the store empty
 * (SR-9) - never partially populated. */
static void LoadFromDisk(void)
{
    FIL file;
    if (f_open(&file, CARD_STORE_FILE_NAME, FA_READ) != FR_OK)
    {
        PRINTF("CardStore: no " CARD_STORE_FILE_NAME " on the SD card yet - starting empty (will sync from server).\r\n");
        return;
    }

    uint8_t header[CARD_STORE_HEADER_LEN];
    UINT read = 0U;
    if (f_read(&file, header, sizeof(header), &read) != FR_OK || read != sizeof(header) ||
        memcmp(header, CARD_STORE_MAGIC, CARD_STORE_MAGIC_LEN) != 0)
    {
        PRINTF("CardStore: " CARD_STORE_FILE_NAME " header invalid - treating store as empty (SR-9).\r\n");
        f_close(&file);
        return;
    }
    uint32_t version = GetU32LE(&header[CARD_STORE_MAGIC_LEN]);
    uint16_t count    = GetU16LE(&header[CARD_STORE_MAGIC_LEN + 4U]);
    if (count > CARD_STORE_MAX_ENTRIES)
    {
        PRINTF("CardStore: " CARD_STORE_FILE_NAME " has %u entries, over this build's %u-entry RAM budget - "
               "treating as empty (SR-9) rather than silently truncating.\r\n",
               (unsigned)count, (unsigned)CARD_STORE_MAX_ENTRIES);
        f_close(&file);
        return;
    }

    uint32_t bodyLen = (uint32_t)count * CARD_STORE_ENTRY_SIZE;
    if (f_read(&file, s_ioBuf, bodyLen, &read) != FR_OK || read != bodyLen)
    {
        PRINTF("CardStore: short/failed read on " CARD_STORE_FILE_NAME " body - treating as empty (SR-9).\r\n");
        f_close(&file);
        return;
    }
    uint8_t crcBytes[4];
    if (f_read(&file, crcBytes, 4U, &read) != FR_OK || read != 4U)
    {
        PRINTF("CardStore: could not read " CARD_STORE_FILE_NAME "'s CRC - treating as empty (SR-9).\r\n");
        f_close(&file);
        return;
    }
    f_close(&file);

    uint32_t crc         = Crc32Update(CRC32_INIT, header, sizeof(header));
    crc                  = Crc32Update(crc, s_ioBuf, bodyLen);
    uint32_t expectedCrc = GetU32LE(crcBytes);
    uint32_t actualCrc   = CRC32_FINISH(crc);
    if (actualCrc != expectedCrc)
    {
        PRINTF("CardStore: CRC mismatch on " CARD_STORE_FILE_NAME " (expected 0x%08X, got 0x%08X) - "
               "treating as empty (SR-9).\r\n",
               (unsigned)expectedCrc, (unsigned)actualCrc);
        return;
    }

    for (uint16_t i = 0U; i < count; i++)
    {
        BytesToEntry(&s_ioBuf[(uint32_t)i * CARD_STORE_ENTRY_SIZE], &s_entries[i]);
    }
    s_count   = count;
    s_version = version;
    PRINTF("CardStore: loaded %u card(s), version %u.\r\n", (unsigned)s_count, (unsigned)s_version);
}

void CARD_STORE_Init(void)
{
    if (!SDCARD_DISK_IsReady())
    {
        PRINTF("CardStore: SD card not ready - local card store disabled, every card denied until it is (SR-9).\r\n");
        s_count   = 0U;
        s_version = 0U;
        return;
    }
    LoadFromDisk();
}

bool CARD_STORE_Lookup(const uint8_t *uid, uint8_t uidLen, card_store_lookup_result_t *outResult)
{
    outResult->found   = false;
    outResult->name[0] = '\0';
    for (uint16_t i = 0U; i < s_count; i++)
    {
        if (s_entries[i].uidLen == uidLen && memcmp(s_entries[i].uid, uid, uidLen) == 0)
        {
            outResult->found = true;
            uint8_t len       = s_entries[i].nameLen;
            if (len > CARD_STORE_NAME_MAX_LEN)
            {
                len = CARD_STORE_NAME_MAX_LEN;
            }
            memcpy(outResult->name, s_entries[i].name, len);
            outResult->name[len] = '\0';
            return true;
        }
    }
    return false;
}

uint32_t CARD_STORE_GetVersion(void)
{
    return s_version;
}

bool CARD_STORE_WriteAtomic(uint32_t version, const card_store_entry_t *entries, uint16_t count)
{
    if (count > CARD_STORE_MAX_ENTRIES)
    {
        PRINTF("CardStore: refusing to write %u entries, over this build's %u-entry budget - "
               "previous store left untouched.\r\n",
               (unsigned)count, (unsigned)CARD_STORE_MAX_ENTRIES);
        return false;
    }
    if (!SDCARD_DISK_IsReady())
    {
        PRINTF("CardStore: SD card not ready - cannot sync, previous store (if any) left untouched.\r\n");
        return false;
    }

    for (uint16_t i = 0U; i < count; i++)
    {
        EntryToBytes(&entries[i], &s_ioBuf[(uint32_t)i * CARD_STORE_ENTRY_SIZE]);
    }
    uint32_t bodyLen = (uint32_t)count * CARD_STORE_ENTRY_SIZE;

    uint8_t header[CARD_STORE_HEADER_LEN];
    memcpy(header, CARD_STORE_MAGIC, CARD_STORE_MAGIC_LEN);
    PutU32LE(&header[CARD_STORE_MAGIC_LEN], version);
    PutU16LE(&header[CARD_STORE_MAGIC_LEN + 4U], count);

    uint32_t crc = Crc32Update(CRC32_INIT, header, sizeof(header));
    crc          = Crc32Update(crc, s_ioBuf, bodyLen);
    uint8_t crcBytes[4];
    PutU32LE(crcBytes, CRC32_FINISH(crc));

    /* FR-54: write the complete new store to a temp file first. Nothing
     * touches CARD_STORE_FILE_NAME until the temp file is fully written,
     * so a failure up to this point leaves the previous store (on disk
     * and in RAM) completely untouched. */
    FIL file;
    if (f_open(&file, CARD_STORE_TMP_NAME, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    {
        PRINTF("CardStore: could not create " CARD_STORE_TMP_NAME " - sync aborted, previous store kept.\r\n");
        return false;
    }
    UINT written          = 0U;
    FRESULT headerResult  = f_write(&file, header, sizeof(header), &written);
    bool headerOk         = (headerResult == FR_OK) && (written == sizeof(header));
    FRESULT bodyResult    = FR_OK;
    bool bodyOk           = true;
    if (headerOk && bodyLen > 0U)
    {
        bodyResult = f_write(&file, s_ioBuf, bodyLen, &written);
        bodyOk     = (bodyResult == FR_OK) && (written == bodyLen);
    }
    FRESULT crcResult = FR_OK;
    bool crcOk        = true;
    if (headerOk && bodyOk)
    {
        crcResult = f_write(&file, crcBytes, sizeof(crcBytes), &written);
        crcOk     = (crcResult == FR_OK) && (written == sizeof(crcBytes));
    }
    f_close(&file);

    if (!headerOk || !bodyOk || !crcOk)
    {
        PRINTF("CardStore: write to " CARD_STORE_TMP_NAME " failed (header FRESULT=%d, body FRESULT=%d, "
               "crc FRESULT=%d) - sync aborted, previous store kept.\r\n",
               (int)headerResult, (int)bodyResult, (int)crcResult);
        (void)f_unlink(CARD_STORE_TMP_NAME);
        return false;
    }

    /* Swap: there is a small window between the unlink and the rename
     * where neither file exists - a power loss exactly there loses the
     * store. That degrades to the SR-9 empty-store case (deny every
     * card until the next sync), never an incorrect grant, so it is an
     * accepted tradeoff rather than something this function tries to
     * eliminate (doing so would need two alternating filenames and a
     * pointer file, real complexity for a failure window measured in
     * milliseconds). */
    (void)f_unlink(CARD_STORE_FILE_NAME); /* ignore FR_NO_FILE - fine if this is the very first sync */
    if (f_rename(CARD_STORE_TMP_NAME, CARD_STORE_FILE_NAME) != FR_OK)
    {
        PRINTF("CardStore: rename " CARD_STORE_TMP_NAME " -> " CARD_STORE_FILE_NAME " failed - "
               "store is now EMPTY on disk (SR-9 applies), will retry on next sync.\r\n");
        s_count   = 0U;
        s_version = 0U;
        return false;
    }

    for (uint16_t i = 0U; i < count; i++)
    {
        s_entries[i] = entries[i];
    }
    s_count   = count;
    s_version = version;
    PRINTF("CardStore: synced %u card(s), now at version %u.\r\n", (unsigned)s_count, (unsigned)s_version);
    return true;
}
