/*
 * spi_bridge.c - see spi_bridge.h / protocol.h.
 *
 * Pin assignment below (GPIO6/8/9/10) is read directly from the
 * schematic's symbol pin names for U3 (esp32-c5-devkit-v2:ESP32-C5-DevKit-V2
 * - SPI_SCK/MOSI/MISO/ESP32_CS nets resolve to pins 24/20/22/21, whose
 * symbol pin names are GPIO6/GPIO10/GPIO8/GPIO9), not guessed. The
 * Library README (PCB_DES/LOCK_EDGE_BASEBOARD/Library/README.md) does
 * flag this symbol's physical column spacing/outline as an unmeasured
 * guess, though - if the populated module's actual header doesn't land
 * where the footprint expects, these *logical* GPIO assignments are
 * still right (they come from the schematic, not the footprint), but
 * double-check against the physical board before trusting this wiring.
 *
 * Two tasks:
 *  - the SPI slave transceive loop (tight, must never block on the
 *    network - see protocol.h for why the loop shape is what it is)
 *  - a worker task that does the actual (slow, blocking) backend HTTP
 *    calls and republishes the result into s_txFrame for the SPI task to
 *    start handing back on the next transactions
 * s_txFrame is the only thing shared between them, under s_txMutex.
 */
#include "spi_bridge.h"
#include "protocol.h"

#include "backend_client.h"
#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_sta.h"
#include "esp_timer.h"
#include "soc/gpio_reg.h"
#include <string.h>
#include <time.h>

static const char *TAG = "spi_bridge";

#define SPI_BRIDGE_HOST     SPI2_HOST
#define PIN_SCK  GPIO_NUM_6
#define PIN_MISO GPIO_NUM_8
#define PIN_MOSI GPIO_NUM_10
#define PIN_CS   GPIO_NUM_9

#define FACE_MAX_BYTES (24U * 1024U) /* bounded scratch buffer - see file header, this path is not yet exercised by real hardware */

static SemaphoreHandle_t s_txMutex;
static proto_frame_t s_txFrame; /* next frame to hand the master; default-initialized to all zero (= RSP_IDLE, len 0) which is already correct */

static QueueHandle_t s_requestQueue; /* holds proto_frame_t by value - FreeRTOS queue only ever has at most 1-2 outstanding, see queue_size below */

static uint8_t s_faceBuf[FACE_MAX_BYTES];
static uint32_t s_faceExpectedLen;
static uint32_t s_faceReceivedLen;
static uint32_t s_faceOccurredAt;

static void SetTxFrame(const proto_frame_t *frame)
{
    xSemaphoreTake(s_txMutex, portMAX_DELAY);
    memcpy(&s_txFrame, frame, sizeof(s_txFrame));
    xSemaphoreGive(s_txMutex);
}

static void SetSimpleResponse(uint8_t type)
{
    proto_frame_t frame = {0};
    frame.magic         = PROTO_MAGIC;
    frame.type          = type;
    SetTxFrame(&frame);
}

static void UnixTimeToIso8601(uint32_t unixTime, char *out, size_t outLen)
{
    time_t t = (time_t)unixTime;
    struct tm tmBuf;
    gmtime_r(&t, &tmBuf);
    strftime(out, outLen, "%Y-%m-%dT%H:%M:%SZ", &tmBuf);
}

static void HandleCardRequest(const proto_frame_t *req)
{
    if (!WIFI_IsConnected())
    {
        SetSimpleResponse(PROTO_RSP_WIFI_DOWN);
        return;
    }
    if (req->payload_len <= PROTO_CARD_UID_OFFSET)
    {
        SetSimpleResponse(PROTO_RSP_ERROR);
        return;
    }

    uint32_t occurredAtBE;
    memcpy(&occurredAtBE, req->payload, 4U);
    uint32_t occurredAt = proto_be32(occurredAtBE);
    char isoTime[32];
    UnixTimeToIso8601(occurredAt, isoTime, sizeof(isoTime));

    const uint8_t *uid = req->payload + PROTO_CARD_UID_OFFSET;
    uint8_t uidLen      = (uint8_t)(req->payload_len - PROTO_CARD_UID_OFFSET);

    backend_decision_t decision;
    if (!BACKEND_AttendanceCard(uid, uidLen, isoTime, &decision))
    {
        SetSimpleResponse(PROTO_RSP_ERROR);
        return;
    }

    proto_frame_t resp = {0};
    resp.magic          = PROTO_MAGIC;
    resp.type            = PROTO_RSP_DECISION;
    proto_decision_payload_t *dp = (proto_decision_payload_t *)resp.payload;
    dp->granted                   = decision.granted ? 1U : 0U;
    dp->unlock_duration_sec        = decision.unlock_duration_sec;
    size_t nameLen                  = strnlen(decision.name, sizeof(dp->name));
    dp->name_len                     = (uint8_t)nameLen;
    memcpy(dp->name, decision.name, nameLen);
    resp.payload_len = (uint16_t)(sizeof(proto_decision_payload_t) - sizeof(dp->name) + nameLen);
    SetTxFrame(&resp);
}

static void HandleFaceBegin(const proto_frame_t *req)
{
    if (req->payload_len < 8U)
    {
        SetSimpleResponse(PROTO_RSP_ERROR);
        return;
    }
    uint32_t occurredAtBE, totalLenBE;
    memcpy(&occurredAtBE, req->payload, 4U);
    memcpy(&totalLenBE, req->payload + 4, 4U);
    s_faceOccurredAt  = proto_be32(occurredAtBE);
    s_faceExpectedLen = proto_be32(totalLenBE);
    s_faceReceivedLen = 0U;

    if (s_faceExpectedLen > FACE_MAX_BYTES)
    {
        ESP_LOGW(TAG, "face image (%u bytes) exceeds scratch buffer (%u) - rejecting", (unsigned)s_faceExpectedLen,
                 FACE_MAX_BYTES);
        SetSimpleResponse(PROTO_RSP_ERROR);
        return;
    }
    SetSimpleResponse(PROTO_RSP_BUSY); /* "begin" acknowledged, waiting for chunks */
}

static void HandleFaceChunk(const proto_frame_t *req)
{
    if (s_faceReceivedLen + req->payload_len > s_faceExpectedLen || s_faceReceivedLen + req->payload_len > FACE_MAX_BYTES)
    {
        SetSimpleResponse(PROTO_RSP_ERROR);
        return;
    }
    memcpy(s_faceBuf + s_faceReceivedLen, req->payload, req->payload_len);
    s_faceReceivedLen += req->payload_len;

    if ((req->flags & PROTO_FLAG_MORE) != 0U)
    {
        SetSimpleResponse(PROTO_RSP_BUSY); /* more chunks expected, nothing to send to the backend yet */
        return;
    }

    /* Last chunk - now actually have the full image. */
    if (!WIFI_IsConnected())
    {
        SetSimpleResponse(PROTO_RSP_WIFI_DOWN);
        return;
    }
    char isoTime[32];
    UnixTimeToIso8601(s_faceOccurredAt, isoTime, sizeof(isoTime));

    backend_decision_t decision;
    if (!BACKEND_AttendanceFace(s_faceBuf, s_faceReceivedLen, isoTime, &decision))
    {
        SetSimpleResponse(PROTO_RSP_ERROR);
        return;
    }

    proto_frame_t resp = {0};
    resp.magic          = PROTO_MAGIC;
    resp.type            = PROTO_RSP_DECISION;
    proto_decision_payload_t *dp = (proto_decision_payload_t *)resp.payload;
    dp->granted                   = decision.granted ? 1U : 0U;
    dp->unlock_duration_sec        = decision.unlock_duration_sec;
    size_t nameLen                  = strnlen(decision.name, sizeof(dp->name));
    dp->name_len                     = (uint8_t)nameLen;
    memcpy(dp->name, decision.name, nameLen);
    resp.payload_len = (uint16_t)(sizeof(proto_decision_payload_t) - sizeof(dp->name) + nameLen);
    SetTxFrame(&resp);
}

static void HandleGetConfig(void)
{
    if (!WIFI_IsConnected())
    {
        SetSimpleResponse(PROTO_RSP_WIFI_DOWN);
        return;
    }
    backend_config_t cfg;
    if (!BACKEND_GetConfig(&cfg))
    {
        SetSimpleResponse(PROTO_RSP_ERROR);
        return;
    }
    proto_frame_t resp = {0};
    resp.magic          = PROTO_MAGIC;
    resp.type            = PROTO_RSP_CONFIG;
    proto_config_payload_t *cp = (proto_config_payload_t *)resp.payload;
    cp->recognition_threshold_x1000 = proto_be16((uint16_t)(cfg.recognition_threshold * 1000.0f));
    cp->unlock_duration_sec          = cfg.unlock_duration_sec;
    cp->cooldown_sec                  = proto_be16(cfg.cooldown_sec);
    cp->request_timeout_sec            = cfg.request_timeout_sec;
    cp->offline_card_access_enabled     = cfg.offline_card_access_enabled ? 1U : 0U;
    resp.payload_len = sizeof(proto_config_payload_t);
    SetTxFrame(&resp);
}

static void HandleHeartbeat(const proto_frame_t *req)
{
    if (!WIFI_IsConnected())
    {
        SetSimpleResponse(PROTO_RSP_WIFI_DOWN);
        return;
    }

    uint32_t deviceCardStoreVersion = 0U;
    char fwVersion[17]              = {0}; /* see proto_heartbeat_request_payload_t.fw_version[16] */
    if (req->payload_len >= sizeof(proto_heartbeat_request_payload_t))
    {
        const proto_heartbeat_request_payload_t *hbReq = (const proto_heartbeat_request_payload_t *)req->payload;
        deviceCardStoreVersion                           = proto_be32(hbReq->card_store_version);
        uint8_t fwLen                                      = hbReq->fw_version_len;
        if (fwLen > sizeof(fwVersion) - 1U)
        {
            fwLen = (uint8_t)(sizeof(fwVersion) - 1U);
        }
        memcpy(fwVersion, hbReq->fw_version, fwLen);
    }

    uint32_t serverTime = 0U;
    backend_heartbeat_t hb;
    if (!BACKEND_Heartbeat(deviceCardStoreVersion, fwVersion, &serverTime, &hb))
    {
        SetSimpleResponse(PROTO_RSP_ERROR);
        return;
    }

    proto_frame_t resp = {0};
    resp.magic          = PROTO_MAGIC;
    resp.type            = PROTO_RSP_HEARTBEAT_OK;
    proto_heartbeat_payload_t *hp = (proto_heartbeat_payload_t *)resp.payload;
    hp->server_time                = proto_be32(serverTime);
    hp->mode                        = hb.enrollmentMode ? PROTO_MODE_ENROLLMENT : PROTO_MODE_ATTENDANCE;
    hp->card_store_version           = proto_be32(hb.cardStoreVersion);
    hp->session_active               = hb.sessionActive ? 1U : 0U;
    size_t nameLen                     = strnlen(hb.sessionUserName, sizeof(hp->session_user_name));
    hp->session_user_name_len           = (uint8_t)nameLen;
    memcpy(hp->session_user_name, hb.sessionUserName, nameLen);
    hp->session_expires_in_sec           = proto_be16(hb.sessionExpiresInSec);
    resp.payload_len = sizeof(proto_heartbeat_payload_t);
    SetTxFrame(&resp);
}

/* ESP32 has far more RAM than the MCXN947 (card_store.h's build measured
 * out at a 32-entry cap) - this cache is sized comfortably above that so
 * truncation, if the server ever has more cards than either side can
 * hold, is logged clearly on both sides rather than silently clipped
 * here first. */
#define CARDSTORE_CACHE_MAX_ENTRIES 128U
static backend_card_entry_t s_cardStoreCache[CARDSTORE_CACHE_MAX_ENTRIES];
static uint16_t s_cardStoreCount   = 0U;
static uint32_t s_cardStoreVersion = 0U;

static void HandleCardStoreGet(const proto_frame_t *req)
{
    if (req->payload_len < 2U)
    {
        SetSimpleResponse(PROTO_RSP_ERROR);
        return;
    }
    uint16_t chunkIndexBE;
    memcpy(&chunkIndexBE, req->payload, 2U);
    uint16_t chunkIndex = proto_be16(chunkIndexBE);

    if (chunkIndex == 0U)
    {
        /* First chunk of a fetch - (re)pull the full list from the
         * backend now; later chunks of the SAME fetch are served from
         * this cache without a second HTTP round trip. */
        if (!WIFI_IsConnected())
        {
            SetSimpleResponse(PROTO_RSP_WIFI_DOWN);
            return;
        }
        if (!BACKEND_GetCardStore(&s_cardStoreVersion, s_cardStoreCache, CARDSTORE_CACHE_MAX_ENTRIES, &s_cardStoreCount))
        {
            SetSimpleResponse(PROTO_RSP_ERROR);
            return;
        }
    }

    uint16_t totalChunks =
        (uint16_t)((s_cardStoreCount + PROTO_CARDSTORE_MAX_ENTRIES_PER_CHUNK - 1U) / PROTO_CARDSTORE_MAX_ENTRIES_PER_CHUNK);
    if (totalChunks == 0U)
    {
        totalChunks = 1U; /* report one (empty) chunk so the master's fetch loop terminates cleanly on an empty store */
    }
    if (chunkIndex >= totalChunks)
    {
        SetSimpleResponse(PROTO_RSP_ERROR);
        return;
    }

    uint16_t startIdx = (uint16_t)(chunkIndex * PROTO_CARDSTORE_MAX_ENTRIES_PER_CHUNK);
    uint16_t countInChunk = (uint16_t)(s_cardStoreCount - startIdx);
    if (countInChunk > PROTO_CARDSTORE_MAX_ENTRIES_PER_CHUNK)
    {
        countInChunk = PROTO_CARDSTORE_MAX_ENTRIES_PER_CHUNK;
    }

    proto_frame_t resp = {0};
    resp.magic          = PROTO_MAGIC;
    resp.type            = PROTO_RSP_CARDSTORE_DATA;
    proto_cardstore_payload_t *cs = (proto_cardstore_payload_t *)resp.payload;
    cs->version                   = proto_be32(s_cardStoreVersion);
    cs->total_chunks                = proto_be16(totalChunks);
    cs->chunk_index                  = proto_be16(chunkIndex);
    cs->entry_count                   = (uint8_t)countInChunk;
    for (uint16_t i = 0U; i < countInChunk; i++)
    {
        const backend_card_entry_t *src = &s_cardStoreCache[startIdx + i];
        proto_card_entry_t *dst          = &cs->entries[i];
        dst->uid_len                      = src->uidLen;
        memcpy(dst->uid, src->uid, sizeof(dst->uid));
        dst->active   = 1U; /* see protocol.h's proto_card_entry_t comment - always true in this feed today */
        dst->name_len = src->nameLen;
        memcpy(dst->name, src->name, sizeof(dst->name));
    }
    resp.payload_len = (uint16_t)(9U + (uint32_t)countInChunk * sizeof(proto_card_entry_t));
    SetTxFrame(&resp);
}

static void HandleEnrollCard(const proto_frame_t *req)
{
    if (!WIFI_IsConnected())
    {
        SetSimpleResponse(PROTO_RSP_WIFI_DOWN);
        return;
    }
    if (req->payload_len == 0U || req->payload_len > BACKEND_CARD_UID_MAX_LEN)
    {
        SetSimpleResponse(PROTO_RSP_ERROR);
        return;
    }

    backend_enroll_result_t result;
    char holderName[32];
    if (!BACKEND_SubmitEnrollmentCard(req->payload, (uint8_t)req->payload_len, &result, holderName, sizeof(holderName)))
    {
        SetSimpleResponse(PROTO_RSP_ERROR);
        return;
    }

    uint8_t code;
    switch (result)
    {
        case BACKEND_ENROLL_ASSIGNED:
            code = PROTO_ENROLL_RESULT_ASSIGNED;
            break;
        case BACKEND_ENROLL_ALREADY_REGISTERED:
            code = PROTO_ENROLL_RESULT_ALREADY_REGISTERED;
            break;
        case BACKEND_ENROLL_NEEDS_CONFIRMATION:
            code = PROTO_ENROLL_RESULT_NEEDS_CONFIRMATION;
            break;
        default:
            code = PROTO_ENROLL_RESULT_ERROR;
            break;
    }

    proto_frame_t resp = {0};
    resp.magic          = PROTO_MAGIC;
    resp.type            = PROTO_RSP_ENROLL_RESULT;
    resp.payload[0]       = code;
    uint16_t len            = 1U;
    if (code == PROTO_ENROLL_RESULT_NEEDS_CONFIRMATION)
    {
        size_t maxNameLen = sizeof(holderName) - 1U; /* holderName is the smaller of the two buffers here */
        uint8_t nameLen     = (uint8_t)strnlen(holderName, maxNameLen);
        resp.payload[1]  = nameLen;
        memcpy(&resp.payload[2], holderName, nameLen);
        len = (uint16_t)(2U + nameLen);
    }
    resp.payload_len = len;
    SetTxFrame(&resp);
}

static void WorkerTask(void *arg)
{
    (void)arg;
    proto_frame_t req;
    while (1)
    {
        if (xQueueReceive(s_requestQueue, &req, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }
        switch (req.type)
        {
            case PROTO_CMD_CARD:
                HandleCardRequest(&req);
                break;
            case PROTO_CMD_FACE_BEGIN:
                HandleFaceBegin(&req);
                break;
            case PROTO_CMD_FACE_CHUNK:
                HandleFaceChunk(&req);
                break;
            case PROTO_CMD_GET_CONFIG:
                HandleGetConfig();
                break;
            case PROTO_CMD_HEARTBEAT:
                HandleHeartbeat(&req);
                break;
            case PROTO_CMD_CARDSTORE_GET:
                HandleCardStoreGet(&req);
                break;
            case PROTO_CMD_ENROLL_CARD:
                HandleEnrollCard(&req);
                break;
            default:
                ESP_LOGW(TAG, "unknown request type 0x%02X", req.type);
                break;
        }
    }
}

static void SpiSlaveTask(void *arg)
{
    (void)arg;
    static uint8_t rxBuf[PROTO_FRAME_SIZE] __attribute__((aligned(4)));
    static uint8_t txBuf[PROTO_FRAME_SIZE] __attribute__((aligned(4)));
    uint8_t lastSeqSeen = 0xFFU; /* sentinel - no request has this value as its very first seq in practice (master starts seq at 0, but even if it collided once, duplicate suppression only costs one dropped poll, not a wrong decision) */
    bool haveLastSeq     = false;

    while (1)
    {
        xSemaphoreTake(s_txMutex, portMAX_DELAY);
        memcpy(txBuf, &s_txFrame, sizeof(txBuf));
        xSemaphoreGive(s_txMutex);
        memset(rxBuf, 0, sizeof(rxBuf));

        spi_slave_transaction_t trans = {0};
        trans.length                  = PROTO_FRAME_SIZE * 8U; /* bits */
        trans.tx_buffer                = txBuf;
        trans.rx_buffer                 = rxBuf;

        esp_err_t err = spi_slave_transmit(SPI_BRIDGE_HOST, &trans, portMAX_DELAY);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "spi_slave_transmit: %s", esp_err_to_name(err));
            continue;
        }

        proto_frame_t *rxFrame = (proto_frame_t *)rxBuf;
        if (rxFrame->magic != PROTO_MAGIC || rxFrame->type == PROTO_CMD_POLL)
        {
            continue; /* keepalive/poll or a garbled transaction (see protocol.h) - nothing to act on */
        }
        ESP_LOGI(TAG, "SPI slave request received: type=0x%02X seq=%u", rxFrame->type, rxFrame->seq);
        if (haveLastSeq && rxFrame->seq == lastSeqSeen)
        {
            continue; /* master is still polling for the previous request's result, not sending a new one */
        }
        lastSeqSeen  = rxFrame->seq;
        haveLastSeq  = true;
        rxFrame->payload_len = proto_be16(rxFrame->payload_len); /* header field is wire-BE; convert once, here, so every handler above can just read it as a plain host value */

        if (xQueueSend(s_requestQueue, rxFrame, pdMS_TO_TICKS(100)) != pdTRUE)
        {
            ESP_LOGW(TAG, "worker busy, dropping request type 0x%02X (master will see RSP_BUSY and retry)", rxFrame->type);
        }
        else if (rxFrame->type != PROTO_CMD_FACE_CHUNK) /* a chunk mid-upload should keep showing BUSY, not flip back and forth */
        {
            SetSimpleResponse(PROTO_RSP_BUSY);
        }
    }
}

static void ConfigureSpiSlave(void)
{
    spi_bus_config_t busConfig = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    spi_slave_interface_config_t slaveConfig = {
        .mode      = 0, /* matches the MCXN947 side's shared-bus LPSPI1 config (mode 0, MSB-first) */
        .spics_io_num = PIN_CS,
        .queue_size  = 1, /* one transaction in flight at a time - matches the tight re-queue loop this protocol relies on, see protocol.h */
    };
    ESP_ERROR_CHECK(spi_slave_initialize(SPI_BRIDGE_HOST, &busConfig, &slaveConfig, SPI_DMA_CH_AUTO));
}

void SPI_BRIDGE_Start(void)
{
    s_txMutex = xSemaphoreCreateMutex();
    s_requestQueue = xQueueCreate(8, sizeof(proto_frame_t));

    ConfigureSpiSlave();

    xTaskCreate(WorkerTask, "net_bridge_worker", 6144, NULL, 5, NULL);
    xTaskCreate(SpiSlaveTask, "net_bridge_spi", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "SPI slave bridge started (SCK=%d MOSI=%d MISO=%d CS=%d)", PIN_SCK, PIN_MOSI, PIN_MISO, PIN_CS);
}

