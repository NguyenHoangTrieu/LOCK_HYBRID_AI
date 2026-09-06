/*
 * main_core1.c - Camera_AI_Test1 dual-core RTOS migration (see WORKLOG.md).
 *
 * This core owns camera capture, LCD push, AI overlay drawing, and SD
 * snapshot - all in one task (CameraLcdTask). Splitting capture/push into
 * separate tasks would reintroduce a single-buffer race this project
 * already fixed once (see ARCHITECTURE.md).
 *
 * Real AI results come from core0 over a frame-ready/result-ready
 * doorbell round trip (source/shared/ipc_events.h) - core1 calls
 * CAMERA_CAPTURE_Deinit(), signals core0, waits for the reply, then draws
 * the overlay / saves the snapshot before CAMERA_CAPTURE_Reinit().
 */
#include <string.h>
#include "fsl_debug_console.h"
#include "board.h"
#include "app.h"
#include "mcmgr.h"
#include "FreeRTOS.h"
#include "task.h"
#include "camera_capture.h"
#include "lcd_display.h"
#include "bbox_overlay.h"
#include "snapshot.h"
#include "spi1_bus.h"
#include "ipc_layout.h"
#include "ipc_events.h"

/* Model's fixed input resolution - can't call AI_MODEL_GetInputWidth/
 * Height() from core1 (model_runner.h's implementation only builds into
 * core0's image). Must match the model actually deployed on core0 - see
 * its own boot log ("AI_MODEL_Init: ... (72x72 input...)"). */
#define AI_MODEL_INPUT_WIDTH  72U
#define AI_MODEL_INPUT_HEIGHT 72U

/* Generous vs. the ~3.9ms NPU inference time actually measured. A timeout
 * here means core0 didn't answer in time - this frame just skips the AI
 * overlay/snapshot check, not a hang. */
#define AI_RESULT_TIMEOUT_MS 100U

/* Identical to main.c's DEMO_ClearScreen() - copied rather than shared
 * since the two builds don't share source files, only headers/drivers. */
static void DEMO_ClearScreen(uint16_t color)
{
    static uint16_t s_clearLine[DEMO_PANEL_WIDTH];
    for (uint16_t i = 0U; i < DEMO_PANEL_WIDTH; i++)
    {
        s_clearLine[i] = color;
    }
    /* Whole multi-call sequence must be atomic against other bus users -
     * see spi1_bus.h. */
    SPI1_BUS_Lock();
    LCD_SetWindow(0U, 0U, DEMO_PANEL_WIDTH - 1U, DEMO_PANEL_HEIGHT - 1U);
    for (uint16_t row = 0U; row < DEMO_PANEL_HEIGHT; row++)
    {
        LCD_PushPixelsOpen(s_clearLine, DEMO_PANEL_WIDTH);
    }
    LCD_EndWindow();
    SPI1_BUS_Unlock();
}

/* Diagnostic: the fps counter alone doesn't prove the pixel data is real -
 * it would keep counting even on a stuck all-zero buffer. A flat
 * min==max reading is the tell for dead/never-written data. */
static void DEMO_LogFrameSignature(const uint16_t *frame)
{
    const uint32_t pixelCount = (uint32_t)DEMO_BUFFER_WIDTH * DEMO_BUFFER_HEIGHT;
    const uint32_t stride     = 97U;
    uint16_t minPixel         = 0xFFFFU;
    uint16_t maxPixel         = 0x0000U;
    uint32_t sum              = 0U;
    uint32_t samples          = 0U;

    for (uint32_t i = 0; i < pixelCount; i += stride)
    {
        uint16_t p = frame[i];
        if (p < minPixel)
        {
            minPixel = p;
        }
        if (p > maxPixel)
        {
            maxPixel = p;
        }
        sum += p;
        samples++;
    }

    PRINTF("Camera: %u samples, pixel range 0x%04X..0x%04X, avg=0x%04X%s\r\n", samples, minPixel, maxPixel,
           (uint16_t)(sum / samples), (minPixel == maxPixel) ? " (flat - dead/no data)" : "");
}

/* Converts the wire-safe ai_ipc_result_t (plain data, no cross-core
 * pointers - see ipc_layout.h) into the ai_model_result_t shape
 * snapshot.h/bbox drawing expect. `out`'s label pointers point INTO
 * `ipc`, so `ipc` must outlive `out` - true at every call site below. */
static void ConvertIpcResult(const ai_ipc_result_t *ipc, ai_model_result_t *out)
{
    out->valid    = ipc->valid;
    out->boxCount = (ipc->boxCount > AI_MODEL_MAX_BOXES) ? AI_MODEL_MAX_BOXES : ipc->boxCount;
    for (uint32_t i = 0; i < out->boxCount; i++)
    {
        out->boxes[i].label  = ipc->boxes[i].label;
        out->boxes[i].x      = ipc->boxes[i].x;
        out->boxes[i].y      = ipc->boxes[i].y;
        out->boxes[i].width  = ipc->boxes[i].width;
        out->boxes[i].height = ipc->boxes[i].height;
        out->boxes[i].score  = ipc->boxes[i].score;
    }
}

/* Cheap rolling hash over a sparse sample of the frame buffer, same
 * stride as DEMO_LogFrameSignature() - used to check whether the buffer
 * is still changing (see the settle-check comment below). */
static uint32_t QuickBufferSignature(const uint16_t *frame)
{
    const uint32_t pixelCount = (uint32_t)DEMO_BUFFER_WIDTH * DEMO_BUFFER_HEIGHT;
    const uint32_t stride     = 97U;
    uint32_t sig              = 0U;

    for (uint32_t i = 0; i < pixelCount; i += stride)
    {
        sig = (sig * 31U) + frame[i];
    }
    return sig;
}

static void CameraLcdTask(void *pvParameters)
{
    (void)pvParameters;

    CAMERA_CAPTURE_Init();
    LCD_Init();
    DEMO_ClearScreen(0x0000U);

    /* NOT wrapped in an outer lock - see sd_spi_disk.c's
     * disk_initialize() comment: locking this whole call broke SD mount,
     * reverted to narrow per-diskio-call locking instead. */
    SNAPSHOT_Init();

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    uint32_t fpsFrameCount      = 0U;
    uint32_t fpsWindowStartCycle = DWT->CYCCNT;
    uint16_t frameSeq            = 0U;

    /* Set right after CAMERA_CAPTURE_Reinit(), cleared once the following
     * frame has been consumed - SmartDMA needs one cycle to resync with
     * the sensor after a fresh Reinit(). */
    bool skipNextFrame = false;

    /* Buffer-content settle check: some saved snapshots came back torn
     * (not truncated - correct file size, but garbled content), tracing
     * to the frame buffer still being mid-write when read, not just a
     * stale frame count (a frame-count-based check was tried first and
     * confirmed NOT sufficient on real hardware). Hash a sparse sample,
     * wait briefly, hash again, and only proceed once two consecutive
     * hashes agree - bounded retries so a genuinely stuck buffer doesn't
     * hang forever. */
#define SETTLE_CHECK_DELAY_MS  2U
#define SETTLE_CHECK_MAX_TRIES 5U

    for (;;)
    {
        if (CAMERA_CAPTURE_IsFrameReady())
        {
            CAMERA_CAPTURE_ClearFrameReady();

            if (skipNextFrame)
            {
                skipNextFrame = false;
                continue;
            }

            /* Stop SmartDMA before reading the frame buffer, restart
             * after - the core0 AI round trip below must fully finish
             * before CAMERA_CAPTURE_Reinit() runs. */
            CAMERA_CAPTURE_Deinit();
            uint16_t *frame = CAMERA_CAPTURE_GetFrameBuffer();

            uint32_t settleSig = QuickBufferSignature(frame);
            for (uint32_t settleTry = 0U; settleTry < SETTLE_CHECK_MAX_TRIES; settleTry++)
            {
                vTaskDelay(pdMS_TO_TICKS(SETTLE_CHECK_DELAY_MS));
                uint32_t settleSig2 = QuickBufferSignature(frame);
                if (settleSig2 == settleSig)
                {
                    break;
                }
                PRINTF("Camera: buffer signature changed after Deinit() (retry %u): 0x%08X -> 0x%08X\r\n",
                       (unsigned)settleTry, (unsigned)settleSig, (unsigned)settleSig2);
                settleSig = settleSig2;
            }

            frameSeq++;
            /* Set to 1 to bypass the core0 IPC round trip entirely, e.g.
             * to isolate whether a symptom correlates with the cross-core
             * exchange itself. */
#define TEMP_SKIP_IPC_ROUNDTRIP 0
#if !TEMP_SKIP_IPC_ROUNDTRIP
            IPC_SignalFrameReady(frameSeq);

            uint32_t notifiedSeq;
            bool haveResult = (xTaskNotifyWait(0, 0xFFFFFFFFU, &notifiedSeq, pdMS_TO_TICKS(AI_RESULT_TIMEOUT_MS)) ==
                               pdTRUE) &&
                              ((uint16_t)notifiedSeq == frameSeq);
            if (!haveResult)
            {
                PRINTF("AI: no result from core0 for frame seq %u within %ums - skipping AI overlay/snapshot this "
                       "frame.\r\n",
                       frameSeq, AI_RESULT_TIMEOUT_MS);
            }
#else
            bool haveResult = false;
#endif

            ai_model_result_t aiResult = {0};
            if (haveResult)
            {
                ai_ipc_result_t ipcResult;
                memcpy(&ipcResult, IPC_RESULT_ADDR, sizeof(ipcResult));
                ConvertIpcResult(&ipcResult, &aiResult);

                if (aiResult.valid)
                {
                    float scaleX = (float)DEMO_BUFFER_WIDTH / (float)AI_MODEL_INPUT_WIDTH;
                    float scaleY = (float)DEMO_BUFFER_HEIGHT / (float)AI_MODEL_INPUT_HEIGHT;
                    for (uint32_t i = 0; i < aiResult.boxCount; i++)
                    {
                        const ai_bbox_t *box = &aiResult.boxes[i];
                        BBOX_DrawRect(frame, DEMO_BUFFER_WIDTH, DEMO_BUFFER_HEIGHT, (int)((float)box->x * scaleX),
                                      (int)((float)box->y * scaleY), (int)((float)box->width * scaleX),
                                      (int)((float)box->height * scaleY), 0x07E0U /* green */);
                        /* debug_console_lite may not support %f, so print
                         * score as a percentage integer. */
                        PRINTF("AI result: box[%u] label=%s x=%u y=%u w=%u h=%u score=%d%%\r\n", i, box->label,
                               box->x, box->y, box->width, box->height, (int)(box->score * 100.0f));
                    }
                }

                /* Internally rate-limited to at most 1 capture/sec (see
                 * snapshot.h) - a no-op most frames. */
                (void)SNAPSHOT_OnFrame(frame, DEMO_BUFFER_WIDTH, DEMO_BUFFER_HEIGHT, &aiResult, AI_MODEL_INPUT_WIDTH,
                                       AI_MODEL_INPUT_HEIGHT);
            }

            LCD_DrawImage(0U, 0U, DEMO_BUFFER_WIDTH, DEMO_BUFFER_HEIGHT, frame);

            fpsFrameCount++;
            bool logSignature = false;
            if ((int32_t)(DWT->CYCCNT - fpsWindowStartCycle) >= (int32_t)SystemCoreClock)
            {
                PRINTF("LCD preview: %u fps\r\n", fpsFrameCount);
                /* Must run BEFORE CAMERA_CAPTURE_Reinit() - Reinit()
                 * memsets the frame buffer for the next capture, so
                 * logging after it would always see a freshly-cleared
                 * buffer regardless of what was actually captured. */
                logSignature = true;
                fpsFrameCount       = 0U;
                fpsWindowStartCycle = DWT->CYCCNT;
            }
            if (logSignature)
            {
                DEMO_LogFrameSignature(frame);
            }

            CAMERA_CAPTURE_Reinit();
            skipNextFrame = true;
        }
    }
}

int main(void)
{
    MCMGR_Init();
    BOARD_InitHardware();

    uint32_t startupData;
    mcmgr_status_t status;
    do
    {
        status = MCMGR_GetStartupData(kMCMGR_Core0, &startupData);
    } while (status != kStatus_MCMGR_Success);

    PRINTF("\r\nCamera_AI_Test1 - core1 (dual-core Stage 5: camera + LCD preview + AI overlay + SD snapshot)\r\n");

    /* Must exist before any task touches the shared LPSPI1 bus. */
    SPI1_BUS_CreateLock();

    TaskHandle_t cameraLcdTaskHandle;
    xTaskCreate(CameraLcdTask, "CameraLcdTask", configMINIMAL_STACK_SIZE + 512, NULL, tskIDLE_PRIORITY + 1,
                &cameraLcdTaskHandle);

    /* core0's AiInferenceTask replies to every IPC_SignalFrameReady() with
     * IPC_SignalResultReady() carrying the same sequence number - wakes
     * CameraLcdTask's xTaskNotifyWait() directly. */
    IPC_EVENTS_RegisterHandler(cameraLcdTaskHandle);

    vTaskStartScheduler();

    for (;;)
    {
        /* Should never reach here. */
    }
}
