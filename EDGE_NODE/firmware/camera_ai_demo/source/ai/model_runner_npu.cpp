/*
 * model_runner_npu.cpp - alternate model_runner.h implementation using
 * this chip's Neutron NPU instead of the CPU+CMSIS-NN path in
 * model_runner.cpp. Selected at build time by AI_MODEL_USE_NPU.
 *
 * Model: neutron/ultraface_slim_ultraslim_npu16_tflite.h, an UltraFace UltraSlim
 * face detector (128x128x3 INT8 input), converted for NXP Neutron NPU.
 * Output tensor: [1, 172, 6] INT8 representing 172 prior anchors:
 * [no_face_score, face_score, left, top, right, bottom].
 * Post-processing: score thresholding + IoU Non-Maximum Suppression (NMS).
 */
#include "model_runner.h"
#include "fsl_debug_console.h"
#include "fsl_common.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/kernels/neutron/neutron.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "neutron/ultraface_slim_ultraslim_npu16_tflite.h"
#include "neutron/ultraface_slim_ultraslim_npu16_tflite_info.h"
#include "neutron/couple_COCO_128_128_rgb.h"

#define NPU_MODEL_INPUT_WIDTH  128U
#define NPU_MODEL_INPUT_HEIGHT 128U
#define NPU_MODEL_NUM_PRIORS   172U
#define NPU_MODEL_DETECTION_THRESHOLD 0.58f
#define NPU_MODEL_NMS_THRESHOLD       0.45f

constexpr int kTensorArenaSize = 244 * 1024;
static uint8_t s_tensorArena[kTensorArenaSize] __attribute__((aligned(16)));

static const tflite::Model *s_model = nullptr;
static tflite::MicroInterpreter *s_interpreter = nullptr;

typedef struct
{
    float score;
    float xmin, ymin, xmax, ymax;
} candidate_box_t;

#define MAX_CANDIDATES 32
static candidate_box_t s_candidates[MAX_CANDIDATES];
static uint32_t s_candidateCount;

static void AI_MODEL_InitTiming(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static int8_t s_inputLut[256];

static void InitInputLut(float scale, int32_t zp)
{
    float stdVal = (float)ULTRAFACE_INPUT_STD;
    float meanVal = (float)ULTRAFACE_INPUT_MEAN;
    for (int i = 0; i < 256; i++)
    {
        float real = ((float)i - meanVal) / stdVal;
        int32_t q = (int32_t)roundf(real / scale) + zp;
        if (q < -128) q = -128;
        if (q > 127) q = 127;
        s_inputLut[i] = (int8_t)q;
    }
}

extern "C" void AI_MODEL_Init(void)
{
    AI_MODEL_InitTiming();

    s_model = tflite::GetModel(ultraface_data);
    if (s_model->version() != TFLITE_SCHEMA_VERSION)
    {
        PRINTF("AI_MODEL_Init: NPU model schema version %d != supported %d\r\n", (int)s_model->version(),
               TFLITE_SCHEMA_VERSION);
        return;
    }

    static tflite::MicroMutableOpResolver<12> s_opResolver;
    s_opResolver.AddCustom(tflite::GetString_NEUTRON_GRAPH(), tflite::Register_NEUTRON_GRAPH());
    s_opResolver.AddPad();
    s_opResolver.AddConcatenation();
    s_opResolver.AddAdd();
    s_opResolver.AddRelu();
    s_opResolver.AddSub();
    s_opResolver.AddSlice();
    s_opResolver.AddSoftmax();
    s_opResolver.AddQuantize();
    s_opResolver.AddDequantize();
    s_opResolver.AddMul();
    s_opResolver.AddExp();

    static tflite::MicroInterpreter s_staticInterpreter(s_model, s_opResolver, s_tensorArena, kTensorArenaSize);
    s_interpreter = &s_staticInterpreter;

    if (s_interpreter->AllocateTensors() != kTfLiteOk)
    {
        PRINTF("AI_MODEL_Init: AllocateTensors() failed - kTensorArenaSize (%d bytes) is too small\r\n",
               kTensorArenaSize);
        s_interpreter = nullptr;
        return;
    }

    TfLiteTensor *inT = s_interpreter->input(0);
    TfLiteTensor *outT = s_interpreter->output(0);
    PRINTF("AI_MODEL_Init: UltraFace UltraSlim NPU face detector ready (%dx%d input, arena used %u/%u bytes)\r\n",
           NPU_MODEL_INPUT_WIDTH, NPU_MODEL_INPUT_HEIGHT,
           (unsigned)s_interpreter->arena_used_bytes(), (unsigned)kTensorArenaSize);
    PRINTF("AI_MODEL_Init: in scale=%d/10000 zp=%d | out scale=%d/10000 zp=%d\r\n",
           inT ? (int)(inT->params.scale * 10000.0f) : 0, inT ? inT->params.zero_point : 0,
           outT ? (int)(outT->params.scale * 10000.0f) : 0, outT ? outT->params.zero_point : 0);

    /* Self-test with known MS COCO couple test image */
    InitInputLut(inT->params.scale <= 0.0f ? 0.0078125f : inT->params.scale, inT->params.zero_point);
    int8_t *inData = inT->data.int8;
    for (size_t i = 0; i < 128U * 128U * 3U; i++)
    {
        inData[i] = s_inputLut[couple_COCO_128_128_rgb_data[i]];
    }
    TfLiteStatus testInv = s_interpreter->Invoke();
    const int8_t *testOut = outT->data.int8;
    float testScale = outT->params.scale <= 0.0f ? ULTRAFACE_OUTPUT_SCALE : outT->params.scale;
    int32_t testZp = outT->params.scale <= 0.0f ? (int32_t)ULTRAFACE_OUTPUT_ZERO_POINT : outT->params.zero_point;
    float testMaxScore = -1.0f;
    uint32_t testFound = 0;
    int maxAnchor = -1;
    for (uint32_t i = 0; i < NPU_MODEL_NUM_PRIORS; i++)
    {
        int8_t sc_q = testOut[i * 6U + 1U];
        float sc = (float)(sc_q - testZp) * testScale;
        if (sc > testMaxScore)
        {
            testMaxScore = sc;
            maxAnchor = (int)i;
        }
        if (sc >= 0.50f)
        {
            testFound++;
        }
    }
    PRINTF("AI_MODEL_Init: COCO couple self-test -> invoke=%d, maxScore=%d%% (anchor=%d), faces>=50%%: %u\r\n",
           (int)testInv, (int)(testMaxScore * 100.0f), maxAnchor, (unsigned)testFound);
    if (maxAnchor >= 0)
    {
        size_t b = (size_t)maxAnchor * 6U;
        PRINTF("  Max anchor [%d]: c0=%d c1=%d | l=%d t=%d r=%d b=%d\r\n",
               maxAnchor, testOut[b], testOut[b+1], testOut[b+2], testOut[b+3], testOut[b+4], testOut[b+5]);
    }
}

static void SampleCropTensor(const uint16_t *pixels, uint16_t width, uint16_t height,
                             int rot, int8_t *inputData,
                             uint16_t *outMidPx, uint8_t *outMidR, uint8_t *outMidG, uint8_t *outMidB)
{
    const uint16_t cropSize = (height < width) ? height : width; /* 240 */
    const uint16_t cropLeft = (width > cropSize) ? ((width - cropSize) / 2U) : 0U; /* 40 */
    const uint16_t cropTop  = (height > cropSize) ? ((height - cropSize) / 2U) : 0U; /* 0 */

    for (uint16_t ty = 0; ty < NPU_MODEL_INPUT_HEIGHT; ty++)
    {
        for (uint16_t tx = 0; tx < NPU_MODEL_INPUT_WIDTH; tx++)
        {
            uint16_t sx, sy;
            switch (rot)
            {
                case 2: /* 180 deg upside-down */
                    sx = cropLeft + (uint16_t)(((uint32_t)(127U - tx) * cropSize) / NPU_MODEL_INPUT_WIDTH);
                    sy = cropTop  + (uint16_t)(((uint32_t)(127U - ty) * cropSize) / NPU_MODEL_INPUT_HEIGHT);
                    break;
                case 1: /* 90 deg CW */
                    sx = cropLeft + (uint16_t)(((uint32_t)ty * cropSize) / NPU_MODEL_INPUT_HEIGHT);
                    sy = cropTop  + (uint16_t)(((uint32_t)(127U - tx) * cropSize) / NPU_MODEL_INPUT_WIDTH);
                    break;
                case 3: /* 270 deg CW / 90 deg CCW */
                    sx = cropLeft + (uint16_t)(((uint32_t)(127U - ty) * cropSize) / NPU_MODEL_INPUT_HEIGHT);
                    sy = cropTop  + (uint16_t)(((uint32_t)tx * cropSize) / NPU_MODEL_INPUT_WIDTH);
                    break;
                case 0: /* 0 deg normal */
                default:
                    sx = cropLeft + (uint16_t)(((uint32_t)tx * cropSize) / NPU_MODEL_INPUT_WIDTH);
                    sy = cropTop  + (uint16_t)(((uint32_t)ty * cropSize) / NPU_MODEL_INPUT_HEIGHT);
                    break;
            }

            uint16_t px = pixels[(uint32_t)sy * width + sx];
            uint8_t r = (uint8_t)(((px >> 11U) & 0x1FU) * 255U / 31U);
            uint8_t g = (uint8_t)(((px >> 5U)  & 0x3FU) * 255U / 63U);
            uint8_t b = (uint8_t)((px & 0x1FU) * 255U / 31U);

            if (rot == 0 && ty == (NPU_MODEL_INPUT_HEIGHT / 2U) && tx == (NPU_MODEL_INPUT_WIDTH / 2U))
            {
                if (outMidPx) *outMidPx = px;
                if (outMidR)  *outMidR = r;
                if (outMidG)  *outMidG = g;
                if (outMidB)  *outMidB = b;
            }

            size_t base = ((size_t)ty * NPU_MODEL_INPUT_WIDTH + tx) * 3U;
            inputData[base + 0U] = s_inputLut[r];
            inputData[base + 1U] = s_inputLut[g];
            inputData[base + 2U] = s_inputLut[b];
        }
    }
}

static void MapBoxBack(int rot, float norm_l, float norm_t, float norm_r, float norm_b,
                       uint16_t cropLeft, uint16_t cropTop, uint16_t cropSize,
                       float *out_xmin, float *out_ymin, float *out_xmax, float *out_ymax)
{
    float cs = (float)cropSize;
    float cl = (float)cropLeft;
    float ct = (float)cropTop;
    switch (rot)
    {
        case 2: /* 180 deg */
            *out_xmin = cl + (1.0f - norm_r) * cs;
            *out_ymin = ct + (1.0f - norm_b) * cs;
            *out_xmax = cl + (1.0f - norm_l) * cs;
            *out_ymax = ct + (1.0f - norm_t) * cs;
            break;
        case 1: /* 90 deg CW */
            *out_xmin = cl + norm_t * cs;
            *out_ymin = ct + (1.0f - norm_r) * cs;
            *out_xmax = cl + norm_b * cs;
            *out_ymax = ct + (1.0f - norm_l) * cs;
            break;
        case 3: /* 270 deg / 90 deg CCW */
            *out_xmin = cl + (1.0f - norm_b) * cs;
            *out_ymin = ct + norm_l * cs;
            *out_xmax = cl + (1.0f - norm_t) * cs;
            *out_ymax = ct + norm_r * cs;
            break;
        case 0: /* 0 deg */
        default:
            *out_xmin = cl + norm_l * cs;
            *out_ymin = ct + norm_t * cs;
            *out_xmax = cl + norm_r * cs;
            *out_ymax = ct + norm_b * cs;
            break;
    }
}

extern "C" void AI_MODEL_RunInference(const uint16_t *pixels, uint16_t width, uint16_t height,
                                       ai_model_result_t *result)
{
    result->valid = false;
    result->boxCount = 0;

    if (s_interpreter == nullptr)
    {
        PRINTF("AI_MODEL_RunInference: interpreter not initialized (AllocateTensors() failed earlier)\r\n");
        return;
    }

    TfLiteTensor *inputTensor = s_interpreter->input(0);
    int8_t *inputData = inputTensor->data.int8;

    const uint16_t cropSize = (height < width) ? height : width; /* 240 */
    const uint16_t cropLeft = (width > cropSize) ? ((width - cropSize) / 2U) : 0U; /* 40 */
    const uint16_t cropTop  = (height > cropSize) ? ((height - cropSize) / 2U) : 0U; /* 0 */

    uint8_t midR = 0, midG = 0, midB = 0;
    uint16_t midPx = 0;
    uint32_t totalElapsedUs = 0;

    static uint32_t s_infFrameCount = 0;
    s_infFrameCount++;

    /* Dynamic fast rotation scheduler:
     * Primary physical orientation on J9 is rot=3 (270 deg CW).
     * If s_activeRot finds a face, we stop immediately (16ms).
     * If no face is found, we run at most ONE alternative rotation round-robin.
     * Total inference is 16ms (active) or 32ms max (idle search), keeping FPS >= 8-10. */
    static int s_activeRot = 3; /* 270 deg default */
    static int s_altStep = 0;
    const int allRots[4] = {3, 0, 2, 1};

    int rotsToTry[2];
    int numRotsToTry = 1;
    rotsToTry[0] = s_activeRot;

    /* If no face was found last frame, pick 1 alternative rotation round-robin */
    static bool s_lastHadFace = false;
    if (!s_lastHadFace)
    {
        s_altStep = (s_altStep + 1) % 4;
        if (allRots[s_altStep] != s_activeRot)
        {
            rotsToTry[1] = allRots[s_altStep];
            numRotsToTry = 2;
        }
    }

    int bestRot = s_activeRot;
    float globalMaxScore = -1.0f;
    int globalMaxAnchor = -1;

    for (int rIdx = 0; rIdx < numRotsToTry; rIdx++)
    {
        int rot = rotsToTry[rIdx];
        SampleCropTensor(pixels, width, height, rot, inputData, &midPx, &midR, &midG, &midB);

        uint32_t cycStart = DWT->CYCCNT;
        TfLiteStatus invokeStatus = s_interpreter->Invoke();
        uint32_t elapsedCycles = DWT->CYCCNT - cycStart;
        totalElapsedUs += (uint32_t)(((uint64_t)elapsedCycles * 1000000ULL) / SystemCoreClock);

        if (invokeStatus != kTfLiteOk) continue;

        TfLiteTensor *outputTensor = s_interpreter->output(0);
        const int8_t *outputData = outputTensor->data.int8;
        float outScale = outputTensor->params.scale <= 0.0f ? ULTRAFACE_OUTPUT_SCALE : outputTensor->params.scale;
        int32_t outZeroPoint = outputTensor->params.scale <= 0.0f ? (int32_t)ULTRAFACE_OUTPUT_ZERO_POINT : outputTensor->params.zero_point;

        s_candidateCount = 0;
        float rotMaxScore = -1.0f;
        int rotMaxAnchor = -1;
        for (uint32_t i = 0; i < NPU_MODEL_NUM_PRIORS; i++)
        {
            size_t base = i * 6U;
            int8_t face_score_q = outputData[base + 1U];
            float score = (float)(face_score_q - outZeroPoint) * outScale;
            if (score > rotMaxScore)
            {
                rotMaxScore = score;
                rotMaxAnchor = (int)i;
            }
            if (score >= NPU_MODEL_DETECTION_THRESHOLD)
            {
                float norm_left   = ((float)(outputData[base + 2U] - outZeroPoint) * outScale);
                float norm_top    = ((float)(outputData[base + 3U] - outZeroPoint) * outScale);
                float norm_right  = ((float)(outputData[base + 4U] - outZeroPoint) * outScale);
                float norm_bottom = ((float)(outputData[base + 5U] - outZeroPoint) * outScale);

                if (norm_left < 0.0f) norm_left = 0.0f;
                if (norm_top < 0.0f) norm_top = 0.0f;
                if (norm_right > 1.0f) norm_right = 1.0f;
                if (norm_bottom > 1.0f) norm_bottom = 1.0f;

                float bw = norm_right - norm_left;
                float bh = norm_bottom - norm_top;

                /* Filter out false-positive anchors:
                 * 1. Aspect ratio of human face is ~1.0 (between 0.45 and 2.2)
                 * 2. Reject boxes that span almost the entire image (bw > 0.90 || bh > 0.90)
                 * 3. Reject tiny slivers (bw < 0.08 || bh < 0.08) */
                if (bw > 0.08f && bh > 0.08f && bw < 0.90f && bh < 0.90f)
                {
                    float aspect = bw / bh;
                    if (aspect >= 0.45f && aspect <= 2.2f && s_candidateCount < MAX_CANDIDATES)
                    {
                        float xmin, ymin, xmax, ymax;
                        MapBoxBack(rot, norm_left, norm_top, norm_right, norm_bottom,
                                   cropLeft, cropTop, cropSize, &xmin, &ymin, &xmax, &ymax);

                        s_candidates[s_candidateCount].score = score;
                        s_candidates[s_candidateCount].xmin = xmin;
                        s_candidates[s_candidateCount].ymin = ymin;
                        s_candidates[s_candidateCount].xmax = xmax;
                        s_candidates[s_candidateCount].ymax = ymax;
                        s_candidateCount++;
                    }
                }
            }
        }

        if (rotMaxScore > globalMaxScore)
        {
            globalMaxScore = rotMaxScore;
            globalMaxAnchor = rotMaxAnchor;
            bestRot = rot;
        }

        /* If this rotation detected a valid face, lock onto it and break! */
        if (s_candidateCount > 0)
        {
            s_activeRot = rot;
            bestRot = rot;
            break;
        }
    }

    s_lastHadFace = (s_candidateCount > 0);

    if ((s_infFrameCount % 16U) == 0U)
    {
        int deg = 0;
        switch (bestRot) {
            case 1: deg = 90; break;
            case 2: deg = 180; break;
            case 3: deg = 270; break;
            default: deg = 0; break;
        }
        PRINTF("AI NPU: %ums | Top score: %d%% (threshold %d%%, rot=%d deg, a=%d) | MidPx: 0x%04X R=%u G=%u B=%u\r\n",
               (unsigned)(totalElapsedUs / 1000U),
               (int)(globalMaxScore * 100.0f),
               (int)(NPU_MODEL_DETECTION_THRESHOLD * 100.0f),
               deg,
               globalMaxAnchor,
               midPx, (unsigned)midR, (unsigned)midG, (unsigned)midB);
    }

    /* Sort candidate boxes descending by score */
    for (uint32_t i = 0; i < s_candidateCount; i++)
    {
        for (uint32_t j = i + 1; j < s_candidateCount; j++)
        {
            if (s_candidates[j].score > s_candidates[i].score)
            {
                candidate_box_t tmp = s_candidates[i];
                s_candidates[i] = s_candidates[j];
                s_candidates[j] = tmp;
            }
        }
    }

    /* Non-Maximum Suppression (NMS) */
    uint32_t boxCount = 0;
    for (uint32_t i = 0; i < s_candidateCount && boxCount < AI_MODEL_MAX_BOXES; i++)
    {
        if (s_candidates[i].score < 0.0f)
        {
            continue; /* Suppressed */
        }

        ai_bbox_t *box = &result->boxes[boxCount++];
        box->label = "face";
        box->score = s_candidates[i].score;
        box->x = (uint16_t)s_candidates[i].xmin;
        box->y = (uint16_t)s_candidates[i].ymin;
        box->width = (uint16_t)(s_candidates[i].xmax - s_candidates[i].xmin);
        box->height = (uint16_t)(s_candidates[i].ymax - s_candidates[i].ymin);

        float area_i = (s_candidates[i].xmax - s_candidates[i].xmin) * (s_candidates[i].ymax - s_candidates[i].ymin);
        for (uint32_t j = i + 1; j < s_candidateCount; j++)
        {
            if (s_candidates[j].score < 0.0f)
            {
                continue;
            }

            float inter_xmin = (s_candidates[i].xmin > s_candidates[j].xmin) ? s_candidates[i].xmin : s_candidates[j].xmin;
            float inter_ymin = (s_candidates[i].ymin > s_candidates[j].ymin) ? s_candidates[i].ymin : s_candidates[j].ymin;
            float inter_xmax = (s_candidates[i].xmax < s_candidates[j].xmax) ? s_candidates[i].xmax : s_candidates[j].xmax;
            float inter_ymax = (s_candidates[i].ymax < s_candidates[j].ymax) ? s_candidates[i].ymax : s_candidates[j].ymax;

            float inter_w = (inter_xmax > inter_xmin) ? (inter_xmax - inter_xmin) : 0.0f;
            float inter_h = (inter_ymax > inter_ymin) ? (inter_ymax - inter_ymin) : 0.0f;
            float inter_area = inter_w * inter_h;

            if (inter_area > 0.0f)
            {
                float area_j = (s_candidates[j].xmax - s_candidates[j].xmin) * (s_candidates[j].ymax - s_candidates[j].ymin);
                float union_area = area_i + area_j - inter_area;
                float iou = (union_area > 0.0f) ? (inter_area / union_area) : 0.0f;
                if (iou >= NPU_MODEL_NMS_THRESHOLD)
                {
                    s_candidates[j].score = -1.0f; /* Suppress */
                }
            }
        }
    }

    result->boxCount = boxCount;
    result->valid = (boxCount > 0U);
}

/* Return coordinate space of bounding boxes (frame space: 320x240) */
extern "C" uint16_t AI_MODEL_GetInputWidth(void)
{
    return 320U;
}

extern "C" uint16_t AI_MODEL_GetInputHeight(void)
{
    return 240U;
}
