/*
 * actuator.h - door latch servo, CT0_MAT0 (Arduino D9 / P0_10).
 *
 * Satisfies SRS HI-3/HI-4/SR-6: drives the latch to unlocked/locked and
 * defaults to locked. P0_10 is left muxed as plain GPIO, driven idle-high,
 * by BOARD_InitBaseboardPeripheralPins() (pin_mux.c) until ACTUATOR_Init()
 * runs - no PWM pulses reach the servo between boot and this driver
 * explicitly taking the pin over, which is itself a safe idle state, not
 * just a convenient one (SR-6: no fault/reset state should produce an
 * unlock pulse).
 *
 * Pulse-width values (ACTUATOR_LOCKED_PULSE_US / ACTUATOR_UNLOCKED_PULSE_US)
 * are generic SG90-style defaults (1.0ms/1.5ms, cleanly inside the common
 * 1-2ms range) - not measured against the real servo wired to this board.
 * Per the Library README (Servo header note), confirm SIG/VCC/GND pin
 * order against the actual servo and tune these two constants on real
 * hardware before relying on them.
 */
#ifndef _ACTUATOR_H_
#define _ACTUATOR_H_

#include <stdbool.h>
#include <stdint.h>

/*! @brief Configure CTIMER0 for 50Hz servo PWM on P0_10 and drive the
 *  latch to the locked position immediately. Call once after
 *  BOARD_InitHardware(). */
void ACTUATOR_Init(void);

/*! @brief Drive the latch to the unlocked position. Caller (the state
 *  machine) is responsible for the hold duration and for calling
 *  ACTUATOR_Lock() afterwards (FR-16) - this function only sets the pulse
 *  width, it does not block or time anything itself. */
void ACTUATOR_Unlock(void);

/*! @brief Drive the latch to the locked position. Safe to call at any
 *  time, including before ACTUATOR_Init() has run on a given boot (the
 *  pin's boot-default GPIO idle-high state already means "not pulsing /
 *  not actively unlocking"). */
void ACTUATOR_Lock(void);

/*! @brief Whether the latch is currently commanded unlocked. Reflects the
 *  last ACTUATOR_Unlock()/ACTUATOR_Lock() call, not a physical sensor -
 *  this board has no latch position feedback. */
bool ACTUATOR_IsUnlocked(void);

#endif /* _ACTUATOR_H_ */
