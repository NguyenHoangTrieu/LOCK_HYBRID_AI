/*
 * network_client.h - device<->Cloud Backend interface (CI-1..CI-5, FR-3..5,
 * FR-9..11).
 *
 * This header is the seam between the state machine and whatever actually
 * talks to the ESP32-C5 (SPI AT-command link, a raw socket API, etc. - not
 * decided/implemented yet, see the project task list). Today only a stub
 * implementation exists (network_client_stub.c) that always reports "no
 * response" so NET_RequestDecision() correctly resolves through the same
 * FR-18 timeout path a real dead network would - NOT a fake grant/deny.
 * Swapping in the real ESP32 transport later only means adding a new .c
 * file behind this same header; state_machine.c does not need to change.
 */
#ifndef _NETWORK_CLIENT_H_
#define _NETWORK_CLIENT_H_

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    NET_METHOD_FACE,
    NET_METHOD_CARD,
} net_method_t;

typedef struct
{
    bool    granted;
    char    name[32]; /* empty if not granted or name unknown */
    uint8_t unlockDurationSec;
} net_decision_t;

/* FR-39/CI-4: the node's current mode, as last reported by the server
 * (see NET_IsOnline()'s doc below - there is no separate call for this,
 * it rides along on the same heartbeat). */
typedef enum
{
    NET_MODE_ATTENDANCE,
    NET_MODE_ENROLLMENT,
} net_mode_t;

/* FR-45/UI-17/UI-20: the enrollment session active on this node, if any -
 * only meaningful when NET_GetMode() == NET_MODE_ENROLLMENT. */
typedef struct
{
    bool     active;
    char     userName[32];
    uint16_t expiresInSec;
} net_enrollment_session_t;

/* FR-48: result of submitting a card UID during a card-registration
 * session. */
typedef enum
{
    NET_ENROLL_RESULT_ASSIGNED,
    NET_ENROLL_RESULT_ALREADY_REGISTERED,
    NET_ENROLL_RESULT_NEEDS_CONFIRMATION, /* UI-21: holderName filled, admin must resolve on the web app (UI-25) */
    NET_ENROLL_RESULT_ERROR,
} net_enroll_result_t;

/* FR-51: one entry of the local card store, as synced from the server. */
typedef struct
{
    uint8_t uidLen;
    uint8_t uid[10];
    char    name[24];
    uint8_t nameLen;
} net_card_entry_t;

/*! @brief One-time bring-up (ESP32 reset/join/etc, once that's defined).
 *  The stub version is a no-op. */
void NET_Init(void);

/*! @brief True once the device has a usable link to the Cloud Backend.
 *  Every call that reaches the server (this one included) refreshes the
 *  cached mode/enrollment-session/card-store-version state that
 *  NET_GetMode()/NET_GetEnrollmentSession()/NET_GetServerCardStoreVersion()
 *  read back (CI-6: the heartbeat doubles as the command channel - see
 *  protocol.h's proto_heartbeat_payload_t). The stub version always
 *  returns false (CON-2/CI-1..5 not implemented yet) so the state
 *  machine's Idle->Offline transition (ARCHITECTURE.md Sec.2.4) is
 *  exercised honestly rather than masked. */
bool NET_IsOnline(void);

/*! @brief Returns current synchronized Unix timestamp (CI-5), or 0 if not yet synced. */
uint32_t NET_GetUnixTime(void);

typedef void (*net_poll_callback_t)(void);

/*! @brief Register a callback invoked periodically during SendAndWait() polling (for UI spinner animation). */
void NET_SetPollCallback(net_poll_callback_t cb);

/*! @brief FR-3: send a face crop and block (up to an internal timeout)
 *  for the server's decision. Returns true only if a decision was
 *  actually received - on false, `outDecision` is untouched and the
 *  caller must treat the attempt as denied (FR-18), not retry silently. */
bool NET_RequestDecision(net_method_t method, const uint8_t *payload, uint32_t payloadLen,
                         net_decision_t *outDecision);

/*! @brief FR-39/CI-4: mode as of the most recent successful NET_IsOnline()
 *  call. Defaults to NET_MODE_ATTENDANCE before the first successful
 *  heartbeat or whenever offline - a node that can't reach the server
 *  keeps doing whatever it was already doing (fail-safe, not fail-open:
 *  Attendance mode never unlocks without a decision either way). */
net_mode_t NET_GetMode(void);

/*! @brief FR-45: fills `outSession` from the most recent heartbeat.
 *  Returns false (outSession untouched) if the last known mode isn't
 *  NET_MODE_ENROLLMENT - callers should check NET_GetMode() first if
 *  that distinction matters, this is just a convenience guard. */
bool NET_GetEnrollmentSession(net_enrollment_session_t *outSession);

/*! @brief FR-52: the server's current target card_store_version, as of
 *  the most recent successful heartbeat. 0 if never synced. Compare
 *  against CARD_STORE_GetVersion() to decide whether NET_FetchCardStore()
 *  is needed. */
uint32_t NET_GetServerCardStoreVersion(void);

/*! @brief FR-10b: best-effort notification of a card decision the node
 *  already made locally (FR-10a) - called AFTER the actuator has
 *  already acted, never before, so this call's latency (up to an
 *  internal timeout) cannot delay PR-2's 0.5s unlock. Timestamp is
 *  taken internally (CI-5's synced clock), same convention as
 *  NET_RequestDecision(). No return value: a failure here means the
 *  event isn't visible on the server/admin UI until the next successful
 *  report, not that the door's decision was wrong - FR-20's offline
 *  event buffering (replay once reconnected) is NOT implemented yet,
 *  see WORKLOG.md. */
void NET_ReportCardEvent(const uint8_t *uid, uint8_t uidLen, bool granted);

/*! @brief FR-51/53: fetch the full server-side card list into
 *  `outEntries` (capacity `maxEntries` - pass CARD_STORE_MAX_ENTRIES,
 *  never protocol.h's larger wire-level cap, so a server with more
 *  cards than this build's RAM budget is truncated loudly rather than
 *  overflowing the caller's array). Returns false on any transport
 *  failure, in which case the caller must leave its existing store
 *  untouched (SR-9/FR-54) - never partially apply `outEntries`. */
bool NET_FetchCardStore(uint32_t *outVersion, net_card_entry_t *outEntries, uint16_t maxEntries, uint16_t *outCount);

/*! @brief FR-48: submit a card UID read during an active card-
 *  registration session. Returns false only on a transport failure
 *  (caller shows a generic error, UI-21); a real server-side outcome
 *  (including "needs confirmation") always returns true with
 *  `outResult` set. `outHolderName` is filled (NUL-terminated) only for
 *  NET_ENROLL_RESULT_NEEDS_CONFIRMATION. */
bool NET_SubmitEnrollmentCard(const uint8_t *uid, uint8_t uidLen, net_enroll_result_t *outResult, char *outHolderName,
                              uint32_t outHolderNameSize);

#endif /* _NETWORK_CLIENT_H_ */
