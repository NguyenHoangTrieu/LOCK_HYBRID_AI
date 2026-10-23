/*
 * spi_bridge.h - SPI slave side of the MCXN947<->ESP32 link. See
 * protocol.h for the wire format and why it's shaped this way (no
 * handshake GPIO on this board).
 */
#ifndef _SPI_BRIDGE_H_
#define _SPI_BRIDGE_H_

/*! @brief Starts the SPI slave transceive loop and the backend-request
 *  worker, both as their own FreeRTOS tasks. Call after WIFI_Init() and
 *  BACKEND_Init(). Never returns control to the caller in the sense of
 *  tearing down - this owns the rest of app_main()'s lifetime via its
 *  tasks. */
void SPI_BRIDGE_Start(void);

#endif /* _SPI_BRIDGE_H_ */
