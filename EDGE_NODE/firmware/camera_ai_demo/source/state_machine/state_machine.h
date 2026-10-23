/*
 * state_machine.h - device state machine, ARCHITECTURE.md Sec.2.4.
 *
 * Owns all state (ARCHITECTURE.md's rule 1: "only the state machine
 * changes state"). Callers (main_edge_node.c) only ever report raw
 * events in - current face-detection result each frame, current RFID
 * poll result - and read the display text/color back out. They never set
 * state directly and never touch the actuator directly either, so the
 * "latch unlocked in exactly one state" invariant (ARCHITECTURE.md Sec.2.4)
 * can't be bypassed from outside this file.
 */
#ifndef _STATE_MACHINE_H_
#define _STATE_MACHINE_H_

#include <stdbool.h>
#include <stdint.h>
#include "model_runner.h"
#include "rfid_rc522.h"

typedef enum
{
    DEVICE_STATE_BOOT,
    DEVICE_STATE_IDLE,
    DEVICE_STATE_WAITING,
    DEVICE_STATE_GRANTED,
    DEVICE_STATE_DENIED,
    DEVICE_STATE_OFFLINE,
    DEVICE_STATE_FAULT,
    /* FR-39/FR-44..48: Enrollment mode (SRS v2.1, SRS.3.2.9) - orthogonal
     * to the Attendance-mode diagram in ARCHITECTURE.md Sec.2.4 above,
     * which predates this mode and still needs updating to show it (see
     * WORKLOG.md). SR-10: ACTUATOR_Unlock() is never called anywhere on
     * this path (grep state_machine.c - its only call site is inside
     * RunCardAttendance()/RunAttendanceAttempt(), neither of which runs
     * while the device is in Enrollment mode). */
    DEVICE_STATE_ENROLL_WAITING, /* UI-17: no session active, waiting for an admin */
    DEVICE_STATE_ENROLL_SESSION, /* UI-20: a card-registration session is active */
} device_state_t;

/*! @brief Call once after ACTUATOR_Init()/RFID_Init(). `peripheralsOk`
 *  false sends the device straight to DEVICE_STATE_FAULT (Boot->Fault,
 *  "init failed" - ARCHITECTURE.md Sec.2.4) instead of Idle. */
void STATE_MACHINE_Init(bool peripheralsOk);

/*! @brief Call once per main-loop iteration with this frame's raw RGB565 buffer (NULL if no new frame),
 *  AI result (NULL if no new frame) and this iteration's RFID poll result (NULL if
 *  RFID_Poll() returned false). Owns the FR-2 stability counter and drives the
 *  Attendance Lock UI (UI-7..UI-15) as well as the actuator and network transactions. */
void STATE_MACHINE_Poll(const uint16_t *frame, const ai_model_result_t *aiResult, const rfid_uid_t *cardUid);

device_state_t STATE_MACHINE_GetState(void);

/*! @brief UI-1/UI-2/UI-3/UI-4: human-readable status text and a 16-bit
 *  RGB565 color for the current state, ready to hand to
 *  TEXT_DrawString(). Text already includes the identified person's name
 *  on a grant (UI-3) and never reveals anything about enrolled users on a
 *  denial (UI-4). */
const char *STATE_MACHINE_GetDisplayText(void);
uint16_t STATE_MACHINE_GetDisplayColor(void);

#endif /* _STATE_MACHINE_H_ */
