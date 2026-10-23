/*
 * main_edge_node.c - Edge AI Node main application.
 *
 * Supports two dual-modes on the same binary:
 *  1. Smart Lock Mode (default):
 *     - Portrait 240x320 UI (standby wallpaper, clock, face preview circle, unlock latch)
 *     - Background AI face recognition + RFID-RC522 + Servo latch + ESP32 network sync
 *     - Pressing SW3 button (P0_6) or touching the screen reboots into Camera Preview Mode
 *  2. Camera Preview Mode:
 *     - Landscape 320x240 full-screen direct camera stream
 *     - Used for lens focus adjustment and camera alignment
 *     - Pressing SW3 button (P0_6) or touching the screen reboots into Smart Lock Mode
 *
 * Mode persists across software reboot (NVIC_SystemReset) via the non-cleared
 * .noinit section in SRAMX.
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
#include "touch_xpt2046.h"
#include "ui_attendance.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define BOOT_MODE_MAGIC_LOCK 0x55AA1122U
#define BOOT_MODE_MAGIC_CAM  0x55AA3344U

/* Persists across NVIC_SystemReset() in SRAMX (.noinit is not cleared by hardware_init) */
static volatile uint32_t s_bootMode __attribute__((section(".noinit"), used));
static volatile uint32_t s_bootModeInv __attribute__((section(".noinit"), used));

static bool IsCameraPreviewMode(void)
{
    return (s_bootMode == BOOT_MODE_MAGIC_CAM && s_bootModeInv == ~BOOT_MODE_MAGIC_CAM);
}

static void SetNextBootMode(uint32_t mode)
{
    s_bootMode    = mode;
    s_bootModeInv = ~mode;
}

static bool IsModeSwitchRequested(void)
{
    /* Triggers if SW3 button (P0_6) is pressed OR touchscreen is touched */
    bool sw3Pressed = (GPIO_PinRead(BOARD_SW3_GPIO, BOARD_SW3_PIN) == 0U);
    bool touchPressed = TOUCH_IsPressed();
    return (sw3Pressed || touchPressed);
}

static void WaitForTriggerRelease(void)
{
    /* If the user is holding SW3 or the screen during reset, wait up to 500ms for release */
    uint32_t startCycle = DWT->CYCCNT;
    while (IsModeSwitchRequested() && ((DWT->CYCCNT - startCycle) < (SystemCoreClock / 2)))
    {
        SDK_DelayAtLeastUs(10000, SystemCoreClock);
    }
}

static void RunCameraPreviewMode(void)
{
    PRINTF("\r\n=======================================================\r\n");
    PRINTF("  MODE: CAMERA PREVIEW (320x240 Landscape)\r\n");
    PRINTF("  Live camera preview active - adjust/focus lens.\r\n");
    PRINTF("  PRESS SW3 BUTTON (or touch screen) to reboot to LOCK MODE.\r\n");
    PRINTF("=======================================================\r\n\r\n");

    CAMERA_CAPTURE_Init();
    LCD_Init();
    LCD_SetOrientation(true); /* landscape 320x240 */
    LCD_Clear(0x0000U, 320, 240);
    TOUCH_Init();

    WaitForTriggerRelease();

    bool skipNextFrame           = false;
    uint32_t switchPressStartCycle = 0;
    bool switching               = false;

    while (1)
    {
        /* Check SW3 button or touch panel */
        if (IsModeSwitchRequested())
        {
            if (!switching)
            {
                switching              = true;
                switchPressStartCycle = DWT->CYCCNT;
            }
            else if ((DWT->CYCCNT - switchPressStartCycle) >= (SystemCoreClock / 7)) /* ~140 ms debounce */
            {
                PRINTF("\r\n[SWITCH] Button SW3 / Touch detected! Switching to SMART LOCK MODE & rebooting...\r\n");
                SetNextBootMode(BOOT_MODE_MAGIC_LOCK);
                SDK_DelayAtLeastUs(200000, SystemCoreClock);
                NVIC_SystemReset();
            }
        }
        else
        {
            switching = false;
        }

        if (!CAMERA_CAPTURE_IsFrameReady())
        {
            continue;
        }

        CAMERA_CAPTURE_ClearFrameReady();
        if (skipNextFrame)
        {
            skipNextFrame = false;
            continue;
        }

        uint16_t *frame = CAMERA_CAPTURE_GetFrameBuffer();

        CAMERA_CAPTURE_Deinit();
        LCD_DrawImage(0U, 0U, DEMO_BUFFER_WIDTH, DEMO_BUFFER_HEIGHT, frame);
        CAMERA_CAPTURE_Reinit();
        skipNextFrame = true;
    }
}

static void RunSmartLockMode(void)
{
    PRINTF("\r\n=======================================================\r\n");
    PRINTF("  LOCK_HYBRID_AI - Smart Lock Mode (FRDM-MCXN947)\r\n");
    PRINTF("  Camera: OV7670 on J9 (background AI face recognition)\r\n");
    PRINTF("  Display: Arduino-header LCD (Standby UI, portrait 240x320)\r\n");
    PRINTF("  PRESS SW3 BUTTON (or touch screen) to reboot to CAMERA PREVIEW.\r\n");
    PRINTF("=======================================================\r\n\r\n");

    CAMERA_CAPTURE_Init();
    AI_MODEL_Init();
    LCD_Init();
    LCD_SetOrientation(false); /* portrait 240x320 */
    TOUCH_Init();
    UI_ATTENDANCE_Init();
    SNAPSHOT_Init();
    ACTUATOR_Init();
    bool rfidOk = RFID_Init();

    STATE_MACHINE_Init(rfidOk);

    WaitForTriggerRelease();

    bool skipNextFrame           = false;
    uint32_t switchPressStartCycle = 0;
    bool switching               = false;

    while (1)
    {
        /* Check SW3 button or touch panel */
        if (IsModeSwitchRequested())
        {
            if (!switching)
            {
                switching              = true;
                switchPressStartCycle = DWT->CYCCNT;
            }
            else if ((DWT->CYCCNT - switchPressStartCycle) >= (SystemCoreClock / 7)) /* ~140 ms debounce */
            {
                PRINTF("\r\n[SWITCH] Button SW3 / Touch detected! Switching to CAMERA PREVIEW & rebooting...\r\n");
                SetNextBootMode(BOOT_MODE_MAGIC_CAM);
                SDK_DelayAtLeastUs(200000, SystemCoreClock);
                NVIC_SystemReset();
            }
        }
        else
        {
            switching = false;
        }

        /* FR-8: RFID and face monitoring run concurrently */
        rfid_uid_t uid;
        bool cardPresent = RFID_Poll(&uid);

        if (!CAMERA_CAPTURE_IsFrameReady())
        {
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

        CAMERA_CAPTURE_Deinit();

        ai_model_result_t aiResult;
        AI_MODEL_RunInference(frame, DEMO_BUFFER_WIDTH, DEMO_BUFFER_HEIGHT, &aiResult);

        SNAPSHOT_OnFrame(frame, DEMO_BUFFER_WIDTH, DEMO_BUFFER_HEIGHT, &aiResult, AI_MODEL_GetInputWidth(),
                         AI_MODEL_GetInputHeight());

        STATE_MACHINE_Poll(frame, &aiResult, cardPresent ? &uid : NULL);

        CAMERA_CAPTURE_Reinit();
        skipNextFrame = true;
    }
}

int main(void)
{
    BOARD_InitHardware();

    /* Enable DWT cycle counter for precise microsecond timing & debouncing */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    if (IsCameraPreviewMode())
    {
        RunCameraPreviewMode();
    }
    else
    {
        RunSmartLockMode();
    }

    return 0;
}
