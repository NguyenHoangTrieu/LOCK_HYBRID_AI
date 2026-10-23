/*
 * network_client_stub.c - see network_client.h.
 *
 * Placeholder until the ESP32-C5 transport (CON-2, CI-1..5) exists. Always
 * reports offline / no decision, so every caller already exercises the
 * real "no server" code path (FR-18, Offline state) instead of silently
 * pretending to be online. Replace this file (not network_client.h) when
 * the real transport is built.
 */
#include "network_client.h"
#include "fsl_debug_console.h"

void NET_Init(void)
{
    PRINTF("Network: stub transport (no ESP32-C5 link yet) - device will run Offline-only\r\n");
}

bool NET_IsOnline(void)
{
    return false;
}

uint32_t NET_GetUnixTime(void)
{
    return 0U;
}

void NET_SetPollCallback(net_poll_callback_t cb)
{
    (void)cb;
}

bool NET_RequestDecision(net_method_t method, const uint8_t *payload, uint32_t payloadLen,
                         net_decision_t *outDecision)
{
    (void)method;
    (void)payload;
    (void)payloadLen;
    (void)outDecision;
    return false; /* no transport -> caller's timeout path, see network_client.h */
}

net_mode_t NET_GetMode(void)
{
    return NET_MODE_ATTENDANCE; /* no transport -> never claim enrollment mode */
}

bool NET_GetEnrollmentSession(net_enrollment_session_t *outSession)
{
    (void)outSession;
    return false;
}

uint32_t NET_GetServerCardStoreVersion(void)
{
    return 0U; /* never synced */
}

void NET_ReportCardEvent(const uint8_t *uid, uint8_t uidLen, bool granted)
{
    (void)uid;
    (void)uidLen;
    (void)granted; /* no transport - nothing to report to; FR-20 offline buffering not implemented yet, see WORKLOG.md */
}

bool NET_FetchCardStore(uint32_t *outVersion, net_card_entry_t *outEntries, uint16_t maxEntries, uint16_t *outCount)
{
    (void)outVersion;
    (void)outEntries;
    (void)maxEntries;
    (void)outCount;
    return false;
}

bool NET_SubmitEnrollmentCard(const uint8_t *uid, uint8_t uidLen, net_enroll_result_t *outResult, char *outHolderName,
                              uint32_t outHolderNameSize)
{
    (void)uid;
    (void)uidLen;
    (void)outResult;
    (void)outHolderName;
    (void)outHolderNameSize;
    return false;
}
