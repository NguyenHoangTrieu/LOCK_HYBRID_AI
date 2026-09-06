/*
 * ipc_layout.h - dual-core RAM layout constants (Camera_AI_Test1).
 *
 * The shared region below is carved out of core0's m_data, immediately
 * below core1's own 0x2004E000 region (see ARCHITECTURE.md / WORKLOG.md:
 * a QVGA RGB565 frame, 153,600 bytes, does not fit inside core1's 104KB
 * region at all - it has to live in core0's much larger m_data instead).
 * Only core0's linker script (board_port/cm33_core0/
 * MCXN947_cm33_core0_dualcore.ld) actually declares a MEMORY region/output
 * section for this range, so the linker can catch core0 overflowing into
 * it by accident. core1 does NOT need its own linker awareness of this
 * region - it just reads/writes through the fixed-address pointers below,
 * the same way any two cores share a physical RAM bank without either one
 * "owning" it at the toolchain level.
 *
 * IPC_SHARED_BASE/IPC_SHARED_SIZE MUST match
 * board_port/cm33_core0/MCXN947_cm33_core0_dualcore.ld's `m_shared` MEMORY
 * block exactly - there is no automated check tying the two together.
 */
#ifndef IPC_LAYOUT_H_
#define IPC_LAYOUT_H_

#include <stdbool.h>
#include <stdint.h>

/* Physical placement: 0x20028000-0x2004E000, immediately below core1's own
 * 0x2004E000-0x20068000 region. core0's own m_data spans (0x20000000,
 * length 0x28000) - see MCXN947_cm33_core0_dualcore.ld's header comment
 * for the Stage 5 history of exactly this boundary (widened once, at this
 * region's expense, for core0's AI tensor arena). */
#define IPC_SHARED_BASE 0x20028000u
#define IPC_SHARED_SIZE 0x00026000u /* 152KB */

/* Layout within the shared region. */
#define IPC_FRAME_BUFFER_OFFSET 0x00000000u
#define IPC_FRAME_BUFFER_SIZE   153600u /* 320*240*2 bytes, QVGA RGB565 - camera_capture.c's frame buffer */

/* Stage 5 (WORKLOG.md): NOT an AI crop/resize buffer - deliberately never
 * added one. AI_MODEL_RunInference() (source/ai/model_runner.h) takes the
 * raw camera frame directly and does its own internal resize/quantize
 * into the model's 72x72 input - confirmed by reading model_runner_npu.cpp
 * before wiring this up, so core0 only ever needs read access to
 * IPC_FRAME_BUFFER_ADDR, nothing else. (An earlier draft of this layout
 * reserved ~15KB here for a crop buffer that would have gone unused -
 * removed, and the space reclaimed by core0's m_data instead, once this
 * was confirmed.) */
#define IPC_RESULT_OFFSET (IPC_FRAME_BUFFER_OFFSET + IPC_FRAME_BUFFER_SIZE)
#define IPC_RESULT_SIZE   256u /* sized generously for ai_ipc_result_t below */

#define IPC_USED_SIZE (IPC_RESULT_OFFSET + IPC_RESULT_SIZE)

#if IPC_USED_SIZE > IPC_SHARED_SIZE
#error "ipc_layout.h: shared region contents overflow IPC_SHARED_SIZE - grow it here AND in MCXN947_cm33_core0_dualcore.ld's m_shared MEMORY block"
#endif

#define IPC_FRAME_BUFFER_ADDR ((void *)(uintptr_t)(IPC_SHARED_BASE + IPC_FRAME_BUFFER_OFFSET))
#define IPC_RESULT_ADDR       ((void *)(uintptr_t)(IPC_SHARED_BASE + IPC_RESULT_OFFSET))

/* AI detection result, doorbell-delivered (see source/shared/ipc_events.h's
 * IPC_SignalFrameReady()/IPC_SignalResultReady()) but with the actual data
 * living here at IPC_RESULT_ADDR, not in the event's 16-bit payload.
 *
 * Deliberately NOT the same type as source/ai/model_runner.h's
 * ai_model_result_t: that struct's ai_bbox_t.label is a `const char *`
 * pointing at one of the model's class-label string literals, which live
 * in CORE0's own flash/text - copying that struct byte-for-byte into
 * shared RAM would hand core1 a pointer that (while likely readable, since
 * flash is physically one bus-shared resource) has no business being
 * treated as portable data between two independently-linked images. This
 * type carries the label as a small inline byte array instead - plain
 * data, safe to memcpy in and out of this shared region from either core. */
#define AI_IPC_LABEL_LEN 12u
#define AI_IPC_MAX_BOXES 10u /* MUST match source/ai/model_runner.h's AI_MODEL_MAX_BOXES. */

typedef struct
{
    char label[AI_IPC_LABEL_LEN]; /* NUL-terminated. */
    uint16_t x, y, width, height; /* AI_MODEL_GetInputWidth/Height() space - same convention as ai_bbox_t. */
    float score;
} ai_ipc_bbox_t;

typedef struct
{
    bool valid;
    uint32_t boxCount;
    ai_ipc_bbox_t boxes[AI_IPC_MAX_BOXES];
} ai_ipc_result_t;

#endif /* IPC_LAYOUT_H_ */
