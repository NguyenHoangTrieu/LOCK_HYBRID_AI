/*
 * ipc_events.c - see ipc_events.h.
 */
#include "ipc_events.h"

/* Runs in MAILBOX_IRQn interrupt context (confirmed by reading
 * mcmgr_internal_core_api_mcxnx4x.c directly - MAILBOX_IRQHandler() calls
 * registered event callbacks straight from the ISR, not deferred to a
 * task) - must only use ISR-safe FreeRTOS APIs. */
static void IPC_EVENTS_Callback(mcmgr_core_t coreNum, uint16_t data, void *context)
{
    (void)coreNum;
    TaskHandle_t notifyTask                 = (TaskHandle_t)context;
    BaseType_t higherPriorityTaskWoken = pdFALSE;

    xTaskNotifyFromISR(notifyTask, data, eSetValueWithOverwrite, &higherPriorityTaskWoken);
    portYIELD_FROM_ISR(higherPriorityTaskWoken);
}

void IPC_EVENTS_RegisterHandler(TaskHandle_t notifyTask)
{
    (void)MCMGR_RegisterEvent(kMCMGR_RemoteApplicationEvent, IPC_EVENTS_Callback, (void *)notifyTask);
}

void IPC_EVENTS_Trigger(mcmgr_core_t targetCore, uint16_t data)
{
    (void)MCMGR_TriggerEvent(targetCore, kMCMGR_RemoteApplicationEvent, data);
}

void IPC_SignalFrameReady(uint16_t frameSeq)
{
    IPC_EVENTS_Trigger(kMCMGR_Core0, frameSeq);
}

void IPC_SignalResultReady(uint16_t frameSeq)
{
    IPC_EVENTS_Trigger(kMCMGR_Core1, frameSeq);
}
