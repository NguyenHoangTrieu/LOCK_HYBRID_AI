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

/* CameraWarmup() (skip-first-N-frames after init) was tried and reverted
 * here - a fresh photo from real hardware showed a DIFFERENT artifact
 * (regular vertical streaking with the underlying scene still visible,
 * not random colored noise) that didn't clear with warm-up and persisted
 * during steady-state running, not just right after reset. That pattern
 * points to the camera's parallel DVP bus (PCLK/HREF/VSYNC/D0-D7 on J9)
 * having a signal-integrity/wiring issue, not a software timing bug -
 * user call: accept this as a hardware limitation for now rather than
 * keep chasing it in firmware. */

/* Objective sensor-color readout, independent of the LCD panel's own
 * color response and whatever a phone camera's auto white balance does
 * when photographing that LCD - both of those distort what a photo shows
 * regardless of what the sensor is actually outputting. Sparse sample,
 * same stride/decode as model_runner_npu.cpp's diagnostic MidPx print. */
/* Same class of bug ARCHITECTURE.md documents for SD snapshots and that
 * main_core1.c's dual-core build already works around: SmartDMA keeps
 * writing the frame buffer right up until CAMERA_CAPTURE_Deinit() stops
 * it, so a read immediately after Deinit() can catch the buffer mid-write
 * - some rows old, some new, no error reported. The AI NPU log's MidPx
 * jumping wildly frame to frame even with a stationary scene/camera
 * (e.g. R=230 G=202 B=8 one frame, R=16 G=52 B=32 the next) is consistent
 * with this - a torn frame feeds the NPU inconsistent/nonsense pixels,
 * which would explain detection being unreliable independent of exposure
 * or threshold tuning. This single-core build (main_edge_node.c) never
 * had this check; dual-core's main_core1.c has had it since the SD-card
 * investigation. Cheap sparse-sample hash, same stride as
 * DEMO_LogFrameSignature()-style checks elsewhere in this project. */
#define FRAME_SETTLE_DELAY_MS  2U
#define FRAME_SETTLE_MAX_TRIES 5U

static uint32_t QuickBufferSignature(const uint16_t *frame)
{
    const uint32_t pixelCount = (uint32_t)DEMO_BUFFER_WIDTH * DEMO_BUFFER_HEIGHT;
    const uint32_t stride     = 97U;
    uint32_t sig              = 0U;

    for (uint32_t i = 0U; i < pixelCount; i += stride)
    {
        sig = (sig * 31U) + frame[i];
    }
    return sig;
}

static void WaitForFrameSettle(const uint16_t *frame)
{
    uint32_t settleSig = QuickBufferSignature(frame);
    for (uint32_t tryIdx = 0U; tryIdx < FRAME_SETTLE_MAX_TRIES; tryIdx++)
    {
        SDK_DelayAtLeastUs(FRAME_SETTLE_DELAY_MS * 1000U, SystemCoreClock);
        uint32_t settleSig2 = QuickBufferSignature(frame);
        if (settleSig2 == settleSig)
        {
            return;
        }
        settleSig = settleSig2;
    }
}

static void LogCameraColorStats(const uint16_t *frame)
{
    const uint32_t pixelCount = (uint32_t)DEMO_BUFFER_WIDTH * DEMO_BUFFER_HEIGHT;
    const uint32_t stride     = 97U;
    uint32_t sumR = 0U, sumG = 0U, sumB = 0U, samples = 0U;
    uint8_t minR = 255U, minG = 255U, minB = 255U;
    uint8_t maxR = 0U, maxG = 0U, maxB = 0U;

    for (uint32_t i = 0U; i < pixelCount; i += stride)
    {
        uint16_t px = frame[i];
        uint8_t r = (uint8_t)(((px >> 11U) & 0x1FU) * 255U / 31U);
        uint8_t g = (uint8_t)(((px >> 5U) & 0x3FU) * 255U / 63U);
        uint8_t b = (uint8_t)((px & 0x1FU) * 255U / 31U);
        sumR += r;
        sumG += g;
        sumB += b;
        if (r < minR) { minR = r; }
        if (r > maxR) { maxR = r; }
        if (g < minG) { minG = g; }
        if (g > maxG) { maxG = g; }
        if (b < minB) { minB = b; }
        if (b > maxB) { maxB = b; }
        samples++;
    }

    PRINTF("Camera color: avg R=%u G=%u B=%u | range R[%u..%u] G[%u..%u] B[%u..%u] (%u samples)\r\n",
           (unsigned)(sumR / samples), (unsigned)(sumG / samples), (unsigned)(sumB / samples),
           minR, maxR, minG, maxG, minB, maxB, (unsigned)samples);
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
    uint32_t frameCounter        = 0U;

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

        frameCounter++;
        if ((frameCounter % 30U) == 0U)
        {
            LogCameraColorStats(frame);
        }

        CAMERA_CAPTURE_Deinit();
        WaitForFrameSettle(frame);
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
        WaitForFrameSettle(frame);

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
