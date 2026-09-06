/*
 * hardware_init.c - core1 (Camera_AI_Test1 dual-core RTOS migration - see
 * WORKLOG.md). Camera + LCD clock/pin bring-up, same sources as the
 * legacy core0 hardware_init.c (see that file for the fix history).
 * No USB-HS/Overdrive code - USB streaming is out of scope here.
 */

#include "pin_mux.h"
#include "clock_config.h"
#include "board.h"
#include "app.h"
#include "fsl_clock.h"
#include "fsl_inputmux.h"
#include "fsl_spc.h"

/* SmartDMA camera capture only runs reliably at DCDC Mid voltage (1.0V) -
 * see WORKLOG.md. core1 never needs Overdrive (no USB here), so this is a
 * one-shot boot-time set, not a pair of helpers to flip between. */
static void BOARD_SetRegulatorsMidVoltage(void)
{
    spc_active_mode_core_ldo_option_t ldoOpt = {
        .CoreLDOVoltage       = kSPC_CoreLDO_MidDriveVoltage,
        .CoreLDODriveStrength = kSPC_CoreLDO_NormalDriveStrength,
    };
    SPC_SetActiveModeCoreLDORegulatorConfig(SPC0, &ldoOpt);

    spc_active_mode_dcdc_option_t dcdcOpt = {
        .DCDCVoltage       = kSPC_DCDC_MidVoltage,
        .DCDCDriveStrength = kSPC_DCDC_NormalDriveStrength,
    };
    SPC_SetActiveModeDCDCRegulatorConfig(SPC0, &dcdcOpt);
}

void BOARD_InitHardware(void)
{
    /* attach FRO 12M to FLEXCOMM4 (debug console) - matches Stage 1/2's
     * core1 bring-up, kept here since BOARD_InitBootClocks() below doesn't
     * do this itself. */
    CLOCK_SetClkDiv(kCLOCK_DivFlexcom4Clk, 1u);
    CLOCK_AttachClk(BOARD_DEBUG_UART_CLK_ATTACH);

    BOARD_InitBootPins();
    BOARD_InitBootClocks();
    BOARD_InitDebugConsole();

    BOARD_SetRegulatorsMidVoltage();

    /* Camera XCLK: route CLKOUT (P2_2) from FRO_HF/2 = 24MHz - see
     * board_port/cm33_core0/hardware_init.c's comment (WORKLOG.md) for the
     * full XCLK-mismatch bug this fixes; unchanged here, just relocated. */
    CLOCK_AttachClk(kFRO_HF_to_CLKOUT);
    CLOCK_SetClkDiv(kCLOCK_DivClkOut, 2U);

    /* GPIO0 for most LCD pins, GPIO1 for DC (Arduino D3 - see app.h). */
    CLOCK_EnableClock(kCLOCK_Gpio0);
    CLOCK_EnableClock(kCLOCK_Gpio1);

    /* Camera I2C (SCCB) clock. */
    CLOCK_AttachClk(kFRO12M_to_FLEXCOMM7);
    CLOCK_EnableClock(kCLOCK_LPFlexComm7);
    CLOCK_EnableClock(kCLOCK_LPI2c7);
    CLOCK_SetClkDiv(kCLOCK_DivFlexcom7Clk, 1u);

    /* Shared LPSPI1 bus (Arduino D10..D13 - LCD/microSD/touch, see
     * source/spi1_bus.h). FRO_HF (48MHz), not FRO12M - see
     * board_port/cm33_core0/hardware_init.c's comment (WORKLOG.md) for why. */
    CLOCK_AttachClk(kFRO_HF_DIV_to_FLEXCOMM1);
    CLOCK_SetClkDiv(kCLOCK_DivFlexcom1Clk, 1u);

    /* Route camera VSYNC/HSYNC/PCLK (P0_4/P0_11/P0_5) to the SmartDMA. */
    INPUTMUX_Init(INPUTMUX0);
    INPUTMUX_AttachSignal(INPUTMUX0, 0, kINPUTMUX_GpioPort0Pin4ToSmartDma);
    INPUTMUX_AttachSignal(INPUTMUX0, 1, kINPUTMUX_GpioPort0Pin11ToSmartDma);
    INPUTMUX_AttachSignal(INPUTMUX0, 2, kINPUTMUX_GpioPort0Pin5ToSmartDma);
    INPUTMUX_Deinit(INPUTMUX0); /* Only needed during setup, save power. */

    BOARD_InitCameraPins();
    BOARD_InitArduinoLcdPins();
    /* Stage 4 (WORKLOG.md): microSD slot, same shared LPSPI1 bus as the
     * LCD - includes the real-hardware-confirmed SDI/DO pull-up fix (see
     * pin_mux.c's BOARD_InitSdCardPins(), a shared file - this project's
     * SD shield has no pull-up of its own). */
    BOARD_InitSdCardPins();
}
