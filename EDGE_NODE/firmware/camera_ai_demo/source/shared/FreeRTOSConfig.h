/*
 * FreeRTOSConfig.h - Camera_AI_Test1 dual-core RTOS migration (see
 * WORKLOG.md), shared by both cores.
 *
 * Hand-written, not generated from Kconfig - pulled in directly via
 * CMakeLists.txt's DUALCORE_RTOS branch instead of enabling FreeRTOS via
 * prj.conf, which is shared with the legacy single-core build and would
 * silently install FreeRTOS's SVC/PendSV/SysTick handlers there too, even
 * though that build never starts a scheduler.
 *
 * Values match the SDK's own Kconfig-generated defaults for this board.
 * configMAX_SYSCALL_INTERRUPT_PRIORITY=2 is confirmed compatible with
 * MCMGR's MAILBOX_IRQn priorities (5 on core0, 2 on core1), so its
 * ISR-context event callbacks can safely call xTaskNotifyFromISR().
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>
extern uint32_t SystemCoreClock;

#define configUSE_PREEMPTION                   1
#define configUSE_TIME_SLICING                 1
#define configCPU_CLOCK_HZ                     (SystemCoreClock)
#define configTICK_RATE_HZ                     1000U
#define configMAX_PRIORITIES                   5
#define configMINIMAL_STACK_SIZE               128U /* words */
#define configMAX_TASK_NAME_LEN                16
#define configUSE_16_BIT_TICKS                 0
#define configIDLE_SHOULD_YIELD                1
#define configUSE_TASK_NOTIFICATIONS           1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES  1
#define configUSE_MUTEXES                      1
#define configUSE_RECURSIVE_MUTEXES            1
#define configUSE_COUNTING_SEMAPHORES          1
#define configQUEUE_REGISTRY_SIZE              8
#define configUSE_QUEUE_SETS                   0
/* No software timers used anywhere in this codebase - timers.c isn't even
 * compiled in (see CMakeLists.txt's DUALCORE_RTOS branch), trimmed for
 * core1's tight m_text budget (WORKLOG.md, Stage 3). */
#define configUSE_TIMERS                       0
#define configUSE_TICKLESS_IDLE                0
#define configUSE_IDLE_HOOK                    0
#define configUSE_TICK_HOOK                    0
#define configSUPPORT_STATIC_ALLOCATION        0
#define configSUPPORT_DYNAMIC_ALLOCATION       1
#define configTOTAL_HEAP_SIZE                  (24U * 1024U) /* tune per stage - see WORKLOG.md RAM budget */
#define configUSE_MALLOC_FAILED_HOOK           1
#define configCHECK_FOR_STACK_OVERFLOW         2 /* cheap insurance - this project has hit real stack-overflow bugs before, see WORKLOG.md */
#define configUSE_TRACE_FACILITY               0
#define configGENERATE_RUN_TIME_STATS          0
#define configRECORD_STACK_HIGH_ADDRESS        1

/* API inclusions actually used by this project's IPC design
 * (source/shared/ipc_events.c) and task bring-up. */
#define INCLUDE_vTaskDelay                     1
#define INCLUDE_vTaskSuspend                   1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark    1

#define configASSERT(x)                                          \
    if ((x) == 0)                                                \
    {                                                             \
        taskDISABLE_INTERRUPTS();                                 \
        for (;;)                                                  \
        {                                                          \
        }                                                          \
    }

/* Cortex-M33 (ARM_CM33_NTZ port, no TrustZone - this project has none
 * configured, see ARCHITECTURE.md). configENABLE_FPU auto-detects per-core:
 * core0 compiles with a hard-float ABI (-mfloat-abi=hard), core1 does not
 * (-mfloat-abi=soft, confirmed from this project's own real core1 compile
 * flags) - __ARM_FP is only defined by GCC when hardware FP is actually
 * enabled for that compile, so this one shared header gets the right
 * answer for each core automatically. */
#if defined(__ARM_FP) && (__ARM_FP != 0)
#define configENABLE_FPU 1
#else
#define configENABLE_FPU 0
#endif
#define configENABLE_MPU       0
#define configENABLE_TRUSTZONE 0
/* Required for this "NTZ" (No TrustZone) port: the valid no-TrustZone
 * combo is configRUN_FREERTOS_SECURE_ONLY=1 with configENABLE_TRUSTZONE=0,
 * not both 0. Leaving this undefined caused a real UsageFault->HardFault
 * during vTaskStartScheduler()'s first task start on real hardware. */
#define configRUN_FREERTOS_SECURE_ONLY 1

#ifdef __NVIC_PRIO_BITS
#define configPRIO_BITS __NVIC_PRIO_BITS
#else
#define configPRIO_BITS 3
#endif

#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY     ((1U << (configPRIO_BITS)) - 1)
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 2
#define configKERNEL_INTERRUPT_PRIORITY             (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY        (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* xPortPendSVHandler/xPortSysTickHandler, not the older
 * vPortPendSVHandler/vPortSysTickHandler naming - the old names don't
 * match this port's real symbols and silently rename nothing, leaving
 * the SDK's non-functional default handlers installed (confirmed on real
 * hardware: scheduler start hung completely, no crash, no output). */
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

#endif /* FREERTOS_CONFIG_H */
