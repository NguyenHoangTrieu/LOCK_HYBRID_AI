/*
 * status_led.h - see status_led.c.
 */
#ifndef _STATUS_LED_H_
#define _STATUS_LED_H_

/*! @brief One-time init of the onboard addressable RGB LED (GPIO27 on
 *  ESP32-C5-DevKitC-1). Safe to call once at boot before using the
 *  other functions below. */
void STATUS_LED_Init(void);

/*! @brief Solid blue - this project's "WiFi setup/config mode" signal,
 *  set for as long as wifi_captive_portal.c's portal is running. */
void STATUS_LED_SetConfigMode(void);

/*! @brief Turns the LED off. */
void STATUS_LED_Off(void);

#endif /* _STATUS_LED_H_ */
