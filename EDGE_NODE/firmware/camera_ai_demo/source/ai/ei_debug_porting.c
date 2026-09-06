/*
 * ei_debug_porting.c - overrides the Edge Impulse SDK's weak ei_printf/
 * ei_printf_float (edge_impulse/edge-impulse-sdk/porting/clib/
 * ei_classifier_porting.cpp, EI_PORTING_CLIB=1) to go through this
 * project's fsl_debug_console PRINTF() instead of plain vprintf() - the
 * project links with --specs=nosys.specs, so plain libc stdout isn't
 * wired to anything and SDK-internal log lines (allocation errors, etc.)
 * would otherwise silently vanish instead of reaching the serial console.
 */
#include <stdarg.h>
#include "fsl_debug_console.h"

void ei_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    DbgConsole_Vprintf(format, args);
    va_end(args);
}

void ei_printf_float(float f)
{
    /* debug_console_lite (used elsewhere in this project) may not support
     * %f - print as a fixed 3-decimal value using integer math instead. */
    int whole = (int)f;
    int frac = (int)((f - (float)whole) * 1000.0f);
    if (frac < 0)
    {
        frac = -frac;
    }
    PRINTF("%d.%03d", whole, frac);
}
