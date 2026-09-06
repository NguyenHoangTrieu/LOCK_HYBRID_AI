/*
 * ei_sramx_alloc.h - see ei_sramx_alloc.c.
 */
#ifndef _EI_SRAMX_ALLOC_H_
#define _EI_SRAMX_ALLOC_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! @brief Reset both the m_sramx pool and the overflow pool (if set) to
 *  empty. Call once per AI_MODEL_RunInference(), before ei_run_classifier(). */
void EI_SRAMX_PoolReset(void);

/*!
 * @brief Lend a second, temporarily-idle buffer to the allocator as
 * overflow space once the m_sramx pool (96KB) is exhausted.
 *
 * main.c lends s_lcdSnapshot's memory during AI_MODEL_RunInference() -
 * safe only because that buffer is filled *after* inference finishes, not
 * before. Don't reorder that copy earlier without removing this call, or
 * ei_run_classifier() will corrupt it.
 *
 * @param ptr  Start of the buffer to lend.
 * @param size Size of the buffer, in bytes.
 */
void EI_SRAMX_SetOverflowPool(void *ptr, size_t size);

/*! @brief Combined high-water mark (m_sramx pool + overflow pool) from the
 *  most recent AI_MODEL_RunInference() call - for diagnostics only. */
size_t EI_SRAMX_GetHighWaterMark(void);

#ifdef __cplusplus
}
#endif

#endif /* _EI_SRAMX_ALLOC_H_ */
