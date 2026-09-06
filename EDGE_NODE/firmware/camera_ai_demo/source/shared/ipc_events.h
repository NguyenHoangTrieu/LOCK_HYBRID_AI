/*
 * ipc_events.h - thin MCMGR event wrapper, shared by both cores (see
 * WORKLOG.md's dual-core RTOS migration entries).
 *
 * Deliberately minimal: one application event type
 * (kMCMGR_RemoteApplicationEvent), a 16-bit payload, ISR-context callback
 * that hands off to a FreeRTOS task via a task notification. This is
 * "MCMGR events only, no RPMsg-Lite" exactly as decided in the approved
 * plan - later stages (frame-ready / result-ready doorbells) reuse this
 * same mechanism, with the real data living in source/shared/ipc_layout.h's
 * shared RAM region rather than the 16-bit payload itself.
 */
#ifndef IPC_EVENTS_H_
#define IPC_EVENTS_H_

#include <stdint.h>
#include "mcmgr.h"
#include "FreeRTOS.h"
#include "task.h"

/* Registers this core's handler for kMCMGR_RemoteApplicationEvent: when
 * the OTHER core triggers it (see IPC_EVENTS_Trigger), notifyTask is woken
 * via xTaskNotifyFromISR with the event's 16-bit payload as the
 * notification value (eSetValueWithOverwrite - only the latest value
 * matters, matching a "doorbell" semantic, not a queued message). Call
 * once per core, after MCMGR_Init() and after notifyTask has been created. */
void IPC_EVENTS_RegisterHandler(TaskHandle_t notifyTask);

/* Trigger kMCMGR_RemoteApplicationEvent on targetCore, from task context
 * (not ISR-safe - MCMGR_TriggerEvent is a task-context API). */
void IPC_EVENTS_Trigger(mcmgr_core_t targetCore, uint16_t data);

/* Stage 5 (WORKLOG.md) doorbells - thin, semantically-named wrappers over
 * IPC_EVENTS_Trigger() above, one pair per direction. The payload in both
 * is a frame sequence number, not the actual frame/result data - that
 * lives in source/shared/ipc_layout.h's shared RAM region
 * (IPC_FRAME_BUFFER_ADDR / IPC_RESULT_ADDR), read directly by whichever
 * side receives the doorbell. core1 (camera/LCD/SD) calls
 * IPC_SignalFrameReady() after stopping SmartDMA (CAMERA_CAPTURE_Deinit())
 * and before restarting it - the frame buffer is only guaranteed stable
 * between those two calls, so core0 must finish reading it and reply
 * with IPC_SignalResultReady() before core1 resumes capture. Not
 * ISR-safe (task-context only), same restriction as IPC_EVENTS_Trigger(). */
void IPC_SignalFrameReady(uint16_t frameSeq);
void IPC_SignalResultReady(uint16_t frameSeq);

#endif /* IPC_EVENTS_H_ */
