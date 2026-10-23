/*
 * main_edge_node.c - Edge AI Node main loop: camera+AI (proven, copied
 * from main.c's default path) plus RFID polling, the device state
 * machine, and the servo latch - the pieces main.c's camera-bring-up
 * build intentionally doesn't have yet (see ../../README.md's "what
 * still needs to be added" list).
 *
 * Built only under -DEDGE_NODE_BUILD=ON (see CMakeLists.txt) - a
 * separate source file rather than #ifdef'ing this into main.c, so the
 * hardware-proven camera/AI/LCD/SD default build (main.c, status
 * documented in README.md as "confirmed working on real hardware")
 * stays completely unmodified and remains buildable/flashable as a
 * rollback, same reasoning CMakeLists.txt already applies to the
 * DUALCORE_RTOS option.
 *
 * NOT yet confirmed on real hardware - RFID, servo, and the state
 * machine are new as of this change; only the camera+AI portion inherits
 * main.c's hardware-proven status.
 */

#include "actuator.h"
#include "app.h"
#include "board.h"
#include "camera_capture.h"
#include "font5x7.h"
#include "fsl_common.h"
#include "fsl_debug_console.h"
#include "lcd_display.h"
#include "model_runner.h"
#include "rfid_rc522.h"
#include "snapshot.h"
#include "state_machine.h"
#include "text_overlay.h"
#include "ui_attendance.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    BOARD_InitHardware();

    PRINTF("\r\nLOCK_HYBRID_AI - Edge AI Node (FRDM-MCXN947)\r\n");
    PRINTF("Camera: OV7670 on J9 SmartDMA/Camera header (background AI)\r\n");
    PRINTF("Display: Arduino-header LCD, Standby Lock UI (no live camera stream)\r\n\r\n");

    CAMERA_CAPTURE_Init();
    AI_MODEL_Init();
    LCD_Init();
    UI_ATTENDANCE_Init();
    SNAPSHOT_Init();
    ACTUATOR_Init();
    bool rfidOk = RFID_Init();

    STATE_MACHINE_Init(rfidOk);

    bool skipNextFrame = false;

    while (1)
    {
        /* FR-8: RFID and face monitoring run concurrently - polled every
         * loop iteration regardless of whether a camera frame is ready
         * this time around. */
        rfid_uid_t uid;
        bool cardPresent = RFID_Poll(&uid);

        if (!CAMERA_CAPTURE_IsFrameReady())
        {
            /* No new frame yet this iteration - poll state machine for card tap
             * and idle clock updates. */
            STATE_MACHINE_Poll(NULL, NULL, cardPresent ? &uid : NULL);
            continue;
        }

        CAMERA_CAPTURE_ClearFrameReady();
        if (skipNextFrame)
        {
            skipNextFrame = false;
            continue;
        }

        uint16_t *frame = CAMERA_CAPTURE_GetFrameBuffer();

        /* Same ordering main.c uses: stop SmartDMA before inference
         * so each subsystem gets uncontested bus access. */
        CAMERA_CAPTURE_Deinit();

        ai_model_result_t aiResult;
        AI_MODEL_RunInference(frame, DEMO_BUFFER_WIDTH, DEMO_BUFFER_HEIGHT, &aiResult);

        SNAPSHOT_OnFrame(frame, DEMO_BUFFER_WIDTH, DEMO_BUFFER_HEIGHT, &aiResult, AI_MODEL_GetInputWidth(),
                         AI_MODEL_GetInputHeight());

        /* UI-8: NO live camera stream to LCD. Pass frame to state machine
         * which captures a still face crop only when verifying (UI-10..12). */
        STATE_MACHINE_Poll(frame, &aiResult, cardPresent ? &uid : NULL);

        CAMERA_CAPTURE_Reinit();
        skipNextFrame = true;
    }
}
