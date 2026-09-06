/*
 * model_runner.h - AI inference API.
 *
 * Small, framework-agnostic C API that main.c calls once per captured
 * frame. Backed by an Edge Impulse FOMO object-detection model
 * (source/ai/edge_impulse/ - "Face_Detection_NXP" project, single class
 * `face`) - see model_runner.cpp for the run_classifier() integration
 * (or model_runner_npu.cpp for the Neutron NPU backend).
 */
#ifndef _MODEL_RUNNER_H_
#define _MODEL_RUNNER_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AI_MODEL_MAX_BOXES 10U

typedef struct
{
    const char *label; /* one of the model's class labels, currently just "face" */
    uint16_t x, y;      /* top-left corner, in AI_MODEL_GetInputWidth/Height() space */
    uint16_t width, height;
    float score; /* 0..1 confidence */
} ai_bbox_t;

typedef struct
{
    bool valid; /* true if at least one box was detected */
    uint32_t boxCount;
    ai_bbox_t boxes[AI_MODEL_MAX_BOXES];
} ai_model_result_t;

/*! @brief One-time setup (allocate tensor arena, load model, etc). */
void AI_MODEL_Init(void);

/*!
 * @brief Run inference on one RGB565 frame.
 *
 * @param pixels RGB565 frame, `width * height` pixels.
 * @param width  frame width in pixels.
 * @param height frame height in pixels.
 * @param result output detection result (bounding boxes).
 */
void AI_MODEL_RunInference(const uint16_t *pixels, uint16_t width, uint16_t height, ai_model_result_t *result);

/*! @brief Model's own input resolution - bbox coords in ai_model_result_t are
 *  in this space, not the camera's; scale by DEMO_BUFFER_WIDTH/HEIGHT
 *  divided by these before drawing on the (320x240) camera frame. */
uint16_t AI_MODEL_GetInputWidth(void);
uint16_t AI_MODEL_GetInputHeight(void);

#ifdef __cplusplus
}
#endif

#endif /* _MODEL_RUNNER_H_ */
