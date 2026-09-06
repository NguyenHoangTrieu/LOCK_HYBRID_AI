/*
 * fault_handler.c - diagnostic HardFault dump.
 *
 * Overrides the default (weak) HardFault_Handler, which is just an
 * infinite loop, to print the fault status registers and PC/LR before
 * looping forever. MemManage/Bus/Usage faults aren't individually enabled
 * in this project, so they all escalate here too - one handler catches
 * all of them.
 */
#include <stdint.h>
#include "fsl_device_registers.h"
#include "fsl_debug_console.h"

__attribute__((used)) static void FaultHandlerC(uint32_t *stackedRegs)
{
    /* debug_console_lite's minimal printf mishandles the 'l' length
     * modifier (prints the literal characters "lX" instead of the value -
     * same class of issue as this project's existing "%f not supported"
     * notes elsewhere). unsigned long and unsigned int are both 32-bit on
     * this target, so dropping the modifier and using plain %08X is
     * equivalent, and actually prints correctly. */
    PRINTF("\r\n*** HardFault ***\r\n");
    PRINTF("CFSR  = 0x%08X (MMFSR=0x%02X BFSR=0x%02X UFSR=0x%04X)\r\n", (unsigned int)SCB->CFSR,
           (unsigned int)(SCB->CFSR & 0xFFUL), (unsigned int)((SCB->CFSR >> 8) & 0xFFUL),
           (unsigned int)((SCB->CFSR >> 16) & 0xFFFFUL));
    PRINTF("HFSR  = 0x%08X\r\n", (unsigned int)SCB->HFSR);
    PRINTF("MMFAR = 0x%08X\r\n", (unsigned int)SCB->MMFAR);
    PRINTF("BFAR  = 0x%08X\r\n", (unsigned int)SCB->BFAR);
    PRINTF("Stacked r0=0x%08X r1=0x%08X r2=0x%08X r3=0x%08X\r\n", (unsigned int)stackedRegs[0],
           (unsigned int)stackedRegs[1], (unsigned int)stackedRegs[2], (unsigned int)stackedRegs[3]);
    PRINTF("Stacked r12=0x%08X LR=0x%08X PC=0x%08X xPSR=0x%08X\r\n", (unsigned int)stackedRegs[4],
           (unsigned int)stackedRegs[5], (unsigned int)stackedRegs[6], (unsigned int)stackedRegs[7]);
    while (1)
    {
    }
}

__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile(
        "tst lr, #4      \n"
        "ite eq          \n"
        "mrseq r0, msp   \n"
        "mrsne r0, psp   \n"
        "b FaultHandlerC \n");
}
