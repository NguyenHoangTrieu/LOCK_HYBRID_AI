/*
 * ui_attendance.h - Lock UI for Attendance Mode (SRS v2.1, UI-7..UI-15)
 *
 * Implements:
 *  - UI-7: Idle display with synchronized digital clock & date (lock screen).
 *  - UI-8: NO live camera stream to LCD in attendance mode (background NPU).
 *  - UI-9: Connection status (online/offline) and interaction prompt.
 *  - UI-10: Circular crop of detected face still image.
 *  - UI-11: Animated circular progress ring during verification.
 *  - UI-12: Decision result (Green ring + name on grant, Red ring on deny).
 *  - UI-13: Immediate card tap result display.
 *  - UI-14: Clear offline warning message when face presented while offline.
 *  - UI-15: Clear state indication at all times.
 */
#ifndef _UI_ATTENDANCE_H_
#define _UI_ATTENDANCE_H_

#include <stdbool.h>
#include <stdint.h>
#include "model_runner.h"

/* RGB565 Palette */
#define UI_COLOR_BG           0x0000U /* Black background */
#define UI_COLOR_WHITE        0xFFFFU
#define UI_COLOR_GRAY         0x8410U
#define UI_COLOR_DARK_GRAY    0x39E7U
#define UI_COLOR_GREEN        0x07E0U /* Granted */
#define UI_COLOR_RED          0xF800U /* Denied */
#define UI_COLOR_YELLOW       0xFFE0U /* Warning / offline */
#define UI_COLOR_CYAN         0x07FFU /* High-tech prompt */
#define UI_COLOR_BLUE         0x001FU

/* Attendance UI States */
typedef enum
{
    UI_ATTENDANCE_STATE_IDLE = 0,
    UI_ATTENDANCE_STATE_VERIFYING,
    UI_ATTENDANCE_STATE_RESULT_FACE,
    UI_ATTENDANCE_STATE_RESULT_CARD,
    UI_ATTENDANCE_STATE_OFFLINE_NOTICE,
} ui_attendance_state_t;

/*!
 * @brief Initialize the attendance UI module.
 */
void UI_ATTENDANCE_Init(void);

/*!
 * @brief Update the idle lock screen with digital clock, date, and status.
 *        Does NOT flicker; only redraws text when seconds/time changes.
 *
 * @param unixTime Current unix epoch (0 if not yet synced).
 * @param online   True if backend connection is active.
 * @param forceRedraw If true, redraws full background and all fields.
 */
void UI_ATTENDANCE_UpdateClock(uint32_t unixTime, bool online, bool forceRedraw);

/*!
 * @brief Crop face from camera frame into circular frame and show "Verifying..." with progress ring.
 *
 * @param frame Camera frame (320x240 RGB565).
 * @param faceCenterX Center X of detected face in camera space (0..319).
 * @param faceCenterY Center Y of detected face in camera space (0..239).
 * @param animStep Initial animation phase (0..7).
 */
void UI_ATTENDANCE_ShowVerifying(const uint16_t *frame, uint16_t faceCenterX, uint16_t faceCenterY, uint16_t faceBoxSize, uint8_t animStep);

/*!
 * @brief Advance the spinning progress ring by one step during verification.
 */
void UI_ATTENDANCE_SpinRing(void);

/*!
 * @brief Show verification result for face (Green ring + name if granted, Red ring if denied).
 *
 * @param granted True if server granted access.
 * @param name User name (if granted).
 */
void UI_ATTENDANCE_ShowFaceResult(bool granted, const char *name);

/*!
 * @brief Show immediate card tap result (UI-13).
 *
 * @param granted True if local card store granted access.
 * @param name User name (if granted).
 */
void UI_ATTENDANCE_ShowCardResult(bool granted, const char *name);

/*!
 * @brief Show offline notice when face is detected while offline (UI-14).
 */
void UI_ATTENDANCE_ShowOfflineNotice(void);

/*!
 * @brief Returns the current UI state.
 */
ui_attendance_state_t UI_ATTENDANCE_GetState(void);

#endif /* _UI_ATTENDANCE_H_ */
