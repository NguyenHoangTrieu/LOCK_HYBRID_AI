/*
 * freertos_hooks.c - required FreeRTOS hook implementations, shared by both
 * cores (see source/shared/FreeRTOSConfig.h: configCHECK_FOR_STACK_OVERFLOW=2,
 * configUSE_MALLOC_FAILED_HOOK=1).
 *
 * Both just spin with interrupts disabled - same "stop, don't limp on"
 * philosophy as this project's own fault_handler.c for the legacy build;
 * a live SWD session can still read the fault reason off the stack this
 * way (matching this project's own established debugging practice).
 */
#include "FreeRTOS.h"
#include "task.h"

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;
    taskDISABLE_INTERRUPTS();
    for (;;)
    {
    }
}

void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();
    for (;;)
    {
    }
}
