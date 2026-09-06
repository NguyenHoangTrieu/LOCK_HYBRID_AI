/*
 * hardware_init.c - Camera_AI_Test1 (FRDM-MCXN947)
 *
 * Camera clock bring-up copied from NXP's smartdma_camera_flexio_mculcd
 * board port. LCD pin init calls whichever of
 * BOARD_InitArduinoLcdPins()/BOARD_InitFlexioPins() (pin_mux.c)
 * DEMO_LCD_ARDUINO_HEADER (app.h) selects - Arduino header is the current
 * default.
 */

#include "pin_mux.h"
#include "clock_config.h"
#include "board.h"
#include "app.h"
#include "fsl_clock.h"
#include "fsl_inputmux.h"
#include "fsl_spc.h"

#ifndef DUALCORE_RTOS
/* USB Video Class streaming is abandoned and not carried forward into the
 * dual-core RTOS build - see ARCHITECTURE.md Sec.4/WORKLOG.md. Guarded so
 * this file can be shared between the legacy single-core build and the
 * dual-core Stage 1+ bring-up (-DDUALCORE_RTOS=1, see CMakeLists.txt)
 * without needing source/usb/ on the include path in the latter. */
#include "usb_device_config.h"
#include "usb.h"
#include "usb_device.h"
#include "usb_device_class.h"
#include "usb_video_camera.h"
#include "usb_phy.h"
#endif /* DUALCORE_RTOS */

/*
 * CONFIRMED (see WORKLOG.md): SmartDMA camera capture only runs reliably
 * with DCDC_VDD_LVL at Mid (1.0V) - any other level stops it after ~2
 * frames, silently. USB HS PHY needs DCDC_VDD_LVL at Overdrive
 * (CLOCK_EnableUsbhsPhyPllClock() spins forever in PLL_LOCK otherwise).
 * Since both can't be true at once, these two helpers isolate just the
 * regulator-level switch (no USB clock/PHY bring-up, no camera re-init)
 * so callers can flip between them repeatedly - see main()'s
 * periodic-refresh loop.
 */
void BOARD_SetRegulatorsMidVoltage(void)
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

void BOARD_SetRegulatorsOverdriveVoltage(void)
{
    /* Do NOT raise CORELDO_VDD_LVL alone without DCDC_VDD_LVL - left the
     * chip's SWD/debug port completely unreachable on real hardware (see
     * WORKLOG.md). Always raise both together, or neither. */
    spc_active_mode_dcdc_option_t usbDcdcOpt = {
        .DCDCVoltage       = kSPC_DCDC_OverdriveVoltage,
        .DCDCDriveStrength = kSPC_DCDC_NormalDriveStrength,
    };
    SPC_SetActiveModeDCDCRegulatorConfig(SPC0, &usbDcdcOpt); /* waits for SPC_SC_BUSY internally */

    spc_active_mode_core_ldo_option_t usbLdoOpt = {
        .CoreLDOVoltage       = kSPC_CoreLDO_OverDriveVoltage,
        .CoreLDODriveStrength = kSPC_CoreLDO_NormalDriveStrength,
    };
    SPC_SetActiveModeCoreLDORegulatorConfig(SPC0, &usbLdoOpt);
    while (SPC0->SC & SPC_SC_BUSY_MASK)
    {
    }
}

#ifndef DUALCORE_RTOS
/*
 * USB High-Speed (EHCI + dedicated HS PHY) bring-up for the UVC camera
 * stream (source/usb/usb_video_camera.c). Copied near-verbatim from the
 * SDK board port for this exact board+example - MCXN947-specific
 * SPC/SCG/SYSCON register bring-up for the USB HS PHY's PLL.
 *
 * Call exactly ONCE, from main() after CAMERA_CAPTURE_Deinit() - camera
 * capture and USB HS can't run at the same time on this chip (see
 * BOARD_SetRegulatorsOverdriveVoltage() above). Unlike the two regulator
 * helpers above, don't call this repeatedly.
 *
 * Not carried into the dual-core RTOS build - see the include guard above.
 */
void USB_DeviceClockInit(void)
{
    usb_phy_config_struct_t phyConfig = {
        BOARD_USB_PHY_D_CAL,
        BOARD_USB_PHY_TXCAL45DP,
        BOARD_USB_PHY_TXCAL45DM,
    };

    SPC0->ACTIVE_VDELAY = 0x0500;
    BOARD_SetRegulatorsOverdriveVoltage();

    SPC0->ACTIVE_CFG |= SPC_ACTIVE_CFG_SYSLDO_VDD_DS_MASK;

    if (0u == (SCG0->LDOCSR & SCG_LDOCSR_LDOEN_MASK))
    {
        SCG0->TRIM_LOCK = 0x5a5a0001U;
        SCG0->LDOCSR |= SCG_LDOCSR_LDOEN_MASK;
        while (0U == (SCG0->LDOCSR & SCG_LDOCSR_VOUT_OK_MASK))
        {
        }
    }
    SYSCON->AHBCLKCTRLSET[2] |= SYSCON_AHBCLKCTRL2_USB_HS_MASK | SYSCON_AHBCLKCTRL2_USB_HS_PHY_MASK;
    SCG0->SOSCCFG &= ~(SCG_SOSCCFG_RANGE_MASK | SCG_SOSCCFG_EREFS_MASK);
    /* xtal = 20 ~ 30MHz */
    SCG0->SOSCCFG = (1U << SCG_SOSCCFG_RANGE_SHIFT) | (1U << SCG_SOSCCFG_EREFS_SHIFT);
    SCG0->SOSCCSR |= SCG_SOSCCSR_SOSCEN_MASK;
    while (0U == (SCG0->SOSCCSR & SCG_SOSCCSR_SOSCVLD_MASK))
    {
    }
    SYSCON->CLOCK_CTRL |= SYSCON_CLOCK_CTRL_CLKIN_ENA_MASK | SYSCON_CLOCK_CTRL_CLKIN_ENA_FM_USBH_LPT_MASK;
    CLOCK_EnableClock(kCLOCK_UsbHs);
    CLOCK_EnableClock(kCLOCK_UsbHsPhy);
    CLOCK_EnableUsbhsPhyPllClock(kCLOCK_Usbphy480M, 24000000U);
    CLOCK_EnableUsbhsClock();
    USB_EhciPhyInit(CONTROLLER_ID, BOARD_XTAL0_CLK_HZ, &phyConfig);
}

void USB1_HS_IRQHandler(void)
{
    extern usb_video_virtual_camera_struct_t g_UsbDeviceVideoVirtualCamera;
    USB_DeviceEhciIsrFunction(g_UsbDeviceVideoVirtualCamera.deviceHandle);
}

void USB_DeviceIsrEnable(void)
{
    uint8_t usbDeviceEhciIrq[] = USBHS_IRQS;
    uint8_t irqNumber          = usbDeviceEhciIrq[CONTROLLER_ID - kUSB_ControllerEhci0];

    NVIC_SetPriority((IRQn_Type)irqNumber, USB_DEVICE_INTERRUPT_PRIORITY);
    EnableIRQ((IRQn_Type)irqNumber);
}
#endif /* DUALCORE_RTOS */

void BOARD_InitHardware(void)
{
    BOARD_InitBootPins();
    BOARD_InitBootClocks();
    BOARD_InitDebugConsole();

    /* Boot at Mid voltage - main() switches to Overdrive itself later,
     * only after camera capture has produced a few frames and stopped
     * for good (or drops back to Mid briefly each periodic-refresh
     * cycle). */
    BOARD_SetRegulatorsMidVoltage();

    /* Camera XCLK: route a clock out through CLKOUT (P2_2) to the OV7670's
     * XCLK pin. Must be an exact match for camera_capture.c's declared
     * `.xclock = kOV7670_InputClock24MHZ` - fsl_ov7670.c picks its
     * CLKRC/timing values from a lookup table keyed on that declared rate
     * and has no way to detect the real one, so feeding it the wrong rate
     * silently runs the whole capture cycle slower (confirmed on real
     * hardware: MAIN_CLK/25 gives 6MHz, not 24MHz, and no integer divisor
     * of MAIN_CLK (150MHz) lands on any rate this sensor's lookup table
     * supports - see WORKLOG.md). Fix: source CLKOUT from FRO_HF (48MHz)
     * instead - FRO_HF/2 = exactly 24MHz. */
    CLOCK_AttachClk(kFRO_HF_to_CLKOUT);
    CLOCK_SetClkDiv(kCLOCK_DivClkOut, 2U);

    /* FlexIO clock, for the J8 8-bit parallel LCD bus - divided down from
     * PLL0 (150MHz) to 37.5MHz for DEMO_FLEXIO_BAUDRATE_BPS's divider
     * range. Below ~3MHz has hung LCD_Init() on this panel (WORKLOG.md). */
    CLOCK_SetClkDiv(kCLOCK_DivFlexioClk, 4u);
    CLOCK_AttachClk(kPLL0_to_FLEXIO);

    /* GPIO clocks for the LCD pins (GPIO0: Arduino header; GPIO4: J8
     * header RST/BLK). GPIO1 kept on defensively, harmless if unused. */
    CLOCK_EnableClock(kCLOCK_Gpio0);
    CLOCK_EnableClock(kCLOCK_Gpio1);
    CLOCK_EnableClock(kCLOCK_Gpio4);

    /* Camera I2C (SCCB) clock. */
    CLOCK_AttachClk(kFRO12M_to_FLEXCOMM7);
    CLOCK_EnableClock(kCLOCK_LPFlexComm7);
    CLOCK_EnableClock(kCLOCK_LPI2c7);
    CLOCK_SetClkDiv(kCLOCK_DivFlexcom7Clk, 1u);

    /* Shared LPSPI1 bus (Arduino D10..D13 - LCD/microSD/touch, see
     * source/spi1_bus.h). Just attach+divide the source clock - spi1_bus.c
     * gates/resets the peripheral itself. FRO_HF (48MHz), not FRO12M -
     * FRO12M caps the achievable SPI baud at ~6MHz (confirmed on real
     * hardware, see WORKLOG.md), too slow for a usable LCD frame rate.
     * FRO_HF is already running (feeds PLL0) and unused elsewhere. */
    CLOCK_AttachClk(kFRO_HF_DIV_to_FLEXCOMM1);
    CLOCK_SetClkDiv(kCLOCK_DivFlexcom1Clk, 1u);

    /* Route camera VSYNC/HSYNC/PCLK (P0_4/P0_11/P0_5) to the SmartDMA. */
    INPUTMUX_Init(INPUTMUX0);
    INPUTMUX_AttachSignal(INPUTMUX0, 0, kINPUTMUX_GpioPort0Pin4ToSmartDma);
    INPUTMUX_AttachSignal(INPUTMUX0, 1, kINPUTMUX_GpioPort0Pin11ToSmartDma);
    INPUTMUX_AttachSignal(INPUTMUX0, 2, kINPUTMUX_GpioPort0Pin5ToSmartDma);
    INPUTMUX_Deinit(INPUTMUX0); /* Only needed during setup, save power. */

    BOARD_InitCameraPins();
#if DEMO_LCD_ARDUINO_HEADER
    BOARD_InitArduinoLcdPins();
    BOARD_InitTouchPins(); /* XPT2046 touch controller - Arduino header only, see pin_mux.c. */
#else
    BOARD_InitFlexioPins();
#endif
    BOARD_InitSdCardPins();
}

#ifdef CORE1_IMAGE_COPY_TO_RAM
/* core1_image_size (components/misc_utilities/fsl_incbin.S) is the actual
 * symbol - see app.h's comment for the whole embed mechanism. */
uint32_t get_core1_image_size(void)
{
    return (uint32_t)core1_image_size;
}
#endif
