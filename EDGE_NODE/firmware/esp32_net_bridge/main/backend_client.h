/*
 * backend_client.h - HTTP(S) client talking to the Cloud Backend
 * (CLOUD_SERVER/, see its README for the exact routes/schemas this
 * mirrors). Base URL and device API key default to the Kconfig.projbuild
 * values (`idf.py menuconfig`), but are overridden at runtime if the
 * WiFi setup captive portal's optional "Backend Server URL"/"Device API
 * Key" fields were filled in (wifi_captive_portal.c writes those to NVS;
 * BACKEND_Init() reads them back) - lets the server address move without
 * a rebuild/reflash, same as the WiFi credentials themselves.
 */
#ifndef _BACKEND_CLIENT_H_
#define _BACKEND_CLIENT_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    bool    granted;
    char    name[32];
    uint8_t unlock_duration_sec;
} backend_decision_t;

typedef struct
{
    float   recognition_threshold;
    uint8_t unlock_duration_sec;
    uint16_t cooldown_sec;
    uint8_t request_timeout_sec;
    bool    offline_card_access_enabled;
} backend_config_t;

/* FR-39/FR-45/FR-52: everything /api/v1/heartbeat's response carries
 * beyond the bare server_time this struct's older sibling used to be -
 * CI-6's "command channel" (see protocol.h's proto_heartbeat_payload_t,
 * which this mirrors on the way back out over SPI). */
typedef struct
{
    bool     enrollmentMode; /* false = attendance */
    bool     sessionActive;  /* only meaningful if enrollmentMode */
    char     sessionUserName[32];
    uint16_t sessionExpiresInSec;
    uint32_t cardStoreVersion;
} backend_heartbeat_t;

#define BACKEND_CARD_UID_MAX_LEN 10U
#define BACKEND_CARD_NAME_MAX_LEN 24U

typedef struct
{
    uint8_t uidLen;
    uint8_t uid[BACKEND_CARD_UID_MAX_LEN];
    uint8_t nameLen;
    char    name[BACKEND_CARD_NAME_MAX_LEN];
} backend_card_entry_t;

typedef enum
{
    BACKEND_ENROLL_ASSIGNED,
    BACKEND_ENROLL_ALREADY_REGISTERED,
    BACKEND_ENROLL_NEEDS_CONFIRMATION,
    BACKEND_ENROLL_ERROR,
} backend_enroll_result_t;

/*! @brief One-time init (esp_http_client config). Call after WiFi is up. */
void BACKEND_Init(void);

/*! @brief POST /api/v1/attendance/card. occurred_at_iso8601 is a
 *  NUL-terminated ISO-8601 timestamp string. Returns false on any
 *  network/parse failure - caller (spi_bridge.c) reports PROTO_RSP_ERROR
 *  or PROTO_RSP_WIFI_DOWN, never fabricates a decision. */
bool BACKEND_AttendanceCard(const uint8_t *uid, uint8_t uidLen, const char *occurred_at_iso8601,
                            backend_decision_t *out);

/*! @brief POST /api/v1/attendance/face (multipart/form-data: image + occurred_at).
 *  Less exercised than the card path - the MCXN947 side does not yet
 *  produce real face-crop JPEGs (see state_machine.c), so this is
 *  reviewed/compiled but not tested against a real image upload end to
 *  end. */
bool BACKEND_AttendanceFace(const uint8_t *jpegBytes, uint32_t jpegLen, const char *occurred_at_iso8601,
                            backend_decision_t *out);

bool BACKEND_GetConfig(backend_config_t *out);

/*! @brief POST /api/v1/heartbeat, reporting this node's own fw_version/
 *  card_store_version (FR-41/FR-54a - `fwVersion` may be NULL/empty if
 *  the MCXN947 side didn't send one). `outServerUnixTime` lets the
 *  MCXN947 side sync its own clock (CI-5) - there is no RTC/SNTP source
 *  on this board otherwise. `out` is filled with the mode/session/
 *  card_store_version fields (CI-6's command channel). */
bool BACKEND_Heartbeat(uint32_t deviceCardStoreVersion, const char *fwVersion, uint32_t *outServerUnixTime,
                       backend_heartbeat_t *out);

/*! @brief GET /api/v1/card_store (FR-51/53). `outEntries` has capacity
 *  `maxEntries` - entries beyond that are dropped (logged), never
 *  overflowed into. */
bool BACKEND_GetCardStore(uint32_t *outVersion, backend_card_entry_t *outEntries, uint16_t maxEntries,
                          uint16_t *outCount);

/*! @brief POST /api/v1/enrollment/card (FR-48). `outHolderName` is
 *  filled only for BACKEND_ENROLL_NEEDS_CONFIRMATION. */
bool BACKEND_SubmitEnrollmentCard(const uint8_t *uid, uint8_t uidLen, backend_enroll_result_t *outResult,
                                  char *outHolderName, size_t outHolderNameSize);

#endif /* _BACKEND_CLIENT_H_ */
