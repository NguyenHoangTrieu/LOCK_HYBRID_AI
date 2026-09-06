/*
 * main.c - Camera_AI_Test1
 *
 * Default build: OV7670 capture (SmartDMA, J9 header, 320x240 RGB565) ->
 * FOMO face-detection inference (model_runner.cpp or model_runner_npu.cpp)
 * -> LCD text status line ("FACE: 1"/"0"), no USB. See WORKLOG.md for why
 * Arduino-header bit-bang is the active default over J8/FlexIO.
 *
 * bbox_overlay.c/h was originally for drawing boxes on the live LCD image;
 * that was dropped in favor of plain text (too much data over the bit-bang
 * bus), but the helper is reused by the SD snapshot feature below.
 *
 * SD card snapshot on face detection (source/storage/snapshot.c): draws a
 * box into the camera frame buffer and saves it as a BMP to the TFT
 * shield's onboard microSD slot, rate-limited to 1 capture/sec. The LCD
 * never shows the box, only the saved file does.
 *
 * USB Video Class streaming (source/usb/) is ABANDONED - SmartDMA capture
 * and the USB HS PHY need mutually exclusive DCDC voltage levels on this
 * chip, no software fix possible. Still builds (opt-in via
 * USB_STREAM_DIAGNOSTIC_DISABLE=OFF), runs time-multiplexed - see
 * WORKLOG.md.
 */

#include <stdbool.h>
#include <string.h>
#include "app.h"
#include "board.h"
#include "camera_capture.h"
#include "ei_sramx_alloc.h"
#include "font5x7.h"
#include "fsl_common.h"
#include "fsl_debug_console.h"
#include "lcd_display.h"
#include "model_runner.h"
#include "snapshot.h"
#include "text_overlay.h"
#include "usb_video_camera.h"

#if !DEMO_LCD_CAMERA_PREVIEW
/* Fixed-width label + ": " + '1'/'0' for whether a face was detected this
 * frame. Unused in the raw camera-preview build. */
#define DEMO_STATUS_TEXT_SCALE 3U
#define DEMO_STATUS_LINE_X 8U
#define DEMO_STATUS_LINE_Y0 40U
#define DEMO_STATUS_LINE_GAP_PX 14U

static void DEMO_DrawStatusLine(uint16_t lineIndex, const char *paddedLabel, bool detected) {
  char text[16];
  size_t n = strlen(paddedLabel);

  memcpy(text, paddedLabel, n);
  text[n] = ':';
  text[n + 1U] = ' ';
  text[n + 2U] = detected ? '1' : '0';
  text[n + 3U] = '\0';

  uint16_t lineHeight = (uint16_t)((FONT5X7_HEIGHT * DEMO_STATUS_TEXT_SCALE) + DEMO_STATUS_LINE_GAP_PX);
  uint16_t y = (uint16_t)(DEMO_STATUS_LINE_Y0 + lineIndex * lineHeight);
  uint16_t fgColor = detected ? 0x07E0U /* green - detected this frame */ : 0x7BEFU /* mid gray - not detected */;

  TEXT_DrawString(DEMO_STATUS_LINE_X, y, text, fgColor, 0x0000U /* black background */, DEMO_STATUS_TEXT_SCALE);
}
#endif /* !DEMO_LCD_CAMERA_PREVIEW */

/* Fills the LCD with one solid color at startup, to clear old GRAM
 * content. Must use LCD_PushPixelsOpen()/LCD_EndWindow() in a loop, not
 * repeated LCD_PushPixels() calls - that closes the transfer (CS high)
 * every time, so only the first row would actually reach the panel. */
static void DEMO_ClearScreen(uint16_t color) {
  static uint16_t s_clearLine[DEMO_PANEL_WIDTH];
  for (uint16_t i = 0U; i < DEMO_PANEL_WIDTH; i++) {
    s_clearLine[i] = color;
  }
  LCD_SetWindow(0U, 0U, DEMO_PANEL_WIDTH - 1U, DEMO_PANEL_HEIGHT - 1U);
  for (uint16_t row = 0U; row < DEMO_PANEL_HEIGHT; row++) {
    LCD_PushPixelsOpen(s_clearLine, DEMO_PANEL_WIDTH);
  }
  LCD_EndWindow();
}

#if !DEMO_LCD_CAMERA_PREVIEW
/* Cheap "is the camera sending real data" check: min/max/avg over a
 * strided sample. A dead/disconnected sensor reads flat (min==max). */
static void CAMERA_CAPTURE_LogFrameSignature(uint32_t frameNumber,
                                             const uint16_t *frame) {
  const uint32_t pixelCount = (uint32_t)DEMO_BUFFER_WIDTH * DEMO_BUFFER_HEIGHT;
  const uint32_t stride = 97U; /* prime, avoids lining up with row width */
  uint16_t minPixel = 0xFFFFU;
  uint16_t maxPixel = 0x0000U;
  uint32_t sum = 0U;
  uint32_t samples = 0U;

  for (uint32_t i = 0; i < pixelCount; i += stride) {
    uint16_t p = frame[i];
    if (p < minPixel) {
      minPixel = p;
    }
    if (p > maxPixel) {
      maxPixel = p;
    }
    sum += p;
    samples++;
  }

  PRINTF("Camera: frame #%u ready, %u samples, pixel range 0x%04X..0x%04X, "
         "avg=0x%04X%s\r\n",
         frameNumber, samples, minPixel, maxPixel, (uint16_t)(sum / samples),
         (minPixel == maxPixel) ? " (flat - lens cap on, or no real image data)"
                                : "");
}
#endif /* !DEMO_LCD_CAMERA_PREVIEW */

#if !DEMO_USB_STREAM_DISABLE && !DEMO_LCD_CAMERA_PREVIEW
/* Everything below, down to DEMO_CaptureFramesAtMidVoltage(), only exists
 * for the abandoned USB-streaming path. */

/* Frames to capture at Mid voltage before switching to Overdrive/USB -
 * more than 1 so auto-exposure/gain can converge. */
#define DEMO_MID_VOLTAGE_WARMUP_FRAMES 10U

/* How long to stay at Overdrive/streaming before dropping back to Mid for
 * a fresh frame. Confirmed stable at 5000ms on real hardware. */
#define DEMO_OVERDRIVE_HOLD_MS 5000U

/* Capture DEMO_MID_VOLTAGE_WARMUP_FRAMES frames at Mid voltage, log the
 * last one, then stop SmartDMA. Caller must already be at DCDC Mid. */
static void DEMO_CaptureFramesAtMidVoltage(void) {
  uint16_t *frame = NULL;
  uint32_t frameNumber = 0U;
  uint32_t startCount = CAMERA_CAPTURE_GetFrameCount();

  while ((CAMERA_CAPTURE_GetFrameCount() - startCount) <
         DEMO_MID_VOLTAGE_WARMUP_FRAMES) {
    if (CAMERA_CAPTURE_IsFrameReady()) {
      CAMERA_CAPTURE_ClearFrameReady();
      frame = CAMERA_CAPTURE_GetFrameBuffer();
      frameNumber = CAMERA_CAPTURE_GetFrameCount();
    }
  }
  CAMERA_CAPTURE_LogFrameSignature(frameNumber, frame);
  PRINTF("Camera: capture stopped after frame #%u.\r\n", frameNumber);
  CAMERA_CAPTURE_Deinit();
}
#endif /* !DEMO_USB_STREAM_DISABLE && !DEMO_LCD_CAMERA_PREVIEW */

int main(void) {
  BOARD_InitHardware();

  PRINTF("\r\nCamera_AI_Test1 - FRDM-MCXN947\r\n");
  PRINTF("Camera: OV7670 on J9 SmartDMA/Camera header\r\n");

#if DEMO_LCD_CAMERA_PREVIEW
  /* Lens-focus diagnostic (-DLCD_CAMERA_PREVIEW=ON): no AI, no status
   * text - raw camera feed pushed straight to the LCD as fast as frames
   * arrive, for focusing by eye. Camera resolution matches the panel 1:1.
   *
   * TEARING FIX (see WORKLOG.md): once the camera clock was fixed to run
   * at its real ~30fps, it became faster than the ~57ms LCD push and
   * could overwrite the frame buffer mid-push, tearing the image. Fixed
   * like the AI loop below: stop SmartDMA before reading the buffer,
   * restart after, discard the first frame post-restart (SmartDMA needs
   * one cycle to resync with the sensor). Real double-buffering isn't
   * RAM-feasible here (a second frame buffer doesn't fit in either bank). */
  PRINTF("Display: raw camera preview on LCD, no AI (LCD_CAMERA_PREVIEW=ON) "
         "- for focusing the lens\r\n\r\n");

  CAMERA_CAPTURE_Init();
  LCD_Init();
  DEMO_ClearScreen(0x0000U);

  /* DWT cycle counter for a measured fps readout once/sec. */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  uint32_t fpsFrameCount = 0U;
  uint32_t fpsWindowStartCycle = DWT->CYCCNT;

  /* Also tracks time spent waiting for the next frame, to see how much of
   * the frame period is the LCD push vs. waiting on the camera. */
  uint32_t diagWaitCycles = 0U;
  uint32_t diagWaitStartCycle = DWT->CYCCNT;

  /* Set right after CAMERA_CAPTURE_Reinit(), cleared once the next frame
   * is consumed - see the tearing-fix comment above. */
  bool skipNextFrame = false;

  while (1) {
    if (CAMERA_CAPTURE_IsFrameReady()) {
      diagWaitCycles += (DWT->CYCCNT - diagWaitStartCycle);
      CAMERA_CAPTURE_ClearFrameReady();

      if (skipNextFrame) {
        skipNextFrame = false;
        diagWaitStartCycle = DWT->CYCCNT;
        continue;
      }

      /* Stop SmartDMA before reading the frame buffer, restart after -
       * tearing fix, see above. */
      CAMERA_CAPTURE_Deinit();
      LCD_DrawImage(0U, 0U, DEMO_BUFFER_WIDTH, DEMO_BUFFER_HEIGHT,
                    CAMERA_CAPTURE_GetFrameBuffer());
      CAMERA_CAPTURE_Reinit();
      skipNextFrame = true;

      fpsFrameCount++;
      diagWaitStartCycle = DWT->CYCCNT;

      /* (int32_t) cast makes this wrap-safe across DWT->CYCCNT rollover. */
      if ((int32_t)(DWT->CYCCNT - fpsWindowStartCycle) >= (int32_t)SystemCoreClock) {
        PRINTF("LCD preview: %u fps (wait-for-frame=%uus/frame avg)\r\n",
               fpsFrameCount,
               (unsigned)(diagWaitCycles / (SystemCoreClock / 1000000U) /
                          (fpsFrameCount == 0U ? 1U : fpsFrameCount)));
        fpsFrameCount = 0U;
        diagWaitCycles = 0U;
        fpsWindowStartCycle = DWT->CYCCNT;
      }
    }
  }
#else

#if DEMO_USB_STREAM_DISABLE
#if DEMO_LCD_ARDUINO_HEADER
  PRINTF("Display: Arduino-header LCD status text (camera + AI hook)\r\n\r\n");
#else
  PRINTF("Display: J8 LCD status text (camera + AI hook)\r\n\r\n");
#endif
#else
  PRINTF("Display: USB Video Class (UVC) webcam over USB High-Speed "
         "(abandoned path, time-multiplexed - see WORKLOG.md)\r\n\r\n");
#endif

  CAMERA_CAPTURE_Init();
  AI_MODEL_Init();

#if DEMO_USB_STREAM_DISABLE
  /* Default build: camera + AI loop, continuous, DCDC stays at Mid the
   * whole time. LCD shows fixed text status lines - see file-level
   * comment above. */
  LCD_Init();
  DEMO_ClearScreen(0x0000U);

  /* SD card snapshot-on-face-detection - see source/storage/snapshot.c.
   * Safe to call with no card present (SNAPSHOT_OnFrame() just no-ops). */
  SNAPSHOT_Init();

#if !DEMO_AI_MODEL_USE_NPU
  /* CPU-path scratch pool - overflow area for ei_sramx_alloc.c once the
   * primary 96KB m_sramx pool is exhausted. Must alone be big enough for
   * the whole tensor arena (currently 112,460 bytes): a single allocation
   * can't span m_sramx and m_data, they're non-contiguous banks. Only
   * used by the CPU/CMSIS-NN build - the NPU build has its own static
   * arena instead. See model_runner_npu.cpp for the matching NPU budget. */
  static uint8_t s_aiScratchPool[120U * 1024U] __attribute__((aligned(16)));
  EI_SRAMX_SetOverflowPool(s_aiScratchPool, sizeof(s_aiScratchPool));
#endif /* !DEMO_AI_MODEL_USE_NPU */

  /* Set right after CAMERA_CAPTURE_Reinit(), cleared once the next frame
   * is consumed - SmartDMA needs one cycle to resync with the sensor
   * after a fresh Reinit(), so the very next frame isn't trustworthy. */
  bool skipNextFrame = false;

  while (1) {
    if (CAMERA_CAPTURE_IsFrameReady()) {
      CAMERA_CAPTURE_ClearFrameReady();

      if (skipNextFrame) {
        skipNextFrame = false;
        continue;
      }

      uint16_t *frame = CAMERA_CAPTURE_GetFrameBuffer();
      uint32_t frameNumber = CAMERA_CAPTURE_GetFrameCount();

      /* Every 15th frame (~2x/sec at 30fps) so the log stays readable. */
      if ((frameNumber % 15U) == 1U) {
        CAMERA_CAPTURE_LogFrameSignature(frameNumber, frame);
      }

      /* Stop SmartDMA before inference: SmartDMA's own working RAM
       * (0x04000000) is the same bank as the AI tensor arena - leaving it
       * running while the arena is used corrupts SmartDMA's state (see
       * WORKLOG.md). */
      CAMERA_CAPTURE_Deinit();

      ai_model_result_t aiResult;
      AI_MODEL_RunInference(frame, DEMO_BUFFER_WIDTH, DEMO_BUFFER_HEIGHT,
                            &aiResult);

      /* Still before Reinit(): draws into/reads `frame` directly, so must
       * run while the buffer is stable. Internally rate-limited to at
       * most 1 capture/sec - a no-op most frames. */
      SNAPSHOT_OnFrame(frame, DEMO_BUFFER_WIDTH, DEMO_BUFFER_HEIGHT,
                       &aiResult, AI_MODEL_GetInputWidth(), AI_MODEL_GetInputHeight());

      CAMERA_CAPTURE_Reinit();
      skipNextFrame = true;

      bool sawFace = false;
      if (aiResult.valid) {
        for (uint32_t i = 0; i < aiResult.boxCount; i++) {
          const ai_bbox_t *box = &aiResult.boxes[i];
          if (strcmp(box->label, "face") == 0) {
            sawFace = true;
          }
          /* debug_console_lite may not support %f, so print score as a
           * percentage integer. */
          PRINTF("AI result: box[%u] label=%s x=%u y=%u w=%u h=%u score=%d%%\r\n",
                 i, box->label, box->x, box->y, box->width, box->height,
                 (int)(box->score * 100.0f));
        }
      }

      DEMO_DrawStatusLine(0U, "FACE      ", sawFace);

      /* Stays lit for the same window SNAPSHOT_OnFrame() rate-limits
       * captures to, so it clears itself as a new capture becomes
       * possible again. */
      DEMO_DrawStatusLine(1U, "CAPTURE   ", SNAPSHOT_IsNoticeActive());
    }
  }
#else
  /* USB streaming build: time-multiplexed. Mid-voltage capture first -
   * wait for settled frames, then stop SmartDMA (capture and USB HS can't
   * run at the same time on this chip). */
  DEMO_CaptureFramesAtMidVoltage();
  PRINTF("Camera: switching to Overdrive for USB.\r\n");

  ai_model_result_t aiResult;
  AI_MODEL_RunInference(CAMERA_CAPTURE_GetFrameBuffer(), DEMO_BUFFER_WIDTH,
                        DEMO_BUFFER_HEIGHT, &aiResult);
  if (aiResult.valid) {
    for (uint32_t i = 0; i < aiResult.boxCount; i++) {
      const ai_bbox_t *box = &aiResult.boxes[i];
      /* debug_console_lite may not support %f, so print score as a
       * percentage integer. */
      PRINTF("AI result: box[%u] label=%s x=%u y=%u w=%u h=%u score=%d%%\r\n",
             i, box->label, box->x, box->y, box->width, box->height,
             (int)(box->score * 100.0f));
    }
  }

  /* Full PHY/PLL bring-up + enumeration runs exactly ONCE here - the
   * periodic refresh loop below only calls the lighter regulator helpers. */
  USB_DeviceClockInit();
  USB_VideoCamera_Init();

  while (1) {
    USB_VideoCamera_Task();

    /* PERIODIC REFRESH - see DEMO_OVERDRIVE_HOLD_MS above. */
    SDK_DelayAtLeastUs(DEMO_OVERDRIVE_HOLD_MS * 1000U,
                       SDK_DEVICE_MAXIMUM_CPU_CLOCK_FREQUENCY);

    PRINTF("Camera: refreshing - dropping to Mid voltage to recapture.\r\n");
    BOARD_SetRegulatorsMidVoltage();
    CAMERA_CAPTURE_Reinit();
    DEMO_CaptureFramesAtMidVoltage();
    BOARD_SetRegulatorsOverdriveVoltage();
    PRINTF("Camera: back to Overdrive, streaming the new frame.\r\n");
  }
#endif
#endif /* DEMO_LCD_CAMERA_PREVIEW */
}
