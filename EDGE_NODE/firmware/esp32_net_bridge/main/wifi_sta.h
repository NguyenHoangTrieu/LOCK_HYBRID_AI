/*
 * wifi_sta.h - WiFi station bring-up. SSID/password come from Kconfig
 * (`idf.py menuconfig` -> "LOCK_HYBRID_AI Edge Node Bridge") - per-site
 * credentials, not compiled defaults meant to be shipped as-is.
 */
#ifndef _WIFI_STA_H_
#define _WIFI_STA_H_

#include <stdbool.h>

/*! @brief Blocks until connected (or the internal retry budget is
 *  exhausted). Safe to call once at boot; WIFI_IsConnected() is what the
 *  rest of the app should poll afterwards, since a running link can
 *  still drop. */
void WIFI_Init(void);

bool WIFI_IsConnected(void);

#endif /* _WIFI_STA_H_ */
