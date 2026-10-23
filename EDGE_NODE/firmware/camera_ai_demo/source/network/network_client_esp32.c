/*
 * network_client_esp32.c - real ESP32-C5 transport for network_client.h,
 * replacing network_client_stub.c. See protocol.h for the wire format
 * and why it's shaped the way it is (no handshake GPIO on this board -
 * read that file before changing anything here).
 *
 * Like rfid_rc522.c, this shares LPSPI1 with the LCD/SD/RFID/touch
 * devices via spi1_bus.h - own GPIO chip-select (DEMO_ESP32_CS_*,
 * already muxed by BOARD_InitBaseboardPeripheralPins()), reclaim the
 * bus's baud rate before every transfer.
 *
 * NOT verified against a real ESP32-C5 on this board yet - no two
 * physical units to test the link between. Builds clean; the actual
 * wire-level behavior (timing margins around the no-handshake design,
 * whether the chosen baud rate is too fast for the ESP32 slave's own
 * re-queue latency) needs real hardware to confirm. See protocol.h's
 * header comment for the full caveat.
 */
#include "network_client.h"
#include "protocol.h"

#include "app.h"
#include "card_store.h"
#include "fsl_debug_console.h"
#include "fsl_gpio.h"
#include "fsl_lpspi.h"
#include "spi1_bus.h"
#include <string.h>

/* FR-41: reported in every heartbeat (see proto_heartbeat_request_payload_t) -
 * bumped by hand alongside anything in this change that the admin UI
 * (FR-43) should be able to tell apart from an older build. */
#define EDGE_NODE_FW_VERSION "1.0.0-enrollment"

/* Conservative default, same reasoning as RFID_SPI_BAUDRATE_HZ in
 * rfid_rc522.c - untested on real hardware, pick something comfortably
 * under any plausible ceiling rather than the bus's max. */
#define ESP32_SPI_BAUDRATE_HZ 200000U

/* How long between polls once a request is outstanding - see protocol.h
 * point 2 (the slave needs slack to re-queue its next transaction
 * between polls, with no handshake line to signal readiness). */
#define POLL_INTERVAL_MS 30U

/* Overall wall-clock budget for one NET_RequestDecision() call, covering
 * however many polls it takes to see a final answer. Deliberately a bit
 * above PR-1's 2s device-to-unlock target, not equal to it - this is a
 * backstop against a wedged link, not the latency budget itself (that's
 * the server's own request_timeout_sec from MR-1, fetched separately by
 * NET_RequestDecision's caller via a GET_CONFIG request before this is
 * ever the bottleneck in practice). */
#define REQUEST_TIMEOUT_MS 5000U
#define HEARTBEAT_TIMEOUT_MS 4000U

static uint8_t s_nextSeq = 0U;

/* Device clock (CI-5): no RTC/SNTP on this board - synced from the
 * backend's own clock via NET_Heartbeat()'s PROTO_RSP_HEARTBEAT_OK
 * payload (see protocol.h) and tracked relative to DWT->CYCCNT from
 * there. Reads as 0 (Unix epoch) until the first successful heartbeat -
 * callers sending a timestamp before that point are sending a
 * recognizably-wrong one, not a silently-plausible-but-fake one. */
static uint32_t s_timeBaseUnix  = 0U;
static uint32_t s_timeBaseCycle = 0U;
static bool     s_timeSynced    = false;

/* FR-39/FR-45/FR-52: cached from the most recent successful NET_IsOnline()
 * heartbeat (see protocol.h's proto_heartbeat_payload_t) - CI-6's
 * "command channel" is just this cache, refreshed every heartbeat. */
static net_mode_t s_mode                      = NET_MODE_ATTENDANCE;
static net_enrollment_session_t s_session      = {0};
static uint32_t s_serverCardStoreVersion       = 0U;

/* Overall budget for a card-store sync or an enrollment submission -
 * longer than REQUEST_TIMEOUT_MS because the first chunk of a card-store
 * fetch can include a slow server-side query, not just an SPI round trip. */
#define CARDSTORE_TIMEOUT_MS 10000U
#define ENROLL_TIMEOUT_MS    5000U

static uint32_t GetUnixTime(void)
{
    if (!s_timeSynced)
    {
        return 0U;
    }
    uint32_t elapsedCycles = DWT->CYCCNT - s_timeBaseCycle;
    return s_timeBaseUnix + (elapsedCycles / SystemCoreClock);
}

uint32_t NET_GetUnixTime(void)
{
    return GetUnixTime();
}

static void EspCsLow(void)
{
    GPIO_PinWrite(DEMO_ESP32_CS_GPIO, DEMO_ESP32_CS_PIN, 0U);
    SDK_DelayAtLeastUs(50U, SDK_DEVICE_MAXIMUM_CPU_CLOCK_FREQUENCY);
}

static void EspCsHigh(void)
{
    SDK_DelayAtLeastUs(20U, SDK_DEVICE_MAXIMUM_CPU_CLOCK_FREQUENCY);
    GPIO_PinWrite(DEMO_ESP32_CS_GPIO, DEMO_ESP32_CS_PIN, 1U);
    SDK_DelayAtLeastUs(50U, SDK_DEVICE_MAXIMUM_CPU_CLOCK_FREQUENCY);
}

/* One full-duplex PROTO_FRAME_SIZE-byte transaction. `tx`/`rx` must each
 * be PROTO_FRAME_SIZE bytes (may be the same pointer only if the caller
 * doesn't need the old tx contents afterwards - they aren't the same
 * pointer anywhere in this file). */
static void Transceive(const proto_frame_t *tx, proto_frame_t *rx)
{
    (void)SPI1_BUS_SetBaudRate(ESP32_SPI_BAUDRATE_HZ);
    EspCsLow();
    (void)SPI1_BUS_TransferBlocking((const uint8_t *)tx, (uint8_t *)rx, PROTO_FRAME_SIZE, kLPSPI_MasterPcs1 | kLPSPI_MasterPcsContinuous);
    EspCsHigh();

    if (rx->magic == PROTO_MAGIC)
    {
        rx->payload_len = proto_be16(rx->payload_len); /* wire format is BE - see spi_bridge.c's matching conversion on the ESP32 side */
    }
}

static net_poll_callback_t s_pollCb = NULL;

void NET_SetPollCallback(net_poll_callback_t cb)
{
    s_pollCb = cb;
}

/* Sends `req` once, then polls (CMD_POLL frames) until a response other
 * than RSP_BUSY/RSP_IDLE/a garbled frame arrives, or REQUEST_TIMEOUT_MS
 * elapses. Returns false on timeout - callers must treat that as "no
 * decision", never fabricate one (see network_client.h). */
static bool SendAndWait(proto_frame_t *req, proto_frame_t *resp, uint32_t timeoutMs)
{
    req->magic = PROTO_MAGIC;
    req->seq   = s_nextSeq++;
    req->payload_len = proto_be16(req->payload_len);

    proto_frame_t rx;
    Transceive(req, &rx);

    proto_frame_t poll = {0};
    poll.magic          = PROTO_MAGIC;
    poll.type            = PROTO_CMD_POLL;

    uint32_t deadlineCycle = DWT->CYCCNT + (SystemCoreClock / 1000U) * timeoutMs;
    uint32_t pollCount = 0U;

    while ((int32_t)(DWT->CYCCNT - deadlineCycle) < 0)
    {
        SDK_DelayAtLeastUs(POLL_INTERVAL_MS * 1000U, SDK_DEVICE_MAXIMUM_CPU_CLOCK_FREQUENCY);
        pollCount++;
        if (s_pollCb != NULL && (pollCount % 3U) == 0U)
        {
            s_pollCb();
        }

        Transceive(&poll, &rx);

        if (rx.magic != PROTO_MAGIC || rx.type == PROTO_RSP_IDLE || rx.type == PROTO_RSP_BUSY)
        {
            continue; /* not garbled-and-ignorable vs. "still working" - either way, keep polling */
        }
        memcpy(resp, &rx, sizeof(rx));
        return true;
    }

    PRINTF("NET: timed out waiting for a response (type=0x%02X, last rx magic=0x%04X type=0x%02X seq=%u)\r\n", req->type, rx.magic, rx.type, rx.seq);
    return false;
}

void NET_Init(void)
{
    SPI1_BUS_Init();
    PRINTF("NET: ESP32-C5 SPI transport (see protocol.h) - not yet confirmed on real hardware\r\n");
}

#define HEARTBEAT_INTERVAL_MS 5000U
static uint32_t s_lastHeartbeatCycle = 0U;
static bool     s_cachedOnline       = false;
static bool     s_hasRunHeartbeat    = false;
static uint8_t  s_consecutiveFailures = 0U;
#define MAX_OFFLINE_FAILURES 3U

bool NET_IsOnline(void)
{
    uint32_t now = DWT->CYCCNT;
    if (s_hasRunHeartbeat && ((uint32_t)(now - s_lastHeartbeatCycle) < (SystemCoreClock / 1000U) * HEARTBEAT_INTERVAL_MS))
    {
        return s_cachedOnline;
    }
    s_lastHeartbeatCycle = now;
    s_hasRunHeartbeat    = true;

    proto_frame_t req = {0};
    req.type            = PROTO_CMD_HEARTBEAT;
    proto_heartbeat_request_payload_t *hbReq = (proto_heartbeat_request_payload_t *)req.payload;
    hbReq->card_store_version = proto_be32(CARD_STORE_GetVersion());
    /* Clamped, not just asserted: EDGE_NODE_FW_VERSION is a compile-time
     * literal today, but a future edit that makes it longer than
     * fw_version[]'s capacity must not become a silent out-of-bounds
     * memcpy(). */
    uint8_t fwVersionLen = (uint8_t)(sizeof(EDGE_NODE_FW_VERSION) - 1U);
    if (fwVersionLen > sizeof(hbReq->fw_version))
    {
        fwVersionLen = (uint8_t)sizeof(hbReq->fw_version);
    }
    hbReq->fw_version_len = fwVersionLen;
    memcpy(hbReq->fw_version, EDGE_NODE_FW_VERSION, fwVersionLen);
    req.payload_len = sizeof(proto_heartbeat_request_payload_t);
    proto_frame_t resp;
    if (!SendAndWait(&req, &resp, HEARTBEAT_TIMEOUT_MS) || (resp.type != PROTO_RSP_HEARTBEAT_OK))
    {
        s_consecutiveFailures++;
        if (s_consecutiveFailures >= MAX_OFFLINE_FAILURES)
        {
            s_cachedOnline = false;
        }
        return s_cachedOnline;
    }

    s_consecutiveFailures = 0U;
    s_cachedOnline        = true;

    if (resp.payload_len < sizeof(proto_heartbeat_payload_t))
    {
        PRINTF("NET: heartbeat response too short (%u bytes) for proto_heartbeat_payload_t - treating as online "
               "but NOT updating mode/session/card_store_version this round.\r\n",
               (unsigned)resp.payload_len);
        return true; /* still a real link */
    }

    const proto_heartbeat_payload_t *hb = (const proto_heartbeat_payload_t *)resp.payload;
    uint32_t serverTimeBE                = hb->server_time;
    s_timeBaseUnix                        = proto_be32(serverTimeBE);
    s_timeBaseCycle                       = DWT->CYCCNT;
    s_timeSynced                          = true;

    s_mode                  = (hb->mode == PROTO_MODE_ENROLLMENT) ? NET_MODE_ENROLLMENT : NET_MODE_ATTENDANCE;
    s_serverCardStoreVersion = proto_be32(hb->card_store_version);

    s_session.active = (s_mode == NET_MODE_ENROLLMENT) && (hb->session_active != 0U);
    if (s_session.active)
    {
        uint8_t nameLen = hb->session_user_name_len;
        if (nameLen >= sizeof(s_session.userName))
        {
            nameLen = (uint8_t)(sizeof(s_session.userName) - 1U);
        }
        memcpy(s_session.userName, hb->session_user_name, nameLen);
        s_session.userName[nameLen] = '\0';
        s_session.expiresInSec       = proto_be16(hb->session_expires_in_sec);
    }
    s_cachedOnline = true;
    return true;
}

net_mode_t NET_GetMode(void)
{
    return s_mode;
}

bool NET_GetEnrollmentSession(net_enrollment_session_t *outSession)
{
    if (s_mode != NET_MODE_ENROLLMENT)
    {
        return false;
    }
    *outSession = s_session;
    return true;
}

uint32_t NET_GetServerCardStoreVersion(void)
{
    return s_serverCardStoreVersion;
}

void NET_ReportCardEvent(const uint8_t *uid, uint8_t uidLen, bool granted)
{
    /* FR-10b: deliberately reuses the unchanged PROTO_CMD_CARD wire
     * format (ESP32 side still relays it to POST /api/v1/attendance/card
     * for the server-side record) - the difference from the old
     * FR-10-era flow is purely at the CALLER: state_machine.c has
     * already acted on a LOCAL decision (FR-10a) by the time this runs,
     * so whatever this returns is ignored here, not awaited. */
    (void)granted;
    if (uidLen > PROTO_PAYLOAD_SIZE - PROTO_CARD_UID_OFFSET)
    {
        return;
    }
    proto_frame_t req = {0};
    req.type            = PROTO_CMD_CARD;
    uint32_t timeBE      = proto_be32(GetUnixTime());
    memcpy(req.payload, &timeBE, 4U);
    memcpy(req.payload + PROTO_CARD_UID_OFFSET, uid, uidLen);
    req.payload_len = (uint16_t)(PROTO_CARD_UID_OFFSET + uidLen);

    proto_frame_t resp;
    if (!SendAndWait(&req, &resp, REQUEST_TIMEOUT_MS))
    {
        PRINTF("NET: card event report timed out (FR-20 offline buffering not implemented - event is lost, "
               "see WORKLOG.md)\r\n");
    }
}

bool NET_FetchCardStore(uint32_t *outVersion, net_card_entry_t *outEntries, uint16_t maxEntries, uint16_t *outCount)
{
    *outCount = 0U;
    uint16_t chunkIndex  = 0U;
    uint16_t totalChunks = 1U; /* updated from the first response */

    while (chunkIndex < totalChunks)
    {
        proto_frame_t req = {0};
        req.type            = PROTO_CMD_CARDSTORE_GET;
        uint16_t chunkBE      = proto_be16(chunkIndex);
        memcpy(req.payload, &chunkBE, 2U);
        req.payload_len = 2U;

        proto_frame_t resp;
        if (!SendAndWait(&req, &resp, CARDSTORE_TIMEOUT_MS) || resp.type != PROTO_RSP_CARDSTORE_DATA ||
            resp.payload_len < 9U) /* 9-byte header (version+total_chunks+chunk_index+entry_count), see protocol.h */
        {
            PRINTF("NET: card store fetch failed at chunk %u/%u\r\n", (unsigned)chunkIndex, (unsigned)totalChunks);
            return false;
        }

        const proto_cardstore_payload_t *cs = (const proto_cardstore_payload_t *)resp.payload;
        *outVersion                          = proto_be32(cs->version);
        totalChunks                          = proto_be16(cs->total_chunks);
        if (totalChunks > PROTO_CARDSTORE_MAX_CHUNKS || totalChunks == 0U)
        {
            PRINTF("NET: card store reports %u chunks, outside the sane 1..%u range - aborting fetch\r\n",
                   (unsigned)totalChunks, (unsigned)PROTO_CARDSTORE_MAX_CHUNKS);
            return false;
        }

        for (uint8_t i = 0U; i < cs->entry_count; i++)
        {
            if (*outCount >= maxEntries)
            {
                PRINTF("NET: card store has more entries than this build's %u-entry budget - truncating "
                       "(the entries past this point will be missing locally until that budget is raised)\r\n",
                       (unsigned)maxEntries);
                return true; /* what was collected so far is still a valid (if incomplete) store */
            }
            const proto_card_entry_t *src = &cs->entries[i];
            net_card_entry_t *dst          = &outEntries[*outCount];
            dst->uidLen                     = src->uid_len;
            memcpy(dst->uid, src->uid, sizeof(dst->uid));
            dst->nameLen = src->name_len;
            memcpy(dst->name, src->name, sizeof(dst->name));
            (*outCount)++;
        }
        chunkIndex++;
    }
    return true;
}

bool NET_SubmitEnrollmentCard(const uint8_t *uid, uint8_t uidLen, net_enroll_result_t *outResult, char *outHolderName,
                              uint32_t outHolderNameSize)
{
    if (outHolderNameSize > 0U)
    {
        outHolderName[0] = '\0';
    }
    if (uidLen > PROTO_PAYLOAD_SIZE)
    {
        return false;
    }
    proto_frame_t req = {0};
    req.type            = PROTO_CMD_ENROLL_CARD;
    memcpy(req.payload, uid, uidLen);
    req.payload_len = uidLen;

    proto_frame_t resp;
    if (!SendAndWait(&req, &resp, ENROLL_TIMEOUT_MS) || resp.type != PROTO_RSP_ENROLL_RESULT || resp.payload_len < 1U)
    {
        return false;
    }

    uint8_t code = resp.payload[0];
    switch (code)
    {
        case PROTO_ENROLL_RESULT_ASSIGNED:
            *outResult = NET_ENROLL_RESULT_ASSIGNED;
            break;
        case PROTO_ENROLL_RESULT_ALREADY_REGISTERED:
            *outResult = NET_ENROLL_RESULT_ALREADY_REGISTERED;
            break;
        case PROTO_ENROLL_RESULT_NEEDS_CONFIRMATION:
            *outResult = NET_ENROLL_RESULT_NEEDS_CONFIRMATION;
            if (resp.payload_len >= 2U)
            {
                uint8_t nameLen = resp.payload[1];
                uint32_t avail   = (outHolderNameSize > 0U) ? (outHolderNameSize - 1U) : 0U;
                if (nameLen > avail)
                {
                    nameLen = (uint8_t)avail;
                }
                if (nameLen > 0U && resp.payload_len >= (uint16_t)(2U + nameLen))
                {
                    memcpy(outHolderName, &resp.payload[2], nameLen);
                    outHolderName[nameLen] = '\0';
                }
            }
            break;
        default:
            *outResult = NET_ENROLL_RESULT_ERROR;
            break;
    }
    return true;
}

bool NET_RequestDecision(net_method_t method, const uint8_t *payload, uint32_t payloadLen, net_decision_t *outDecision)
{
    proto_frame_t resp;

    if (method == NET_METHOD_CARD)
    {
        if (payloadLen > PROTO_PAYLOAD_SIZE - PROTO_CARD_UID_OFFSET)
        {
            return false; /* not a real UID length (max 10 bytes per rfid_rc522.h) - refuse rather than truncate */
        }
        proto_frame_t req = {0};
        req.type            = PROTO_CMD_CARD;
        uint32_t timeBE      = proto_be32(GetUnixTime());
        memcpy(req.payload, &timeBE, 4U);
        memcpy(req.payload + PROTO_CARD_UID_OFFSET, payload, payloadLen);
        req.payload_len = (uint16_t)(PROTO_CARD_UID_OFFSET + payloadLen);

        if (!SendAndWait(&req, &resp, REQUEST_TIMEOUT_MS))
        {
            return false;
        }
    }
    else /* NET_METHOD_FACE */
    {
        /* Chunked upload - see protocol.h. Not exercised end to end yet:
         * state_machine.c currently calls NET_RequestDecision() with
         * payload=NULL/payloadLen=0 for faces (no JPEG encoder wired up
         * on the device side - see README.md's open items), so this
         * path compiles and is reviewed but has nothing real to upload
         * to test against today. */
        proto_frame_t begin = {0};
        begin.type            = PROTO_CMD_FACE_BEGIN;
        uint32_t timeBE        = proto_be32(GetUnixTime());
        uint32_t lenBE          = proto_be32(payloadLen);
        memcpy(begin.payload, &timeBE, 4U);
        memcpy(begin.payload + 4, &lenBE, 4U);
        begin.payload_len = 8U;
        if (!SendAndWait(&begin, &resp, REQUEST_TIMEOUT_MS) || resp.type != PROTO_RSP_BUSY)
        {
            return false;
        }

        uint32_t sent = 0U;
        while (sent < payloadLen)
        {
            uint32_t chunkLen = payloadLen - sent;
            if (chunkLen > PROTO_PAYLOAD_SIZE)
            {
                chunkLen = PROTO_PAYLOAD_SIZE;
            }
            proto_frame_t chunk = {0};
            chunk.type            = PROTO_CMD_FACE_CHUNK;
            chunk.flags            = ((sent + chunkLen) < payloadLen) ? PROTO_FLAG_MORE : 0U;
            memcpy(chunk.payload, payload + sent, chunkLen);
            chunk.payload_len = (uint16_t)chunkLen;
            sent += chunkLen;

            bool isLast = (sent >= payloadLen);
            if (!SendAndWait(&chunk, &resp, REQUEST_TIMEOUT_MS))
            {
                return false;
            }
            if (!isLast && resp.type != PROTO_RSP_BUSY)
            {
                return false; /* ESP32 rejected mid-upload (e.g. size mismatch) - see spi_bridge.c's HandleFaceChunk() */
            }
        }
        /* After the last chunk, `resp` already holds whatever the final
         * SendAndWait() call returned (RSP_DECISION on success). */
    }

    if (resp.type != PROTO_RSP_DECISION)
    {
        return false; /* RSP_ERROR, RSP_WIFI_DOWN, or anything else - no decision, caller treats as denied (FR-18) */
    }

    const proto_decision_payload_t *dp = (const proto_decision_payload_t *)resp.payload;
    outDecision->granted           = (dp->granted != 0U);
    outDecision->unlockDurationSec = dp->unlock_duration_sec;
    uint8_t nameLen                 = dp->name_len;
    if (nameLen >= sizeof(outDecision->name))
    {
        nameLen = (uint8_t)(sizeof(outDecision->name) - 1U);
    }
    memcpy(outDecision->name, dp->name, nameLen);
    outDecision->name[nameLen] = '\0';
    return true;
}
