/*
 * wifi_sta.h - WiFi station bring-up. No SSID/password compiled in - the
 * site's WiFi credentials are set at runtime via a SoftAP captive portal
 * (wifi_captive_portal.c) the first time the device boots unprovisioned,
 * then persisted in NVS by esp_wifi itself for every boot after that.
 * Press the BOOT button (GPIO28 on ESP32-C5-DevKitC-1) at any time to
 * toggle setup mode: in normal mode it reboots into the setup captive
 * portal (not erasing esp_wifi's stored SSID/password, just the
 * "provisioned" flag); in setup mode it reboots back into normal mode
 * using whatever SSID/password are already stored, no need to resubmit
 * the form. See wifi_sta.c's ButtonMonitorTask().
 */
#ifndef _WIFI_STA_H_
#define _WIFI_STA_H_

#include <stdbool.h>

/*! @brief Connects using stored credentials if already provisioned, or
 *  never returns (runs the captive portal and reboots once credentials
 *  are submitted) if not. Safe to call once at boot; WIFI_IsConnected()
 *  is what the rest of the app should poll afterwards, since a running
 *  link can still drop. */
void WIFI_Init(void);

bool WIFI_IsConnected(void);

#endif /* _WIFI_STA_H_ */
