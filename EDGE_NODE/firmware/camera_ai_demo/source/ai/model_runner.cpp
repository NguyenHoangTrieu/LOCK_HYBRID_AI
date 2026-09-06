/*
 * model_runner.cpp - Edge Impulse integration.
 *
 * Wraps the exported FOMO impulse behind the plain-C API in
 * model_runner.h so main.c doesn't touch the Edge Impulse SDK directly.
 * Dimensions/labels come from EI_CLASSIFIER_* macros in
 * edge_impulse/model-parameters/model_metadata.h - this file is
 * model-agnostic.
 *
 * signal.total_length must be the MODEL's input pixel count
 * (EI_CLASSIFIER_INPUT_WIDTH*HEIGHT), not the camera's raw frame size -
 * extract_image_features() doesn't resize, it reads exactly that many
 * elements via get_data(). Passing the wrong (larger) length here once
 * caused a real buffer overflow that only HardFaulted once it reached the
 * end of m_sramx (see WORKLOG.md). The resize itself (squash, independent
 * per axis) is this callback's job: map each target pixel back to the
 * nearest source camera pixel.
 */
#include "model_runner.h"
#include "fsl_debug_console.h"
#include "fsl_common.h"

/* NOT including ei_run_classifier.h/ei_run_classifier_c.h - both pull in
 * function bodies meant for exactly one translation unit
 * (ei_run_classifier_c.cpp), causing ODR link errors if included here
 * too. ei_classifier_types.h is the lightweight, header-only one.
 * ei_run_classifier() is forward-declared by hand below instead, to link
 * against the real symbol without re-including its implementation. */
#include "edge_impulse/edge-impulse-sdk/classifier/ei_classifier_types.h"
#include "edge_impulse/edge-impulse-sdk/dsp/returntypes.h"

extern "C" EI_IMPULSE_ERROR ei_run_classifier(ei::signal_t *signal, ei_impulse_result_t *result, bool debug);

#include "ei_sramx_alloc.h"

static const uint16_t *s_pixels;
static uint16_t s_srcWidth;
static uint16_t s_srcHeight;

/* Resize (nearest-neighbor squash, source 320x240 -> target 64x64) +
 * RGB565 -> packed 0xRRGGBB float, the pixel format Edge Impulse's image
 * DSP block expects from a camera signal callback. `offset`/`length` here
 * are indices into the TARGET (model input) pixel grid, per the
 * signal.total_length note above - not the source camera frame. */
static int get_signal_data(size_t offset, size_t length, float *out_ptr)
{
    const uint16_t dstWidth = EI_CLASSIFIER_INPUT_WIDTH;
    const uint16_t dstHeight = EI_CLASSIFIER_INPUT_HEIGHT;

    for (size_t i = 0; i < length; i++)
    {
        size_t dstIndex = offset + i;
        uint16_t tx = (uint16_t)(dstIndex % dstWidth);
        uint16_t ty = (uint16_t)(dstIndex / dstWidth);
        uint16_t sx = (uint16_t)(((uint32_t)tx * s_srcWidth) / dstWidth);
        uint16_t sy = (uint16_t)(((uint32_t)ty * s_srcHeight) / dstHeight);

        uint16_t px = s_pixels[(uint32_t)sy * s_srcWidth + sx];
        uint8_t r = (uint8_t)(((px >> 11) & 0x1FU) << 3);
        uint8_t g = (uint8_t)(((px >> 5) & 0x3FU) << 2);
        uint8_t b = (uint8_t)((px & 0x1FU) << 3);
        out_ptr[i] = (float)(((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
    }
    return 0;
}

/* This SDK's clib porting layer hard-codes ei_read_timer_us() to return
 * 0 (not overridable), so ei_result.timing is always zero here. Timing
 * the whole call by hand instead via the DWT cycle counter. */
static void AI_MODEL_InitTiming(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

extern "C" void AI_MODEL_Init(void)
{
    AI_MODEL_InitTiming();
    PRINTF("AI_MODEL_Init: Edge Impulse face-detection FOMO ready (%dx%d input, %d class(es))\r\n",
           EI_CLASSIFIER_INPUT_WIDTH, EI_CLASSIFIER_INPUT_HEIGHT, EI_CLASSIFIER_LABEL_COUNT);
}

extern "C" void AI_MODEL_RunInference(const uint16_t *pixels, uint16_t width, uint16_t height,
                                       ai_model_result_t *result)
{
    s_pixels = pixels;
    s_srcWidth = width;
    s_srcHeight = height;

    ei::signal_t signal;
    signal.total_length = (size_t)EI_CLASSIFIER_INPUT_WIDTH * (size_t)EI_CLASSIFIER_INPUT_HEIGHT;
    signal.get_data = &get_signal_data;

    /* run_classifier() allocates the tensor arena + a couple of small aux
     * buffers via ei_calloc/ei_malloc (see ei_sramx_alloc.c) and frees them
     * before returning - reset the pool fresh for each call rather than
     * relying on ei_free() (a no-op in our allocator). */
    EI_SRAMX_PoolReset();

    ei_impulse_result_t ei_result = {0};
    uint32_t cycStart = DWT->CYCCNT;
    EI_IMPULSE_ERROR err = ei_run_classifier(&signal, &ei_result, false /* debug */);
    uint32_t elapsedCycles = DWT->CYCCNT - cycStart;
    uint32_t elapsedUs = (uint32_t)(((uint64_t)elapsedCycles * 1000000ULL) / SystemCoreClock);
    PRINTF("AI_MODEL_RunInference: total classifier time = %uus (%ums)\r\n",
           (unsigned)elapsedUs, (unsigned)(elapsedUs / 1000U));

    result->valid = false;
    result->boxCount = 0;

    if (err != EI_IMPULSE_OK)
    {
        PRINTF("AI_MODEL_RunInference: ei_run_classifier failed (%d), combined high-water=%u bytes\r\n",
               (int)err, (unsigned)EI_SRAMX_GetHighWaterMark());
        return;
    }

    uint32_t n = ei_result.bounding_boxes_count;
    if (n > AI_MODEL_MAX_BOXES)
    {
        n = AI_MODEL_MAX_BOXES;
    }
    for (uint32_t i = 0; i < n; i++)
    {
        ei_impulse_result_bounding_box_t *bb = &ei_result.bounding_boxes[i];
        result->boxes[i].label = bb->label;
        result->boxes[i].x = (uint16_t)bb->x;
        result->boxes[i].y = (uint16_t)bb->y;
        result->boxes[i].width = (uint16_t)bb->width;
        result->boxes[i].height = (uint16_t)bb->height;
        result->boxes[i].score = bb->value;
    }
    result->boxCount = n;
    result->valid = (n > 0U);
}

extern "C" uint16_t AI_MODEL_GetInputWidth(void)
{
    return EI_CLASSIFIER_INPUT_WIDTH;
}

extern "C" uint16_t AI_MODEL_GetInputHeight(void)
{
    return EI_CLASSIFIER_INPUT_HEIGHT;
}
