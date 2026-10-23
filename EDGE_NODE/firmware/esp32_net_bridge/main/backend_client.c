/*
 * backend_client.c - see backend_client.h.
 *
 * Response bodies (decision/config JSON) are small (well under 1KB) -
 * read into one fixed static buffer per call rather than streaming, kept
 * simple on purpose. The face-upload request body is the one place that
 * streams (esp_http_client_write in chunks), since a 10-30KB JPEG
 * shouldn't be duplicated into a second in-RAM buffer on a chip this
 * size.
 */
#include "backend_client.h"

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "backend_client";

#define RESPONSE_BUF_SIZE 1024U
static char s_responseBuf[RESPONSE_BUF_SIZE];
static int s_responseLen;

static esp_err_t HttpEventHandler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA)
    {
        int room = (int)RESPONSE_BUF_SIZE - 1 - s_responseLen;
        if (room > 0)
        {
            int n = (evt->data_len < room) ? evt->data_len : room;
            memcpy(s_responseBuf + s_responseLen, evt->data, (size_t)n);
            s_responseLen += n;
            s_responseBuf[s_responseLen] = '\0';
        }
    }
    return ESP_OK;
}

static esp_http_client_handle_t OpenClient(const char *path, esp_http_client_method_t method)
{
    char url[256];
    (void)snprintf(url, sizeof(url), "%s%s", CONFIG_LOCK_HYBRID_BACKEND_BASE_URL, path);

    /* CONFIG_LOCK_HYBRID_BACKEND_SKIP_TLS_VERIFY is a bool Kconfig option -
     * kconfgen only emits the macro at all when it's set to 'y', so this
     * can't be used as a bare value the way the int/string options above
     * can. */
#ifdef CONFIG_LOCK_HYBRID_BACKEND_SKIP_TLS_VERIFY
    bool skipTlsVerify = true;
#else
    bool skipTlsVerify = false;
#endif

    esp_http_client_config_t config = {
        .url            = url,
        .method         = method,
        .event_handler  = HttpEventHandler,
        .timeout_ms     = CONFIG_LOCK_HYBRID_BACKEND_TIMEOUT_MS,
        .skip_cert_common_name_check = skipTlsVerify,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "X-Device-Key", CONFIG_LOCK_HYBRID_DEVICE_API_KEY);
    return client;
}

void BACKEND_Init(void)
{
    ESP_LOGI(TAG, "backend base URL: %s", CONFIG_LOCK_HYBRID_BACKEND_BASE_URL);
}

static void UidToHex(const uint8_t *uid, uint8_t uidLen, char *out, size_t outSize)
{
    out[0] = '\0';
    for (uint8_t i = 0U; i < uidLen && i < 10U && (size_t)(i * 2 + 2) < outSize; i++)
    {
        (void)snprintf(out + i * 2, 3, "%02X", uid[i]);
    }
}

/* Inverse of UidToHex() - decodes a "DEADBEEF"-style hex string from the
 * server back into raw bytes (FR-51's card_store feed carries UIDs this
 * way, same convention BACKEND_AttendanceCard() already uses outbound).
 * Returns the byte count, 0 on any malformed input (odd length, non-hex
 * character, or longer than outCapacity bytes). */
static uint8_t HexToUid(const char *hex, uint8_t *out, uint8_t outCapacity)
{
    size_t len = strlen(hex);
    if (len == 0U || (len % 2U) != 0U || (len / 2U) > outCapacity)
    {
        return 0U;
    }
    for (size_t i = 0U; i < len; i += 2U)
    {
        unsigned int byte;
        if (sscanf(hex + i, "%2x", &byte) != 1)
        {
            return 0U;
        }
        out[i / 2U] = (uint8_t)byte;
    }
    return (uint8_t)(len / 2U);
}

static bool PerformJsonRequest(const char *path, esp_http_client_method_t method, const char *jsonBody,
                               int expectStatus, cJSON **outJson)
{
    s_responseLen        = 0;
    s_responseBuf[0]     = '\0';
    esp_http_client_handle_t client = OpenClient(path, method);
    if (jsonBody != NULL)
    {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, jsonBody, (int)strlen(jsonBody));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "%s: transport error %s", path, esp_err_to_name(err));
        return false;
    }
    if (status != expectStatus)
    {
        ESP_LOGW(TAG, "%s: HTTP %d: %s", path, status, s_responseBuf);
        return false;
    }

    if (outJson != NULL)
    {
        *outJson = cJSON_Parse(s_responseBuf);
        if (*outJson == NULL)
        {
            ESP_LOGW(TAG, "%s: response was not valid JSON: %s", path, s_responseBuf);
            return false;
        }
    }
    return true;
}

bool BACKEND_AttendanceCard(const uint8_t *uid, uint8_t uidLen, const char *occurred_at_iso8601,
                            backend_decision_t *out)
{
    char uidHex[21] = {0};
    UidToHex(uid, uidLen, uidHex, sizeof(uidHex));

    char body[128];
    (void)snprintf(body, sizeof(body), "{\"card_uid\":\"%s\",\"occurred_at\":\"%s\"}", uidHex, occurred_at_iso8601);

    cJSON *json = NULL;
    if (!PerformJsonRequest("/api/v1/attendance/card", HTTP_METHOD_POST, body, 200, &json))
    {
        return false;
    }

    out->granted = cJSON_IsTrue(cJSON_GetObjectItem(json, "granted"));
    const cJSON *name = cJSON_GetObjectItem(json, "name");
    (void)snprintf(out->name, sizeof(out->name), "%s", cJSON_IsString(name) ? name->valuestring : "");
    out->unlock_duration_sec = (uint8_t)cJSON_GetObjectItem(json, "unlock_duration_sec")->valueint;
    cJSON_Delete(json);
    return true;
}

bool BACKEND_AttendanceFace(const uint8_t *jpegBytes, uint32_t jpegLen, const char *occurred_at_iso8601,
                            backend_decision_t *out)
{
    /* Manual multipart/form-data body: two fields, "occurred_at" (text)
     * and "image" (binary). Content-Length is computed up front so
     * esp_http_client_open() can be used in streaming mode instead of
     * buffering the whole multipart body (the image alone can be
     *10-30KB, see ARCHITECTURE.md Sec.5.2). */
    static const char boundary[] = "----LockHybridBoundary7e1";

    char fieldPart[256];
    int fieldPartLen = snprintf(fieldPart, sizeof(fieldPart),
                                "--%s\r\nContent-Disposition: form-data; name=\"occurred_at\"\r\n\r\n%s\r\n"
                                "--%s\r\nContent-Disposition: form-data; name=\"image\"; filename=\"face.jpg\"\r\n"
                                "Content-Type: image/jpeg\r\n\r\n",
                                boundary, occurred_at_iso8601, boundary);

    char tailPart[64];
    int tailPartLen = snprintf(tailPart, sizeof(tailPart), "\r\n--%s--\r\n", boundary);

    if (fieldPartLen < 0 || tailPartLen < 0)
    {
        return false;
    }
    int totalLen = fieldPartLen + (int)jpegLen + tailPartLen;

    char contentType[64];
    (void)snprintf(contentType, sizeof(contentType), "multipart/form-data; boundary=%s", boundary);

    s_responseLen    = 0;
    s_responseBuf[0] = '\0';
    esp_http_client_handle_t client = OpenClient("/api/v1/attendance/face", HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", contentType);

    bool ok = false;
    if (esp_http_client_open(client, totalLen) == ESP_OK)
    {
        if (esp_http_client_write(client, fieldPart, fieldPartLen) >= 0 &&
            esp_http_client_write(client, (const char *)jpegBytes, (int)jpegLen) >= 0 &&
            esp_http_client_write(client, tailPart, tailPartLen) >= 0)
        {
            esp_http_client_fetch_headers(client);
            int status = esp_http_client_get_status_code(client);
            int readLen;
            while ((readLen = esp_http_client_read(client, s_responseBuf + s_responseLen,
                                                   (int)RESPONSE_BUF_SIZE - 1 - s_responseLen)) > 0)
            {
                s_responseLen += readLen;
            }
            s_responseBuf[s_responseLen] = '\0';
            ok                           = (status == 200);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!ok)
    {
        ESP_LOGW(TAG, "face attendance failed: %s", s_responseBuf);
        return false;
    }

    cJSON *json = cJSON_Parse(s_responseBuf);
    if (json == NULL)
    {
        return false;
    }
    out->granted      = cJSON_IsTrue(cJSON_GetObjectItem(json, "granted"));
    const cJSON *name = cJSON_GetObjectItem(json, "name");
    (void)snprintf(out->name, sizeof(out->name), "%s", cJSON_IsString(name) ? name->valuestring : "");
    out->unlock_duration_sec = (uint8_t)cJSON_GetObjectItem(json, "unlock_duration_sec")->valueint;
    cJSON_Delete(json);
    return true;
}

bool BACKEND_GetConfig(backend_config_t *out)
{
    cJSON *json = NULL;
    if (!PerformJsonRequest("/api/v1/config", HTTP_METHOD_GET, NULL, 200, &json))
    {
        return false;
    }
    out->recognition_threshold       = (float)cJSON_GetObjectItem(json, "recognition_threshold")->valuedouble;
    out->unlock_duration_sec         = (uint8_t)cJSON_GetObjectItem(json, "unlock_duration_sec")->valueint;
    out->cooldown_sec                = (uint16_t)cJSON_GetObjectItem(json, "cooldown_sec")->valueint;
    out->request_timeout_sec         = (uint8_t)cJSON_GetObjectItem(json, "request_timeout_sec")->valueint;
    out->offline_card_access_enabled = cJSON_IsTrue(cJSON_GetObjectItem(json, "offline_card_access_enabled"));
    cJSON_Delete(json);
    return true;
}

bool BACKEND_Heartbeat(uint32_t deviceCardStoreVersion, const char *fwVersion, uint32_t *outServerUnixTime,
                       backend_heartbeat_t *out)
{
    char body[96];
    (void)snprintf(body, sizeof(body), "{\"fw_version\":\"%s\",\"card_store_version\":%u}",
                   (fwVersion != NULL) ? fwVersion : "", (unsigned)deviceCardStoreVersion);

    cJSON *json = NULL;
    if (!PerformJsonRequest("/api/v1/heartbeat", HTTP_METHOD_POST, body, 200, &json))
    {
        return false;
    }

    /* server_time is ISO-8601 (FastAPI's default datetime serialization,
     * e.g. "2026-10-21T09:30:35.190122+00:00") - parse just the parts
     * timegm() needs; microseconds/offset beyond a bare 'Z'/'+00:00' are
     * ignored, which is fine for a once-per-heartbeat clock sync, not for
     * anything needing sub-second accuracy. */
    const cJSON *serverTime = cJSON_GetObjectItem(json, "server_time");
    if (outServerUnixTime != NULL && cJSON_IsString(serverTime))
    {
        struct tm tmBuf = {0};
        if (sscanf(serverTime->valuestring, "%d-%d-%dT%d:%d:%d", &tmBuf.tm_year, &tmBuf.tm_mon, &tmBuf.tm_mday,
                   &tmBuf.tm_hour, &tmBuf.tm_min, &tmBuf.tm_sec) == 6)
        {
            tmBuf.tm_year -= 1900;
            tmBuf.tm_mon -= 1;
            *outServerUnixTime = (uint32_t)timegm(&tmBuf);
        }
    }

    if (out != NULL)
    {
        memset(out, 0, sizeof(*out));
        const cJSON *mode = cJSON_GetObjectItem(json, "mode");
        out->enrollmentMode = cJSON_IsString(mode) && (strcmp(mode->valuestring, "enrollment") == 0);

        const cJSON *cardStoreVersion = cJSON_GetObjectItem(json, "card_store_version");
        if (cJSON_IsNumber(cardStoreVersion))
        {
            out->cardStoreVersion = (uint32_t)cardStoreVersion->valuedouble;
        }

        const cJSON *session = cJSON_GetObjectItem(json, "enrollment_session");
        if (cJSON_IsObject(session))
        {
            out->sessionActive = true;
            const cJSON *userName = cJSON_GetObjectItem(session, "user_name");
            (void)snprintf(out->sessionUserName, sizeof(out->sessionUserName), "%s",
                           cJSON_IsString(userName) ? userName->valuestring : "");
            const cJSON *expires = cJSON_GetObjectItem(session, "expires_in_sec");
            out->sessionExpiresInSec = cJSON_IsNumber(expires) ? (uint16_t)expires->valuedouble : 0U;
        }
    }
    cJSON_Delete(json);
    return true;
}

bool BACKEND_GetCardStore(uint32_t *outVersion, backend_card_entry_t *outEntries, uint16_t maxEntries,
                          uint16_t *outCount)
{
    *outCount = 0U;
    cJSON *json = NULL;
    if (!PerformJsonRequest("/api/v1/card_store", HTTP_METHOD_GET, NULL, 200, &json))
    {
        return false;
    }

    const cJSON *version = cJSON_GetObjectItem(json, "version");
    *outVersion            = cJSON_IsNumber(version) ? (uint32_t)version->valuedouble : 0U;

    const cJSON *cardsArray = cJSON_GetObjectItem(json, "cards");
    if (cJSON_IsArray(cardsArray))
    {
        const cJSON *entry;
        cJSON_ArrayForEach(entry, cardsArray)
        {
            if (*outCount >= maxEntries)
            {
                ESP_LOGW(TAG, "card store has more than %u entries - truncating this fetch", (unsigned)maxEntries);
                break;
            }
            const cJSON *uidField  = cJSON_GetObjectItem(entry, "uid");
            const cJSON *nameField = cJSON_GetObjectItem(entry, "name");
            if (!cJSON_IsString(uidField))
            {
                continue;
            }
            backend_card_entry_t *dst = &outEntries[*outCount];
            dst->uidLen                = HexToUid(uidField->valuestring, dst->uid, BACKEND_CARD_UID_MAX_LEN);
            if (dst->uidLen == 0U)
            {
                ESP_LOGW(TAG, "card store entry has malformed uid '%s' - skipping", uidField->valuestring);
                continue;
            }
            size_t nameLen = cJSON_IsString(nameField) ? strnlen(nameField->valuestring, BACKEND_CARD_NAME_MAX_LEN) : 0U;
            dst->nameLen    = (uint8_t)nameLen;
            memcpy(dst->name, cJSON_IsString(nameField) ? nameField->valuestring : "", nameLen);
            (*outCount)++;
        }
    }
    cJSON_Delete(json);
    return true;
}

bool BACKEND_SubmitEnrollmentCard(const uint8_t *uid, uint8_t uidLen, backend_enroll_result_t *outResult,
                                  char *outHolderName, size_t outHolderNameSize)
{
    if (outHolderNameSize > 0U)
    {
        outHolderName[0] = '\0';
    }
    char uidHex[21] = {0};
    UidToHex(uid, uidLen, uidHex, sizeof(uidHex));

    char body[48];
    (void)snprintf(body, sizeof(body), "{\"card_uid\":\"%s\"}", uidHex);

    cJSON *json = NULL;
    if (!PerformJsonRequest("/api/v1/enrollment/card", HTTP_METHOD_POST, body, 200, &json))
    {
        return false;
    }

    const cJSON *result = cJSON_GetObjectItem(json, "result");
    const char *resultStr = cJSON_IsString(result) ? result->valuestring : "";
    if (strcmp(resultStr, "assigned") == 0)
    {
        *outResult = BACKEND_ENROLL_ASSIGNED;
    }
    else if (strcmp(resultStr, "already_registered") == 0)
    {
        *outResult = BACKEND_ENROLL_ALREADY_REGISTERED;
    }
    else if (strcmp(resultStr, "needs_confirmation") == 0)
    {
        *outResult = BACKEND_ENROLL_NEEDS_CONFIRMATION;
        const cJSON *holder = cJSON_GetObjectItem(json, "holder_name");
        (void)snprintf(outHolderName, outHolderNameSize, "%s", cJSON_IsString(holder) ? holder->valuestring : "");
    }
    else
    {
        *outResult = BACKEND_ENROLL_ERROR;
    }
    cJSON_Delete(json);
    return true;
}
