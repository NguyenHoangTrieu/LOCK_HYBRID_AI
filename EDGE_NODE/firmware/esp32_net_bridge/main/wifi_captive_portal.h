/*
 * wifi_captive_portal.h - see wifi_captive_portal.c.
 */
#ifndef _WIFI_CAPTIVE_PORTAL_H_
#define _WIFI_CAPTIVE_PORTAL_H_

/*! @brief Starts the SoftAP + DNS-redirect + HTTP config-page portal and
 *  blocks forever (this device has nothing useful to do until it has
 *  real WiFi credentials). On a successful form submission, saves the
 *  credentials via esp_wifi (persisted to NVS the same way esp_wifi
 *  always does) and this project's own "provisioned" NVS flag, then
 *  reboots via esp_restart() - does not return. */
void WIFI_CAPTIVE_PORTAL_RunForever(void);

#endif /* _WIFI_CAPTIVE_PORTAL_H_ */
