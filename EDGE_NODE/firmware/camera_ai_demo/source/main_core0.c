/*
 * main_core0.c - Camera_AI_Test1 dual-core RTOS migration (see WORKLOG.md).
 *
 * This core boots core1 via MCMGR, then just runs AI inference:
 * AiInferenceTask blocks on core1's frame-ready doorbell, reads the
 * shared frame buffer (only safe between core1's Deinit()/Reinit() calls -
 * see ipc_events.h), runs inference via the same model_runner.h API the
 * legacy single-core build uses, writes the result to shared RAM as a
 * plain-data struct (ipc_layout.h - not the pointer-carrying
 * ai_model_result_t), and replies with IPC_SignalResultReady().
 */
#include <string.h>
#include "fsl_debug_console.h"
#include "fsl_gpio.h"
#include "board.h"
#include "app.h"
#include "mcmgr.h"
#include "FreeRTOS.h"
#include "task.h"
#include "model_runner.h"
#include "ipc_layout.h"
#include "ipc_events.h"

static void AiInferenceTask(void *pvParameters)
{
    (void)pvParameters;

    AI_MODEL_Init();

    for (;;)
    {
        uint32_t frameSeq;
        if (xTaskNotifyWait(0, 0xFFFFFFFFU, &frameSeq, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        /* Safe to read: core1 stopped SmartDMA (CAMERA_CAPTURE_Deinit())
         * before signaling frame-ready, and won't touch the buffer again
         * (CAMERA_CAPTURE_Reinit()) until it receives this task's reply
         * below - see ipc_events.h's IPC_SignalFrameReady() comment. */
        const uint16_t *frame = (const uint16_t *)IPC_FRAME_BUFFER_ADDR;

        ai_model_result_t result;
        AI_MODEL_RunInference(frame, DEMO_BUFFER_WIDTH, DEMO_BUFFER_HEIGHT, &result);

        /* Convert to the wire-safe, pointer-free shape (ipc_layout.h) -
         * result.boxes[i].label is a `const char *` into core0's own
         * flash, not portable data to hand to core1 as-is. */
        ai_ipc_result_t ipcResult = {0};
        ipcResult.valid    = result.valid;
        ipcResult.boxCount = (result.boxCount > AI_IPC_MAX_BOXES) ? AI_IPC_MAX_BOXES : result.boxCount;
        for (uint32_t i = 0; i < ipcResult.boxCount; i++)
        {
            const ai_bbox_t *box = &result.boxes[i];
            strncpy(ipcResult.boxes[i].label, box->label, AI_IPC_LABEL_LEN - 1U);
            ipcResult.boxes[i].label[AI_IPC_LABEL_LEN - 1U] = '\0';
            ipcResult.boxes[i].x      = box->x;
            ipcResult.boxes[i].y      = box->y;
            ipcResult.boxes[i].width  = box->width;
            ipcResult.boxes[i].height = box->height;
            ipcResult.boxes[i].score  = box->score;
        }
        memcpy(IPC_RESULT_ADDR, &ipcResult, sizeof(ipcResult));

        IPC_SignalResultReady((uint16_t)frameSeq);
    }
}

int main(void)
{
    MCMGR_Init();
    BOARD_InitHardware();

    PRINTF("\r\nCamera_AI_Test1 - core0 (dual-core Stage 5: AI inference)\r\n");

    /* core1 has no SAU (always Non-Secure) - grant it GPIO access to its
     * own LCD pins before releasing it below, or its writes silently do
     * nothing (see WORKLOG.md, KNOWLEDGE.md §9). Pin numbers must match
     * core1/app.h - can't include that header from here. */
    GPIO0->PCNS |= GPIO_PCNS_NSE15_MASK  /* LCD RST, P0_15 */
                 | GPIO_PCNS_NSE22_MASK  /* LCD CS,  P0_22 */
                 | GPIO_PCNS_NSE23_MASK; /* LCD BLK, P0_23 */
    GPIO1->PCNS |= GPIO_PCNS_NSE23_MASK; /* LCD DC,  P1_23 (Arduino D3) */

#ifdef CORE1_IMAGE_COPY_TO_RAM
    uint32_t core1_image_size = get_core1_image_size();
    PRINTF("core0: copying core1 image to 0x%x, size %u bytes\r\n", (unsigned)CORE1_BOOT_ADDRESS,
           (unsigned)core1_image_size);
    memcpy((void *)(uintptr_t)CORE1_BOOT_ADDRESS, CORE1_IMAGE_START, core1_image_size);
#endif

    /* Calling any FreeRTOS API before MCMGR_StartCore() hangs the boot
     * handshake completely (confirmed on real hardware - see WORKLOG.md).
     * MCMGR_StartCore() itself is plain C, safe to call pre-scheduler -
     * finish the whole MCMGR handshake first, only touch FreeRTOS after. */
    PRINTF("core0: starting core1...\r\n");
    MCMGR_StartCore(kMCMGR_Core1, (void *)(uintptr_t)CORE1_BOOT_ADDRESS, 0, kMCMGR_Start_Synchronous);
    PRINTF("core0: core1 started.\r\n");

    TaskHandle_t aiTaskHandle;
    xTaskCreate(AiInferenceTask, "AiInferenceTask", configMINIMAL_STACK_SIZE + 512, NULL, tskIDLE_PRIORITY + 1,
                &aiTaskHandle);
    IPC_EVENTS_RegisterHandler(aiTaskHandle);

    vTaskStartScheduler();

    for (;;)
    {
        /* Should never reach here. */
    }
}
