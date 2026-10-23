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

/* SG90-style generic defaults:
 * 1.0ms (1000us) = 0 deg (locked)
 * 2.0ms (2000us) = ~90/180 deg (unlocked)
 * 50Hz PWM rate = 20ms period (20000us)
 */
#define ACTUATOR_LOCKED_PULSE_US   1000U
#define ACTUATOR_UNLOCKED_PULSE_US 2000U

static bool s_unlocked = false;

static void ACTUATOR_SetPulseUs(uint32_t pulseUs)
{
    if (pulseUs > ACTUATOR_PERIOD_TICKS)
    {
        pulseUs = ACTUATOR_PERIOD_TICKS;
    }
    /* LPC/MCX CTIMER in PWM mode (PWMC bit set) drives the match pin LOW
     * when the counter matches the period and resets to 0.
     * The pin drives HIGH when the counter reaches MR[pulseChannel].
     * Therefore, HIGH time duration = ACTUATOR_PERIOD_TICKS - MR[pulseChannel].
     * To obtain a HIGH pulse width of pulseUs microseconds:
     * MR[pulseChannel] must be set to (ACTUATOR_PERIOD_TICKS - pulseUs).
     */
    uint32_t pulseMatchVal = ACTUATOR_PERIOD_TICKS - pulseUs;
    (void)CTIMER_SetupPwmPeriod(ACTUATOR_CTIMER_BASE, ACTUATOR_PERIOD_CHANNEL, ACTUATOR_PULSE_CHANNEL,
                                ACTUATOR_PERIOD_TICKS - 1U, pulseMatchVal, false);
}

void ACTUATOR_Init(void)
{
    /* Take P0_10 (Arduino D9 / J2-4) to CT0_MAT0 */
    const port_pin_config_t pwmPinConfig = {
        .pullSelect          = kPORT_PullDisable,
        .pullValueSelect     = kPORT_LowPullResistor,
        .slewRate            = kPORT_FastSlewRate,
        .passiveFilterEnable = kPORT_PassiveFilterDisable,
        .openDrainEnable     = kPORT_OpenDrainDisable,
        .driveStrength       = kPORT_HighDriveStrength,
        .mux                 = kPORT_MuxAlt4, /* CT0_MAT0 */
        .inputBuffer         = kPORT_InputBufferEnable,
        .invertInput         = kPORT_InputNormal,
        .lockRegister        = kPORT_UnlockRegister,
    };
    PORT_SetPinConfig(DEMO_SERVO_PWM_PORT, DEMO_SERVO_PWM_PIN, &pwmPinConfig);

    CLOCK_SetClkDiv(kCLOCK_DivCtimer0Clk, 1U);
    CLOCK_AttachClk(kFRO_HF_to_CTIMER0);

    uint32_t ctimerClk = CLOCK_GetCTimerClkFreq(0);
    ctimer_config_t config;
    CTIMER_GetDefaultConfig(&config);
    if (ctimerClk >= 1000000U)
    {
        config.prescale = (ctimerClk / 1000000U) - 1U;
    }
    else
    {
        config.prescale = 47U;
    }
    CTIMER_Init(ACTUATOR_CTIMER_BASE, &config);

    ACTUATOR_SetPulseUs(ACTUATOR_LOCKED_PULSE_US);
    s_unlocked = false;
    CTIMER_StartTimer(ACTUATOR_CTIMER_BASE);

    PRINTF("Actuator: servo PWM on P0_10 (CT0_MAT0 / Arduino D9 / J2-4), clock=%lu Hz, prescale=%u, locked (%u us pulse)\r\n",
           ctimerClk, (unsigned int)config.prescale, (unsigned int)ACTUATOR_LOCKED_PULSE_US);
}

void ACTUATOR_Unlock(void)
{
    ACTUATOR_SetPulseUs(ACTUATOR_UNLOCKED_PULSE_US);
    s_unlocked = true;
    PRINTF("Actuator: UNLOCKED (%u us pulse)\r\n", (unsigned int)ACTUATOR_UNLOCKED_PULSE_US);
}

void ACTUATOR_Lock(void)
{
    ACTUATOR_SetPulseUs(ACTUATOR_LOCKED_PULSE_US);
    s_unlocked = false;
    PRINTF("Actuator: LOCKED (%u us pulse)\r\n", (unsigned int)ACTUATOR_LOCKED_PULSE_US);
}

bool ACTUATOR_IsUnlocked(void)
{
    return s_unlocked;
}
