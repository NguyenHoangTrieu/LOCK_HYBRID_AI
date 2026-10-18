/*
 * model_runner_npu.cpp - alternate model_runner.h implementation using
 * this chip's Neutron NPU instead of the CPU+CMSIS-NN path in
 * model_runner.cpp. Selected at build time by AI_MODEL_USE_NPU.
 *
 * Model: neutron/ultraface_slim_ultraslim_npu16_tflite.h, an UltraFace
 * UltraSlim face detector (128x128x3 INT8 input), converted for NXP Neutron
 * NPU. Output tensor: [1, 172, 6] INT8 representing 172 prior anchors:
 * [no_face_score, face_score, left, top, right, bottom].
 * Post-processing: score thresholding + IoU Non-Maximum Suppression (NMS).
 */
#include "fsl_common.h"
#include "fsl_debug_console.h"
#include "model_runner.h"

#include "tensorflow/lite/micro/kernels/neutron/neutron.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "neutron/ultraface_slim_ultraslim_npu16_tflite.h"
#include "neutron/ultraface_slim_ultraslim_npu16_tflite_info.h"
#if defined(AI_ENABLE_BOOT_SELFTEST) && AI_ENABLE_BOOT_SELFTEST
#include "neutron/couple_COCO_128_128_rgb.h"
#endif

#ifdef DUALCORE_RTOS
#define AI_SRAMX_BSS
#else
#define AI_SRAMX_BSS __attribute__((section(".sramx_bss"), aligned(4)))
#endif

#define NPU_MODEL_INPUT_WIDTH 128U
#define NPU_MODEL_INPUT_HEIGHT 128U
#define NPU_MODEL_NUM_PRIORS 172U
#define NPU_MODEL_DETECTION_THRESHOLD 0.80f
#define NPU_MODEL_NMS_THRESHOLD 0.45f

constexpr int kTensorArenaSize = 244 * 1024;
static uint8_t s_tensorArena[kTensorArenaSize] __attribute__((aligned(16)));

static const tflite::Model *s_model = nullptr;
static tflite::MicroInterpreter *s_interpreter = nullptr;

typedef struct {
  float score;
  float xmin, ymin, xmax, ymax;
} candidate_box_t;

#define MAX_CANDIDATES 32
static candidate_box_t s_candidates[MAX_CANDIDATES] AI_SRAMX_BSS;
static uint32_t s_candidateCount;

static void AI_MODEL_InitTiming(void) {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* Direct RGB565 channel -> INT8 quantized tensor lookup tables.
 * Avoids 3 integer multiplications + 3 integer divisions (/31, /63, /31)
 * per pixel across 16,384 pixels every frame. */
static int8_t s_lutR5B5[32] AI_SRAMX_BSS;
static int8_t s_lutG6[64] AI_SRAMX_BSS;

static int8_t QuantizeChannelByte(uint32_t val8, float scale, int32_t zp) {
  const float stdVal = (float)ULTRAFACE_INPUT_STD;
  const float meanVal = (float)ULTRAFACE_INPUT_MEAN;
  float real = ((float)val8 - meanVal) / stdVal;
  int32_t q = (int32_t)roundf(real / scale) + zp;
  if (q < -128)
    q = -128;
  if (q > 127)
    q = 127;
  return (int8_t)q;
}

static void InitInputLut(float scale, int32_t zp) {
  for (uint32_t v5 = 0U; v5 < 32U; v5++) {
    uint32_t val8 = (v5 * 255U) / 31U;
    s_lutR5B5[v5] = QuantizeChannelByte(val8, scale, zp);
  }
  for (uint32_t v6 = 0U; v6 < 64U; v6++) {
    uint32_t val8 = (v6 * 255U) / 63U;
    s_lutG6[v6] = QuantizeChannelByte(val8, scale, zp);
  }
}

extern "C" void AI_MODEL_Init(void) {
  AI_MODEL_InitTiming();

  s_model = tflite::GetModel(ultraface_data);
  if (s_model->version() != TFLITE_SCHEMA_VERSION) {
    PRINTF("AI_MODEL_Init: NPU model schema version %d != supported %d\r\n",
           (int)s_model->version(), TFLITE_SCHEMA_VERSION);
    return;
  }

  static tflite::MicroMutableOpResolver<12> s_opResolver;
  s_opResolver.AddCustom(tflite::GetString_NEUTRON_GRAPH(),
                         tflite::Register_NEUTRON_GRAPH());
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

  static tflite::MicroInterpreter s_staticInterpreter(
      s_model, s_opResolver, s_tensorArena, kTensorArenaSize);
  s_interpreter = &s_staticInterpreter;

  if (s_interpreter->AllocateTensors() != kTfLiteOk) {
    PRINTF("AI_MODEL_Init: AllocateTensors() failed - kTensorArenaSize (%d "
           "bytes) is too small\r\n",
           kTensorArenaSize);
    s_interpreter = nullptr;
    return;
  }

  TfLiteTensor *inT = s_interpreter->input(0);
  TfLiteTensor *outT = s_interpreter->output(0);
  PRINTF("AI_MODEL_Init: UltraFace UltraSlim NPU face detector ready (%dx%d "
         "input, arena used %u/%u bytes)\r\n",
         NPU_MODEL_INPUT_WIDTH, NPU_MODEL_INPUT_HEIGHT,
         (unsigned)s_interpreter->arena_used_bytes(),
         (unsigned)kTensorArenaSize);
  PRINTF(
      "AI_MODEL_Init: in scale=%d/10000 zp=%d | out scale=%d/10000 zp=%d\r\n",
      inT ? (int)(inT->params.scale * 10000.0f) : 0,
      inT ? inT->params.zero_point : 0,
      outT ? (int)(outT->params.scale * 10000.0f) : 0,
      outT ? outT->params.zero_point : 0);

  float inScale =
      (inT && inT->params.scale > 0.0f) ? inT->params.scale : 0.0078125f;
  int32_t inZp = inT ? inT->params.zero_point : 0;
  InitInputLut(inScale, inZp);

#if defined(AI_ENABLE_BOOT_SELFTEST) && AI_ENABLE_BOOT_SELFTEST
  /* Self-test with known MS COCO couple test image */
  int8_t *inData = inT->data.int8;
  for (size_t i = 0; i < 128U * 128U * 3U; i++) {
    inData[i] =
        QuantizeChannelByte(couple_COCO_128_128_rgb_data[i], inScale, inZp);
  }
  TfLiteStatus testInv = s_interpreter->Invoke();
  const int8_t *testOut = outT->data.int8;
  float testScale =
      outT->params.scale <= 0.0f ? ULTRAFACE_OUTPUT_SCALE : outT->params.scale;
  int32_t testZp = outT->params.scale <= 0.0f
                       ? (int32_t)ULTRAFACE_OUTPUT_ZERO_POINT
                       : outT->params.zero_point;
  float testMaxScore = -1.0f;
  uint32_t testFound = 0;
  int maxAnchor = -1;
  for (uint32_t i = 0; i < NPU_MODEL_NUM_PRIORS; i++) {
    int8_t sc_q = testOut[i * 6U + 1U];
    float sc = (float)(sc_q - testZp) * testScale;
    if (sc > testMaxScore) {
      testMaxScore = sc;
      maxAnchor = (int)i;
    }
    if (sc >= 0.50f) {
      testFound++;
    }
  }
  PRINTF("AI_MODEL_Init: COCO couple self-test -> invoke=%d, maxScore=%d%% "
         "(anchor=%d), faces>=50%%: %u\r\n",
         (int)testInv, (int)(testMaxScore * 100.0f), maxAnchor,
         (unsigned)testFound);
  if (maxAnchor >= 0) {
    size_t b = (size_t)maxAnchor * 6U;
    PRINTF("  Max anchor [%d]: c0=%d c1=%d | l=%d t=%d r=%d b=%d\r\n",
           maxAnchor, testOut[b], testOut[b + 1], testOut[b + 2],
           testOut[b + 3], testOut[b + 4], testOut[b + 5]);
  }
#endif
}

static void SampleCropTensor(const uint16_t *pixels, uint16_t width,
                             uint16_t height, int rot, int8_t *inputData,
                             uint16_t *outMidPx, uint8_t *outMidR,
                             uint8_t *outMidG, uint8_t *outMidB) {
  const uint16_t cropSize = (height < width) ? height : width; /* 240 */
  const uint16_t cropLeft =
      (width > cropSize) ? ((width - cropSize) / 2U) : 0U; /* 40 */
  const uint16_t cropTop =
      (height > cropSize) ? ((height - cropSize) / 2U) : 0U; /* 0 */

  /* Sample center pixel once outside the hot loop for diagnostic logging */
  {
    uint16_t midSx = cropLeft + (cropSize >> 1U);
    uint16_t midSy = cropTop + (cropSize >> 1U);
    uint16_t px = pixels[(uint32_t)midSy * width + midSx];
    if (outMidPx)
      *outMidPx = px;
    if (outMidR)
      *outMidR = (uint8_t)(((px >> 11U) & 0x1FU) * 255U / 31U);
    if (outMidG)
      *outMidG = (uint8_t)(((px >> 5U) & 0x3FU) * 255U / 63U);
    if (outMidB)
      *outMidB = (uint8_t)((px & 0x1FU) * 255U / 31U);
  }

  int8_t *dst = inputData;

  for (uint32_t ty = 0U; ty < NPU_MODEL_INPUT_HEIGHT; ty++) {
    uint32_t scaledTy = (ty * cropSize) >> 7U;
    uint32_t scaledInvTy = ((127U - ty) * cropSize) >> 7U;

    for (uint32_t tx = 0U; tx < NPU_MODEL_INPUT_WIDTH; tx++) {
      uint32_t sx, sy;
      switch (rot) {
      case 2: /* 180 deg upside-down */
        sx = cropLeft + (((127U - tx) * cropSize) >> 7U);
        sy = cropTop + scaledInvTy;
        break;
      case 1: /* 90 deg CW */
        sx = cropLeft + scaledTy;
        sy = cropTop + (((127U - tx) * cropSize) >> 7U);
        break;
      case 3: /* 270 deg CW / 90 deg CCW */
        sx = cropLeft + scaledInvTy;
        sy = cropTop + ((tx * cropSize) >> 7U);
        break;
      case 0: /* 0 deg normal */
      default:
        sx = cropLeft + ((tx * cropSize) >> 7U);
        sy = cropTop + scaledTy;
        break;
      }

      uint16_t px = pixels[sy * width + sx];
      dst[0] = s_lutR5B5[(px >> 11U) & 0x1FU];
      dst[1] = s_lutG6[(px >> 5U) & 0x3FU];
      dst[2] = s_lutR5B5[px & 0x1FU];
      dst += 3;
    }
  }
}

static void MapBoxBack(int rot, float norm_l, float norm_t, float norm_r,
                       float norm_b, uint16_t cropLeft, uint16_t cropTop,
                       uint16_t cropSize, float *out_xmin, float *out_ymin,
                       float *out_xmax, float *out_ymax) {
  float cs = (float)cropSize;
  float cl = (float)cropLeft;
  float ct = (float)cropTop;
  switch (rot) {
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

extern "C" void AI_MODEL_RunInference(const uint16_t *pixels, uint16_t width,
                                      uint16_t height,
                                      ai_model_result_t *result) {
  result->valid = false;
  result->boxCount = 0;

  if (s_interpreter == nullptr) {
    PRINTF("AI_MODEL_RunInference: interpreter not initialized "
           "(AllocateTensors() failed earlier)\r\n");
    return;
  }

  TfLiteTensor *inputTensor = s_interpreter->input(0);
  int8_t *inputData = inputTensor->data.int8;

  const uint16_t cropSize = (height < width) ? height : width; /* 240 */
  const uint16_t cropLeft =
      (width > cropSize) ? ((width - cropSize) / 2U) : 0U; /* 40 */
  const uint16_t cropTop =
      (height > cropSize) ? ((height - cropSize) / 2U) : 0U; /* 0 */

  uint8_t midR = 0, midG = 0, midB = 0;
  uint16_t midPx = 0;
  uint32_t totalElapsedUs = 0;

  static uint32_t s_infFrameCount = 0;
  s_infFrameCount++;

  /* Dynamic fast rotation scheduler:
   * Primary physical orientation on J9 is rot=3 (270 deg CW).
   * Runs 1 NPU pass (16ms) per frame normally; probes an alternative
   * rotation once every 32 idle frames so idle FPS stays high (~16-18 FPS). */
  static int s_activeRot = 3; /* 270 deg default */
  static int s_altStep = 0;
  const int allRots[4] = {3, 0, 2, 1};

  int rotsToTry[2];
  int numRotsToTry = 1;
  rotsToTry[0] = s_activeRot;

  /* Probe an alternative rotation occasionally when idle */
  static bool s_lastHadFace = false;
  if (!s_lastHadFace && ((s_infFrameCount & 31U) == 0U)) {
    s_altStep = (s_altStep + 1) % 4;
    if (allRots[s_altStep] != s_activeRot) {
      rotsToTry[1] = allRots[s_altStep];
      numRotsToTry = 2;
    }
  }

  int bestRot = s_activeRot;
  float globalMaxScore = -1.0f;
  int globalMaxAnchor = -1;

  for (int rIdx = 0; rIdx < numRotsToTry; rIdx++) {
    int rot = rotsToTry[rIdx];
    SampleCropTensor(pixels, width, height, rot, inputData, &midPx, &midR,
                     &midG, &midB);

    uint32_t cycStart = DWT->CYCCNT;
    TfLiteStatus invokeStatus = s_interpreter->Invoke();
    uint32_t elapsedCycles = DWT->CYCCNT - cycStart;
    totalElapsedUs +=
        (uint32_t)(((uint64_t)elapsedCycles * 1000000ULL) / SystemCoreClock);

    if (invokeStatus != kTfLiteOk)
      continue;

    TfLiteTensor *outputTensor = s_interpreter->output(0);
    const int8_t *outputData = outputTensor->data.int8;
    float outScale = outputTensor->params.scale <= 0.0f
                         ? ULTRAFACE_OUTPUT_SCALE
                         : outputTensor->params.scale;
    int32_t outZeroPoint = outputTensor->params.scale <= 0.0f
                               ? (int32_t)ULTRAFACE_OUTPUT_ZERO_POINT
                               : outputTensor->params.zero_point;

    s_candidateCount = 0;
    float rotMaxScore = -1.0f;
    int rotMaxAnchor = -1;
    for (uint32_t i = 0; i < NPU_MODEL_NUM_PRIORS; i++) {
      size_t base = i * 6U;
      int8_t face_score_q = outputData[base + 1U];
      float score = (float)(face_score_q - outZeroPoint) * outScale;
      if (score > rotMaxScore) {
        rotMaxScore = score;
        rotMaxAnchor = (int)i;
      }
      if (score >= NPU_MODEL_DETECTION_THRESHOLD) {
        float norm_left =
            ((float)(outputData[base + 2U] - outZeroPoint) * outScale);
        float norm_top =
            ((float)(outputData[base + 3U] - outZeroPoint) * outScale);
        float norm_right =
            ((float)(outputData[base + 4U] - outZeroPoint) * outScale);
        float norm_bottom =
            ((float)(outputData[base + 5U] - outZeroPoint) * outScale);

        if (norm_left < 0.0f)
          norm_left = 0.0f;
        if (norm_top < 0.0f)
          norm_top = 0.0f;
        if (norm_right > 1.0f)
          norm_right = 1.0f;
        if (norm_bottom > 1.0f)
          norm_bottom = 1.0f;

        float bw = norm_right - norm_left;
        float bh = norm_bottom - norm_top;

        /* Filter out false-positive anchors:
         * 1. Aspect ratio of human face is ~1.0 (between 0.45 and 2.2)
         * 2. Reject boxes that span almost the entire image (bw > 0.90 || bh >
         * 0.90)
         * 3. Reject tiny slivers (bw < 0.08 || bh < 0.08) */
        if (bw > 0.08f && bh > 0.08f && bw < 0.90f && bh < 0.90f) {
          float aspect = bw / bh;
          if (aspect >= 0.45f && aspect <= 2.2f &&
              s_candidateCount < MAX_CANDIDATES) {
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

    if (rotMaxScore > globalMaxScore) {
      globalMaxScore = rotMaxScore;
      globalMaxAnchor = rotMaxAnchor;
      bestRot = rot;
    }

    /* If this rotation detected a valid face, lock onto it and break! */
    if (s_candidateCount > 0) {
      s_activeRot = rot;
      bestRot = rot;
      break;
    }
  }

  s_lastHadFace = (s_candidateCount > 0);

  if ((s_infFrameCount % 16U) == 0U) {
    int deg = 0;
    switch (bestRot) {
    case 1:
      deg = 90;
      break;
    case 2:
      deg = 180;
      break;
    case 3:
      deg = 270;
      break;
    default:
      deg = 0;
      break;
    }
    PRINTF("AI NPU: %ums | Top score: %d%% (threshold %d%%, rot=%d deg, a=%d) "
           "| MidPx: 0x%04X R=%u G=%u B=%u\r\n",
           (unsigned)(totalElapsedUs / 1000U), (int)(globalMaxScore * 100.0f),
           (int)(NPU_MODEL_DETECTION_THRESHOLD * 100.0f), deg, globalMaxAnchor,
           midPx, (unsigned)midR, (unsigned)midG, (unsigned)midB);
  }

  /* Sort candidate boxes descending by score */
  for (uint32_t i = 0; i < s_candidateCount; i++) {
    for (uint32_t j = i + 1; j < s_candidateCount; j++) {
      if (s_candidates[j].score > s_candidates[i].score) {
        candidate_box_t tmp = s_candidates[i];
        s_candidates[i] = s_candidates[j];
        s_candidates[j] = tmp;
      }
    }
  }

  /* Non-Maximum Suppression (NMS) */
  uint32_t boxCount = 0;
  for (uint32_t i = 0; i < s_candidateCount && boxCount < AI_MODEL_MAX_BOXES;
       i++) {
    if (s_candidates[i].score < 0.0f) {
      continue; /* Suppressed */
    }

    ai_bbox_t *box = &result->boxes[boxCount++];
    box->label = "face";
    box->score = s_candidates[i].score;
    box->x = (uint16_t)s_candidates[i].xmin;
    box->y = (uint16_t)s_candidates[i].ymin;
    box->width = (uint16_t)(s_candidates[i].xmax - s_candidates[i].xmin);
    box->height = (uint16_t)(s_candidates[i].ymax - s_candidates[i].ymin);

    float area_i = (s_candidates[i].xmax - s_candidates[i].xmin) *
                   (s_candidates[i].ymax - s_candidates[i].ymin);
    for (uint32_t j = i + 1; j < s_candidateCount; j++) {
      if (s_candidates[j].score < 0.0f) {
        continue;
      }

      float inter_xmin = (s_candidates[i].xmin > s_candidates[j].xmin)
                             ? s_candidates[i].xmin
                             : s_candidates[j].xmin;
      float inter_ymin = (s_candidates[i].ymin > s_candidates[j].ymin)
                             ? s_candidates[i].ymin
                             : s_candidates[j].ymin;
      float inter_xmax = (s_candidates[i].xmax < s_candidates[j].xmax)
                             ? s_candidates[i].xmax
                             : s_candidates[j].xmax;
      float inter_ymax = (s_candidates[i].ymax < s_candidates[j].ymax)
                             ? s_candidates[i].ymax
                             : s_candidates[j].ymax;

      float inter_w =
          (inter_xmax > inter_xmin) ? (inter_xmax - inter_xmin) : 0.0f;
      float inter_h =
          (inter_ymax > inter_ymin) ? (inter_ymax - inter_ymin) : 0.0f;
      float inter_area = inter_w * inter_h;

      if (inter_area > 0.0f) {
        float area_j = (s_candidates[j].xmax - s_candidates[j].xmin) *
                       (s_candidates[j].ymax - s_candidates[j].ymin);
        float union_area = area_i + area_j - inter_area;
        float iou = (union_area > 0.0f) ? (inter_area / union_area) : 0.0f;
        if (iou >= NPU_MODEL_NMS_THRESHOLD) {
          s_candidates[j].score = -1.0f; /* Suppress */
        }
      }
    }
  }

  result->boxCount = boxCount;
  result->valid = (boxCount > 0U);
}

/* Return coordinate space of bounding boxes (frame space: 320x240) */
extern "C" uint16_t AI_MODEL_GetInputWidth(void) { return 320U; }

extern "C" uint16_t AI_MODEL_GetInputHeight(void) { return 240U; }
