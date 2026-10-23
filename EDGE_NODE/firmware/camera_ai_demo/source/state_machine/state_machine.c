/*
 * state_machine.c - see state_machine.h.
 */
#include "state_machine.h"
#include "actuator.h"
#include "app.h"
#include "card_store.h"
#include "fsl_debug_console.h"
#include "network_client.h"
#include "ui_attendance.h"
#include <stdio.h>
#include <string.h>

/* FR-2: a face must be present for this many consecutive frames before an
 * identification attempt starts - the "3 frames" ARCHITECTURE.md Sec.4.2
 * calls out, not just a single lucky detection on someone passing by. */
#define STATE_MACHINE_FACE_STABLE_FRAMES 3U

/* FR-16: how long the latch stays unlocked on a grant (snappy 1.5s - 2.0s). */
#define STATE_MACHINE_UNLOCK_HOLD_MS 1500U

/* UI-5: result screens return to Idle automatically on denial. */
#define STATE_MACHINE_RESULT_DISPLAY_MS 1200U

/* UI-21/FR-46: how long an enrollment result ("Card registered for X",
 * "already registered", "needs confirmation") stays up before the
 * screen returns to waiting for the next session - same reasoning as
 * STATE_MACHINE_RESULT_DISPLAY_MS, just a separate constant since
 * UI-21's "clear summary" has no specific number of its own either. */
#define STATE_MACHINE_ENROLL_RESULT_DISPLAY_MS 2000U

#define COLOR_RGB565(r, g, b) (uint16_t)((((r) & 0xF8U) << 8) | (((g) & 0xFCU) << 3) | ((b) >> 3))
#define COLOR_READY    COLOR_RGB565(0x20, 0x90, 0x20)   /* UI-2: distinguishable by color, not just text */
#define COLOR_WORKING  COLOR_RGB565(0xE0, 0xA0, 0x00)
#define COLOR_GRANTED  COLOR_RGB565(0x00, 0xE0, 0x00)
#define COLOR_DENIED   COLOR_RGB565(0xE0, 0x00, 0x00)
#define COLOR_OFFLINE  COLOR_RGB565(0x80, 0x80, 0x80)
#define COLOR_FAULT    COLOR_RGB565(0xE0, 0x00, 0xE0)
#define COLOR_ENROLL   COLOR_RGB565(0x20, 0x60, 0xE0)   /* UI-16: distinct header color for Enrollment mode */

#define FACE_CROP_DIM 104U
#define FACE_BMP_PAYLOAD_SIZE (66U + FACE_CROP_DIM * FACE_CROP_DIM * 2U) /* 21,698 bytes */

static uint8_t s_faceBmpBuf[FACE_BMP_PAYLOAD_SIZE] __attribute__((section(".sramx_bss"), aligned(4)));

static void BuildFaceCropBmp(const uint16_t *frame, uint16_t faceCenterX, uint16_t faceCenterY, uint16_t faceBoxSize)
{
    /* 1. BMP Header (66 bytes, top-down RGB565 BI_BITFIELDS) */
    memset(s_faceBmpBuf, 0, 66U);
    s_faceBmpBuf[0] = 'B';
    s_faceBmpBuf[1] = 'M';
    uint32_t totalSize = FACE_BMP_PAYLOAD_SIZE;
    uint32_t pixelBytes = (uint32_t)FACE_CROP_DIM * FACE_CROP_DIM * 2U;

    /* bfSize */
    s_faceBmpBuf[2] = (uint8_t)totalSize;
    s_faceBmpBuf[3] = (uint8_t)(totalSize >> 8);
    s_faceBmpBuf[4] = (uint8_t)(totalSize >> 16);
    s_faceBmpBuf[5] = (uint8_t)(totalSize >> 24);

    /* bfOffBits = 66 */
    s_faceBmpBuf[10] = 66U;

    /* biSize = 40 */
    s_faceBmpBuf[14] = 40U;

    /* biWidth = 104 */
    s_faceBmpBuf[18] = (uint8_t)FACE_CROP_DIM;
    s_faceBmpBuf[19] = (uint8_t)(FACE_CROP_DIM >> 8);

    /* biHeight = -104 (negative = top-down) */
    int32_t negH = -(int32_t)FACE_CROP_DIM;
    s_faceBmpBuf[22] = (uint8_t)negH;
    s_faceBmpBuf[23] = (uint8_t)(negH >> 8);
    s_faceBmpBuf[24] = (uint8_t)(negH >> 16);
    s_faceBmpBuf[25] = (uint8_t)(negH >> 24);

    /* biPlanes = 1 */
    s_faceBmpBuf[26] = 1U;
    /* biBitCount = 16 */
    s_faceBmpBuf[28] = 16U;
    /* biCompression = 3 (BI_BITFIELDS) */
    s_faceBmpBuf[30] = 3U;
    /* biSizeImage = pixelBytes */
    s_faceBmpBuf[34] = (uint8_t)pixelBytes;
    s_faceBmpBuf[35] = (uint8_t)(pixelBytes >> 8);
    s_faceBmpBuf[36] = (uint8_t)(pixelBytes >> 16);
    s_faceBmpBuf[37] = (uint8_t)(pixelBytes >> 24);

    /* RGB565 channel masks */
    s_faceBmpBuf[54] = 0x00U;
    s_faceBmpBuf[55] = 0xF8U; /* Red: 0xF800 */
    s_faceBmpBuf[58] = 0xE0U;
    s_faceBmpBuf[59] = 0x07U; /* Green: 0x07E0 */
    s_faceBmpBuf[62] = 0x1FU;
    s_faceBmpBuf[63] = 0x00U; /* Blue: 0x001F */

    /* 2. Sample upright face crop matching UI_ATTENDANCE */
    uint32_t targetCoverage = (uint32_t)faceBoxSize * 140U / 100U;
    if (targetCoverage < 130U) targetCoverage = 130U;
    if (targetCoverage > 220U) targetCoverage = 220U;
    uint16_t scaleFp8 = (uint16_t)((targetCoverage << 8U) / FACE_CROP_DIM);

    uint16_t halfDim = FACE_CROP_DIM / 2U;
    uint16_t *dstPixels = (uint16_t *)(void *)&s_faceBmpBuf[66U];

    for (uint16_t row = 0U; row < FACE_CROP_DIM; row++)
    {
        int16_t dy = (int16_t)row - (int16_t)halfDim;
        int32_t dCamX = -((int32_t)dy * (int32_t)scaleFp8) >> 8U;
        int32_t baseCamX = (int32_t)faceCenterX + dCamX;

        for (uint16_t col = 0U; col < FACE_CROP_DIM; col++)
        {
            int16_t dx = (int16_t)col - (int16_t)halfDim;
            int32_t dCamY = ((int32_t)dx * (int32_t)scaleFp8) >> 8U;
            int32_t camX = baseCamX;
            int32_t camY = (int32_t)faceCenterY + dCamY;

            if (camX < 0) camX = 0;
            if (camX > 319) camX = 319;
            if (camY < 0) camY = 0;
            if (camY > 239) camY = 239;

            uint16_t px = frame[(uint32_t)camY * 320U + (uint32_t)camX];
            dstPixels[(uint32_t)row * FACE_CROP_DIM + col] = px;
        }
    }
}

static device_state_t s_state = DEVICE_STATE_BOOT;
static char s_displayText[40] = "Starting up";
static uint16_t s_displayColor = COLOR_WORKING;
static uint32_t s_faceStableCount = 0U;

static void SetDisplay(const char *text, uint16_t color)
{
    (void)strncpy(s_displayText, text, sizeof(s_displayText) - 1U);
    s_displayText[sizeof(s_displayText) - 1U] = '\0';
    s_displayColor = color;
}

static void EnterIdle(void)
{
    s_state = DEVICE_STATE_IDLE;
    s_faceStableCount = 0U;
    SetDisplay("Ready", COLOR_READY);
    UI_ATTENDANCE_UpdateClock(NET_GetUnixTime(), NET_IsOnline(), true);
}

static void EnterOffline(void)
{
    s_state = DEVICE_STATE_OFFLINE;
    SetDisplay("Offline mode", COLOR_OFFLINE);
    UI_ATTENDANCE_UpdateClock(NET_GetUnixTime(), false, true);
}

/* Runs the whole Waiting->Granted/Denied round trip synchronously - see
 * state_machine.h's header comment on why this is fine for a bare-metal
 * single loop. Owns the one and only call site for
 * ACTUATOR_Unlock()/Lock() (ARCHITECTURE.md Sec.2.4: "the latch is
 * unlocked in exactly one state"). */
static void RunAttendanceAttempt(net_method_t method, const uint16_t *frame, const ai_model_result_t *aiResult)
{
    s_state = DEVICE_STATE_WAITING;
    SetDisplay("Processing", COLOR_WORKING);
    PRINTF("StateMachine: attempt started (method=%s)\r\n", (method == NET_METHOD_FACE) ? "face" : "card");

    net_decision_t decision = {0};
    bool gotDecision = false;

    if (method == NET_METHOD_FACE && frame != NULL)
    {
        uint16_t faceCenterX = 160U;
        uint16_t faceCenterY = 120U;
        uint16_t faceBoxSize = 130U;
        if (aiResult != NULL && aiResult->boxCount > 0U)
        {
            /* aiResult->boxes[0] coordinates are already mapped to camera frame (0..319, 0..239) */
            uint16_t bx = aiResult->boxes[0].x;
            uint16_t by = aiResult->boxes[0].y;
            uint16_t bw = aiResult->boxes[0].width;
            uint16_t bh = aiResult->boxes[0].height;
            faceCenterX = (uint16_t)(bx + bw / 2U);
            faceCenterY = (uint16_t)(by + bh / 2U);
            faceBoxSize = (bw > bh) ? bw : bh;
        }

        /* UI-10, UI-11: Circular face crop with animated progress ring (upright & scaled to full face) */
        UI_ATTENDANCE_ShowVerifying(frame, faceCenterX, faceCenterY, faceBoxSize, 0U);
        NET_SetPollCallback(UI_ATTENDANCE_SpinRing);

        /* Build real upright face crop BMP (104x104) and request real verification from Cloud Server */
        BuildFaceCropBmp(frame, faceCenterX, faceCenterY, faceBoxSize);
        gotDecision = NET_RequestDecision(NET_METHOD_FACE, s_faceBmpBuf, FACE_BMP_PAYLOAD_SIZE, &decision);
    }
    else
    {
        gotDecision = NET_RequestDecision(method, NULL, 0U, &decision);
    }

    NET_SetPollCallback(NULL);

    /* FR-18: no decision within timeout -> denied, never unlock. Also
     * covers the stub network transport (see network_client_stub.c). */
    if (!gotDecision)
    {
        s_state = DEVICE_STATE_DENIED;
        SetDisplay("Not recognized", COLOR_DENIED);
        PRINTF("StateMachine: no decision (offline/timeout) -> denied\r\n");
        if (method == NET_METHOD_FACE && frame != NULL)
        {
            UI_ATTENDANCE_ShowFaceResult(false, NULL);
        }
    }
    else if (decision.granted)
    {
        s_state = DEVICE_STATE_GRANTED;
        bool isCheckout = false;
        const char *displayName = decision.name;
        if (strncmp(decision.name, "OUT:", 4) == 0)
        {
            isCheckout = true;
            displayName = decision.name + 4;
        }
        else if (strncmp(decision.name, "[OUT]", 5) == 0)
        {
            isCheckout = true;
            displayName = decision.name + 5;
        }

        char text[40];
        if (displayName[0] != '\0')
        {
            (void)snprintf(text, sizeof(text), "%s, %s", isCheckout ? "Goodbye" : "Welcome", displayName);
        }
        else
        {
            (void)snprintf(text, sizeof(text), "%s", isCheckout ? "Goodbye" : "Welcome");
        }
        SetDisplay(text, COLOR_GRANTED);
        PRINTF("StateMachine: granted (%s, isCheckout=%d) -> unlocking\r\n", displayName, (int)isCheckout);

        if (method == NET_METHOD_FACE && frame != NULL)
        {
            UI_ATTENDANCE_ShowFaceResult(true, decision.name);
        }

        /* HI-3/FR-16: unlock, hold, relock (snappy responsive hold time). */
        uint32_t holdMs = (decision.unlockDurationSec > 0U) ? ((uint32_t)decision.unlockDurationSec * 1000U) : STATE_MACHINE_UNLOCK_HOLD_MS;
        if (holdMs > 2500U) holdMs = 2000U;
        ACTUATOR_Unlock();
        SDK_DelayAtLeastUs(holdMs * 1000U, SDK_DEVICE_MAXIMUM_CPU_CLOCK_FREQUENCY);
        ACTUATOR_Lock();

        /* Result was visible during the entire unlock hold -> return to Idle immediately */
        EnterIdle();
        return;
    }
    else
    {
        s_state = DEVICE_STATE_DENIED;
        SetDisplay("Not recognized", COLOR_DENIED); /* UI-4: no detail about why/who */
        PRINTF("StateMachine: denied by server\r\n");
        if (method == NET_METHOD_FACE && frame != NULL)
        {
            UI_ATTENDANCE_ShowFaceResult(false, NULL);
        }
    }

    SDK_DelayAtLeastUs(STATE_MACHINE_RESULT_DISPLAY_MS * 1000U, SDK_DEVICE_MAXIMUM_CPU_CLOCK_FREQUENCY);
    EnterIdle();
}

void STATE_MACHINE_Init(bool peripheralsOk)
{
    if (!peripheralsOk)
    {
        s_state = DEVICE_STATE_FAULT;
        SetDisplay("Service unavailable", COLOR_FAULT);
        PRINTF("StateMachine: Boot->Fault (peripheral init failed)\r\n");
        return;
    }

    NET_Init();
    /* Card authentication is handled 100% centrally on the server - no local card store needed */
    EnterIdle();
    PRINTF("StateMachine: Boot->Idle\r\n");
}

static void EnterEnrollWaiting(void)
{
    s_state = DEVICE_STATE_ENROLL_WAITING;
    SetDisplay("Enrollment: waiting for admin", COLOR_ENROLL);
}

/* FR-10a/FR-10b/PR-2: the entire local card-attendance decision and
 * action, followed by a best-effort report to the server. Shares
 * RunAttendanceAttempt()'s display/latch conventions but is a separate
 * function because the decision source is different (CARD_STORE_Lookup(),
 * not a network round trip) and must complete before any network call
 * is even started. */
static void RunCardAttendance(const rfid_uid_t *cardUid)
{
    s_state = DEVICE_STATE_WAITING;
    SetDisplay("Checking card...", COLOR_WORKING);
    PRINTF("StateMachine: card attempt -> checking with server\r\n");

    net_decision_t decision = {0};
    bool gotDecision = NET_RequestDecision(NET_METHOD_CARD, cardUid->uid, cardUid->length, &decision);

    if (gotDecision && decision.granted)
    {
        s_state = DEVICE_STATE_GRANTED;
        bool isCheckout = false;
        const char *displayName = decision.name;
        if (strncmp(decision.name, "OUT:", 4) == 0)
        {
            isCheckout = true;
            displayName = decision.name + 4;
        }
        else if (strncmp(decision.name, "[OUT]", 5) == 0)
        {
            isCheckout = true;
            displayName = decision.name + 5;
        }

        char text[40];
        if (displayName[0] != '\0')
        {
            (void)snprintf(text, sizeof(text), "%s, %s", isCheckout ? "Goodbye" : "Welcome", displayName);
        }
        else
        {
            (void)snprintf(text, sizeof(text), "%s", isCheckout ? "Goodbye" : "Welcome");
        }
        SetDisplay(text, COLOR_GRANTED);
        PRINTF("StateMachine: card granted by server (%s, isCheckout=%d) -> unlocking\r\n", displayName, (int)isCheckout);
        UI_ATTENDANCE_ShowCardResult(true, decision.name);

        uint32_t holdMs = (decision.unlockDurationSec > 0U) ? ((uint32_t)decision.unlockDurationSec * 1000U) : STATE_MACHINE_UNLOCK_HOLD_MS;
        if (holdMs > 2500U) holdMs = 2000U;
        ACTUATOR_Unlock();
        SDK_DelayAtLeastUs(holdMs * 1000U, SDK_DEVICE_MAXIMUM_CPU_CLOCK_FREQUENCY);
        ACTUATOR_Lock();

        EnterIdle();
        return;
    }
    else
    {
        s_state = DEVICE_STATE_DENIED;
        SetDisplay("Not recognized", COLOR_DENIED);
        PRINTF("StateMachine: card denied by server\r\n");
        UI_ATTENDANCE_ShowCardResult(false, NULL);
    }

    SDK_DelayAtLeastUs(STATE_MACHINE_RESULT_DISPLAY_MS * 1000U, SDK_DEVICE_MAXIMUM_CPU_CLOCK_FREQUENCY);
    EnterIdle();
}

/* FR-44..48/UI-17/UI-20/UI-21/SR-10: Enrollment mode's entire poll
 * handling. Deliberately has NO call to ACTUATOR_Unlock() anywhere in
 * this function or anything it calls - that is SR-10 enforced by
 * construction, not by a runtime check. Only card-registration sessions
 * (FR-48) are implemented; face-enrollment sessions (FR-47, live camera
 * preview with pose guidance) are NOT - see WORKLOG.md. */
static void EnrollmentPoll(const rfid_uid_t *cardUid)
{
    if (s_state != DEVICE_STATE_ENROLL_WAITING && s_state != DEVICE_STATE_ENROLL_SESSION)
    {
        EnterEnrollWaiting(); /* just switched into Enrollment mode (FR-42: applied now that we're at rest) */
    }

    net_enrollment_session_t session;
    bool hasSession = NET_GetEnrollmentSession(&session) && session.active;

    if (!hasSession)
    {
        if (s_state != DEVICE_STATE_ENROLL_WAITING)
        {
            EnterEnrollWaiting();
        }
        return;
    }

    /* Every session this protocol/server can report is a card session -
     * neither proto_heartbeat_payload_t nor EnrollmentStartRequest on the
     * server carry a session type field, because face sessions (FR-47)
     * aren't implemented on either side yet (see WORKLOG.md). Nothing
     * to branch on here until that changes. */
    s_state = DEVICE_STATE_ENROLL_SESSION;
    char text[40];
    (void)snprintf(text, sizeof(text), "Scan card for %s (%us)", session.userName, (unsigned)session.expiresInSec);
    SetDisplay(text, COLOR_WORKING);

    if (cardUid == NULL || cardUid->length == 0U)
    {
        return; /* UI-20: still waiting for a tap */
    }

    net_enroll_result_t result;
    char holderName[32];
    bool ok = NET_SubmitEnrollmentCard(cardUid->uid, cardUid->length, &result, holderName, sizeof(holderName));
    if (!ok)
    {
        SetDisplay("Enrollment: server error - try again", COLOR_DENIED);
        PRINTF("StateMachine: enrollment card submit failed (transport)\r\n");
    }
    else
    {
        switch (result)
        {
            case NET_ENROLL_RESULT_ASSIGNED:
                (void)snprintf(text, sizeof(text), "Card registered for %s", session.userName);
                SetDisplay(text, COLOR_GRANTED);
                break;
            case NET_ENROLL_RESULT_ALREADY_REGISTERED:
                SetDisplay("Already registered to this user", COLOR_READY);
                break;
            case NET_ENROLL_RESULT_NEEDS_CONFIRMATION:
                (void)snprintf(text, sizeof(text), "Card held by %s - confirm on web", holderName);
                SetDisplay(text, COLOR_DENIED); /* UI-21/UI-25: admin must resolve this on the web app */
                break;
            default:
                SetDisplay("Enrollment: error", COLOR_DENIED);
                break;
        }
        PRINTF("StateMachine: enrollment card submit result=%d\r\n", (int)result);
    }

    /* FR-46/FR-48: the server already ended the session on this submit
     * (assigned/already_registered/needs_confirmation all terminate it -
     * see enrollment.py). Show the summary (UI-21), then go back to
     * waiting for the next one. */
    SDK_DelayAtLeastUs(STATE_MACHINE_ENROLL_RESULT_DISPLAY_MS * 1000U, SDK_DEVICE_MAXIMUM_CPU_CLOCK_FREQUENCY);
    EnterEnrollWaiting();
}

void STATE_MACHINE_Poll(const uint16_t *frame, const ai_model_result_t *aiResult, const rfid_uid_t *cardUid)
{
    if (s_state == DEVICE_STATE_FAULT)
    {
        /* ARCHITECTURE.md Sec.2.4: Fault->Boot only via watchdog reset -
         * nothing in this firmware clears Fault in software, by design
         * (SR-6: a fault state must not self-heal into granting access). */
        return;
    }

    /* FR-2 stability counter - tracked only when an actual camera frame inference occurred (aiResult != NULL).
     * Idle polls without a frame (aiResult == NULL) must NOT reset the counter. */
    if (aiResult != NULL)
    {
        bool faceDetected = aiResult->valid && (aiResult->boxCount > 0U);
        s_faceStableCount = faceDetected ? (s_faceStableCount + 1U) : 0U;
    }

    bool atRest = (s_state == DEVICE_STATE_IDLE) || (s_state == DEVICE_STATE_OFFLINE) ||
                  (s_state == DEVICE_STATE_ENROLL_WAITING) || (s_state == DEVICE_STATE_ENROLL_SESSION);
    if (!atRest)
    {
        return; /* Waiting/Granted/Denied are handled synchronously inside RunCardAttendance()/RunAttendanceAttempt() - nothing else to do mid-attempt. */
    }

    /* CI-6: every heartbeat refreshes the mode/enrollment-session/
     * card_store_version cache NET_GetMode() etc. read from - this is
     * the only place that cache gets refreshed while idle. */
    bool online = NET_IsOnline();

    if (online && NET_GetMode() == NET_MODE_ENROLLMENT)
    {
        EnrollmentPoll(cardUid);
        return;
    }

    if (s_state == DEVICE_STATE_ENROLL_WAITING || s_state == DEVICE_STATE_ENROLL_SESSION)
    {
        /* Mode changed away from Enrollment (or the link dropped, in
         * which case staying an enrollment station with no way to
         * confirm anything against the server would be meaningless,
         * SR-10) - fall through to Attendance mode's own states below. */
        EnterIdle();
    }
    else if (s_state == DEVICE_STATE_OFFLINE && online)
    {
        EnterIdle(); /* Offline->Idle: link restored */
    }

    /* FR-10a/PR-2: card access is decided from the local store - this
     * runs online OR offline, unlike the face path below (FR-24). Card
     * detection takes priority over a pending face stability count, same
     * as FR-8. */
    if (cardUid != NULL && cardUid->length > 0U)
    {
        RunCardAttendance(cardUid);
        return;
    }

    if (!online)
    {
        /* FR-24 / UI-14: no offline face access - if face presented while offline, show clear message */
        if (s_faceStableCount >= STATE_MACHINE_FACE_STABLE_FRAMES)
        {
            s_faceStableCount = 0U;
            UI_ATTENDANCE_ShowOfflineNotice();
            SDK_DelayAtLeastUs(STATE_MACHINE_RESULT_DISPLAY_MS * 1000U, SDK_DEVICE_MAXIMUM_CPU_CLOCK_FREQUENCY);
            EnterOffline();
        }
        else
        {
            if (s_state != DEVICE_STATE_OFFLINE)
            {
                EnterOffline();
            }
            else
            {
                UI_ATTENDANCE_UpdateClock(NET_GetUnixTime(), false, false);
            }
        }
        return;
    }

    if (s_faceStableCount >= STATE_MACHINE_FACE_STABLE_FRAMES)
    {
        s_faceStableCount = 0U;
        RunAttendanceAttempt(NET_METHOD_FACE, frame, aiResult);
        return;
    }

    /* Idle state: update digital clock display smoothly without flicker (UI-7, UI-9) */
    UI_ATTENDANCE_UpdateClock(NET_GetUnixTime(), true, false);
}

device_state_t STATE_MACHINE_GetState(void)
{
    return s_state;
}

const char *STATE_MACHINE_GetDisplayText(void)
{
    return s_displayText;
}

uint16_t STATE_MACHINE_GetDisplayColor(void)
{
    return s_displayColor;
}
