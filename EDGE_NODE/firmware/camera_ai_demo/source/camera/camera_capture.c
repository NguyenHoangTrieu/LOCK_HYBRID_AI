/*
 * camera_capture.c - see camera_capture.h
 *
 * Adapted from the camera portion of NXP's
 * `display_examples/smartdma_camera_flexio_mculcd` example.
 */

#include "camera_capture.h"
#include "app.h"
#include "board.h"
#include "fsl_debug_console.h"
#include "fsl_ov7670.h"
#include "fsl_sccb.h"
#include "fsl_smartdma.h"
#include "fsl_smartdma_prv.h"
#include <string.h>

/* OV7670 MVFP register (mirror/vflip) - flips the image at the sensor
 * itself. Not exposed as a public function in fsl_ov7670.c, so this talks
 * to the register directly via SCCB_ModifyReg().
 *
 * fsl_ov7670.c's OV7670_CameraInit() (called from CAMERA_DEVICE_Init()
 * below) turns MIRROR on by default every boot - DEMO_CAMERA_MIRROR
 * defaults to 1 here to match that. Override either bit to see the effect
 * on-screen without touching LCD-side code. */
#define OV7670_SCCB_I2C_ADDR 0x21U
#define OV7670_MVFP_REG 0x1EU
#define OV7670_MVFP_MIRROR_MASK 0x20U /* horizontal mirror (left/right) */
#define OV7670_MVFP_FLIP_MASK 0x10U   /* vertical flip (top/bottom) */

#ifndef DEMO_CAMERA_MIRROR
#define DEMO_CAMERA_MIRROR 0U
#endif
#ifndef DEMO_CAMERA_VFLIP
#define DEMO_CAMERA_VFLIP 0U
#endif

static const ov7670_resource_t s_cameraResource = {
    .i2cSendFunc = BOARD_Camera_I2C_SendSCCB,
    .i2cReceiveFunc = BOARD_Camera_I2C_ReceiveSCCB,
    .xclock = kOV7670_InputClock24MHZ,
};

static camera_device_handle_t s_cameraHandle = {
    .resource = (void *)&s_cameraResource,
    .ops = &ov7670_ops,
};

static volatile bool s_frameReady = false;
static volatile uint32_t s_frameCount = 0;

#ifdef DUALCORE_RTOS
/* A 320x240 RGB565 frame (153,600 bytes) doesn't fit in core1's own
 * ~43KB RAM region at all - lives in the shared RAM region instead
 * (ipc_layout.h), carved out of core0's much larger m_data. core1 just
 * points at the fixed address directly. */
#include "ipc_layout.h"
#define s_frameBuffer ((uint16_t *)IPC_FRAME_BUFFER_ADDR)
#else
static uint16_t s_frameBuffer[DEMO_BUFFER_WIDTH * DEMO_BUFFER_HEIGHT];
#endif

/* Must be >= 64 bytes per fsl_smartdma_fw.h's own documented minimum -
 * this was once only 32, which caused a real stack overflow once
 * SmartDMA started rebooting every frame (see WORKLOG.md). Sized to 128
 * for real margin, not the bare minimum. */
static uint8_t s_smartdmaStack[128];

static void CAMERA_CAPTURE_CompleteCallback(void *param) {
  (void)param;
  s_frameReady = true;
  s_frameCount++;
}

static void CAMERA_CAPTURE_InitDevice(void) {
  camera_config_t camConfig = {
      .pixelFormat = kVIDEO_PixelFormatRGB565,
      .resolution = DEMO_CAMERA_RESOLUTION,
      .framePerSec = 30,
      .interface = kCAMERA_InterfaceGatedClock,
      .frameBufferLinePitch_Bytes = 0,
      .controlFlags = 0,
      .bytesPerPixel = 0,
      .mipiChannel = 0,
      .csiLanes = 0,
  };

  BOARD_Camera_I2C_Init();

  /* CAMERA_DEVICE_Init() -> OV7670_Init() reads back PID/VER over SCCB
   * and checks them against the fixed OV7670 values (0x76/0x73), so
   * success here confirms the camera is alive and answering on J9. */
  if (CAMERA_DEVICE_Init(&s_cameraHandle, &camConfig) != kStatus_Success) {
    PRINTF("Camera: OV7670 NOT detected on J9 SCCB (PID/VER register\r\n");
    PRINTF("  readback failed or mismatched) - check J9 wiring/rework\r\n");
    PRINTF("  (SJ16/SJ26/SJ27) in README.md.\r\n");
    while (1) {
    }
  }

  PRINTF("Camera: OV7670 detected on J9 (PID=0x76 VER=0x73 confirmed), %ux%u @ "
         "%u fps.\r\n",
         DEMO_BUFFER_WIDTH, DEMO_BUFFER_HEIGHT, camConfig.framePerSec);

  /* Default light mode (OV7670_LIGHT_MODE_DISABLED) under-compensates for
   * indoor artificial light (R>>B even with AWB enabled) - HOME is the
   * vendor preset for indoor/incandescent light. Try OFFICE if this still
   * isn't enough for your lighting. */
  (void)OV7670_LightMode(&s_cameraHandle, &OV7670_LIGHT_MODE_HOME);

  /* Set mirror/vflip explicitly, after OV7670_LightMode() so it's the
   * last word on MVFP - overrides the hidden mirror-on default above. */
  {
    uint8_t mvfpValue = 0U;
#if DEMO_CAMERA_MIRROR
    mvfpValue |= OV7670_MVFP_MIRROR_MASK;
#endif
#if DEMO_CAMERA_VFLIP
    mvfpValue |= OV7670_MVFP_FLIP_MASK;
#endif
    (void)SCCB_ModifyReg(
        OV7670_SCCB_I2C_ADDR, kSCCB_RegAddr8Bit, OV7670_MVFP_REG,
        OV7670_MVFP_MIRROR_MASK | OV7670_MVFP_FLIP_MASK, mvfpValue,
        s_cameraResource.i2cReceiveFunc, s_cameraResource.i2cSendFunc);
  }
}

static void CAMERA_CAPTURE_InitSmartDma(void) {
  static smartdma_camera_param_t smartdmaParam;

  /* sizeof(s_frameBuffer) would be wrong in the DUALCORE_RTOS build - that
   * name is a pointer-valued macro there (see the declaration above), not
   * an array, so sizeof() would only cover the pointer itself. */
  memset((void *)s_frameBuffer, 0, (size_t)DEMO_BUFFER_WIDTH * DEMO_BUFFER_HEIGHT * sizeof(uint16_t));

  SMARTDMA_InitWithoutFirmware();
  SMARTDMA_InstallFirmware(SMARTDMA_CAMERA_MEM_ADDR, s_smartdmaCameraFirmware,
                           SMARTDMA_CAMERA_FIRMWARE_SIZE);
  SMARTDMA_InstallCallback(CAMERA_CAPTURE_CompleteCallback, NULL);
  NVIC_EnableIRQ(SMARTDMA_IRQn);
  NVIC_SetPriority(SMARTDMA_IRQn, 3);

  smartdmaParam.smartdma_stack = (uint32_t *)s_smartdmaStack;
  smartdmaParam.p_buffer = (uint32_t *)s_frameBuffer;
  SMARTDMA_Boot(DEMO_SMARTDMA_API, &smartdmaParam, 0x2);
}

void CAMERA_CAPTURE_Init(void) {
  CAMERA_CAPTURE_InitDevice();
  CAMERA_CAPTURE_InitSmartDma();
}

void CAMERA_CAPTURE_Deinit(void) {
  NVIC_DisableIRQ(SMARTDMA_IRQn);
  SMARTDMA_Deinit();
}

void CAMERA_CAPTURE_Reinit(void) { CAMERA_CAPTURE_InitSmartDma(); }

bool CAMERA_CAPTURE_IsFrameReady(void) { return s_frameReady; }

void CAMERA_CAPTURE_ClearFrameReady(void) { s_frameReady = false; }

uint16_t *CAMERA_CAPTURE_GetFrameBuffer(void) { return s_frameBuffer; }

uint32_t CAMERA_CAPTURE_GetFrameCount(void) { return s_frameCount; }
