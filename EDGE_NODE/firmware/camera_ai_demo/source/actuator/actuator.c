/*
 * actuator.c - see actuator.h.
 *
 * CTIMER0 match channel 0 (CT0_MAT0, the only CTIMER function routed to
 * P0_10 per the SDK's own frdmmcxn947 ctimer/simple_pwm example pin_mux.c
 * - ALT4) drives the PWM pulse; match channel 3 only defines the period
 * and is not pinned out anywhere (same split NXP's own simple_pwm.c
 * example uses). Clock: FRO_HF (48MHz) divided by a 48x prescaler inside
 * CTIMER0 down to an even 1MHz timer tick, so match-register ticks equal
 * microseconds directly - no fractional-tick rounding for the 1-2ms pulse
 * widths this needs.
 *
 * PWM semantics (confirmed by reading fsl_ctimer.c's CTIMER_SetupPwmPeriod,
 * not assumed): the pulse channel's match register is the HIGH-time tick
 * count directly - the pin goes high when the counter resets to 0 (at the
 * period channel's match) and low when the counter reaches the pulse
 * channel's match value. So MR[pulse channel] = desired pulse width in
 * microseconds, with the 1MHz tick rate above.
 */

#include "actuator.h"
#include "app.h"
#include "fsl_clock.h"
#include "fsl_ctimer.h"
#include "fsl_debug_console.h"
#include "fsl_port.h"

#define ACTUATOR_CTIMER_BASE   CTIMER0
#define ACTUATOR_PULSE_CHANNEL kCTIMER_Match_0 /* CT0_MAT0 = P0_10, the only pinned-out choice */
#define ACTUATOR_PERIOD_CHANNEL kCTIMER_Match_3 /* not pinned out - period-only, per CTIMER_SetupPwmPeriod's requirement that this differ from the pulse channel */

#define ACTUATOR_TIMER_TICK_HZ 1000000U /* 1 tick = 1us, see file header comment */
#define ACTUATOR_PWM_FREQ_HZ   50U      /* standard analog/hobby servo PWM rate */
#define ACTUATOR_PERIOD_TICKS  (ACTUATOR_TIMER_TICK_HZ / ACTUATOR_PWM_FREQ_HZ) /* 20000 ticks = 20ms */

/* SG90-style generic defaults - NOT measured against the real servo on
 * this board. Confirm SIG/VCC/GND wiring and these two values on real
 * hardware before trusting them (see actuator.h). */
#define ACTUATOR_LOCKED_PULSE_US   1000U
#define ACTUATOR_UNLOCKED_PULSE_US 1500U

static bool s_unlocked = false;

static void ACTUATOR_SetPulseUs(uint32_t pulseUs)
{
    (void)CTIMER_SetupPwmPeriod(ACTUATOR_CTIMER_BASE, ACTUATOR_PERIOD_CHANNEL, ACTUATOR_PULSE_CHANNEL,
                                ACTUATOR_PERIOD_TICKS - 1U, pulseUs, false);
}

void ACTUATOR_Init(void)
{
    /* Take P0_10 from its pin_mux.c boot-default (plain GPIO, idle high -
     * see actuator.h) over to CT0_MAT0. Only from here on can the pin
     * actually pulse; everything before this call is the safe idle state. */
    const port_pin_config_t pwmPinConfig = {
        .pullSelect          = kPORT_PullDisable,
        .pullValueSelect     = kPORT_LowPullResistor,
        .slewRate            = kPORT_FastSlewRate,
        .passiveFilterEnable = kPORT_PassiveFilterDisable,
        .openDrainEnable     = kPORT_OpenDrainDisable,
        .driveStrength       = kPORT_LowDriveStrength,
        .mux                 = kPORT_MuxAlt4, /* CT0_MAT0, confirmed against the SDK's own frdmmcxn947 ctimer examples */
        .inputBuffer         = kPORT_InputBufferEnable,
        .invertInput         = kPORT_InputNormal,
        .lockRegister        = kPORT_UnlockRegister,
    };
    PORT_SetPinConfig(DEMO_SERVO_PWM_PORT, DEMO_SERVO_PWM_PIN, &pwmPinConfig);

    CLOCK_SetClkDiv(kCLOCK_DivCtimer0Clk, 1U);
    CLOCK_AttachClk(kFRO_HF_to_CTIMER0);

    ctimer_config_t config;
    CTIMER_GetDefaultConfig(&config);
    /* FRO_HF is 48MHz on this board (see hardware_init.c's other clock
     * comments) - prescale 47 (divide by 48) gives an even 1MHz tick. */
    config.prescale = 47U;
    CTIMER_Init(ACTUATOR_CTIMER_BASE, &config);

    /* Locked before the timer starts - no unlock pulse can reach the
     * servo ahead of this (SR-6). */
    ACTUATOR_SetPulseUs(ACTUATOR_LOCKED_PULSE_US);
    s_unlocked = false;
    CTIMER_StartTimer(ACTUATOR_CTIMER_BASE);

    PRINTF("Actuator: servo PWM on P0_10 (CT0_MAT0), locked\r\n");
}

void ACTUATOR_Unlock(void)
{
    ACTUATOR_SetPulseUs(ACTUATOR_UNLOCKED_PULSE_US);
    s_unlocked = true;
}

void ACTUATOR_Lock(void)
{
    ACTUATOR_SetPulseUs(ACTUATOR_LOCKED_PULSE_US);
    s_unlocked = false;
}

bool ACTUATOR_IsUnlocked(void)
{
    return s_unlocked;
}
