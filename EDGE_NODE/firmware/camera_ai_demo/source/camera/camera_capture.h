/*
 * camera_capture.h - OV7670 capture via SmartDMA, J9 SmartDMA/Camera header.
 *
 * Thin wrapper around the camera + SmartDMA driver combination from NXP's
 * `display_examples/smartdma_camera_flexio_mculcd` example.
 */
#ifndef _CAMERA_CAPTURE_H_
#define _CAMERA_CAPTURE_H_

#include <stdint.h>
#include <stdbool.h>

/*! @brief Init camera (SCCB/I2C + OV7670 registers) and boot the SmartDMA capture firmware. */
void CAMERA_CAPTURE_Init(void);

/*! @brief Stop the SmartDMA capture engine and gate its clock. The last frame stays intact in
 * the buffer. Must be called (capture fully stopped) before switching DCDC away from Mid for
 * USB HS - SmartDMA only runs reliably at DCDC Mid, see WORKLOG.md. */
void CAMERA_CAPTURE_Deinit(void);

/*! @brief Restart SmartDMA capture after CAMERA_CAPTURE_Deinit(), without re-running OV7670
 * SCCB/I2C init (sensor doesn't need reconfiguring, only the pixel pipe does). Caller must be
 * at DCDC Mid voltage before calling this. */
void CAMERA_CAPTURE_Reinit(void);

/*! @brief True once a new frame has landed in the capture buffer since the last call. */
bool CAMERA_CAPTURE_IsFrameReady(void);

/*! @brief Clear the "frame ready" flag after consuming the buffer. */
void CAMERA_CAPTURE_ClearFrameReady(void);

/*! @brief RGB565 frame buffer, DEMO_BUFFER_WIDTH x DEMO_BUFFER_HEIGHT pixels (see app.h). */
uint16_t *CAMERA_CAPTURE_GetFrameBuffer(void);

/*! @brief Total frames captured since CAMERA_CAPTURE_Init(), for liveness logging. */
uint32_t CAMERA_CAPTURE_GetFrameCount(void);

#endif /* _CAMERA_CAPTURE_H_ */
