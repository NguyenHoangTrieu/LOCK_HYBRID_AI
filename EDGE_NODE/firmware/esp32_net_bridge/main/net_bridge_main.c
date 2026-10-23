/*
 * net_bridge_main.c - app_main(): bring up WiFi, the backend HTTP client,
 * then start the SPI bridge to the MCXN947. See spi_bridge.c/protocol.h
 * for the actual device<->cloud relay logic - this file is just
 * sequencing.
 */
#include "backend_client.h"
#include "esp_log.h"
#include "spi_bridge.h"
#include "wifi_sta.h"

static const char *TAG = "net_bridge_main";

void app_main(void)
{
    ESP_LOGI(TAG, "LOCK_HYBRID_AI network bridge starting");

    WIFI_Init();
    BACKEND_Init();
    SPI_BRIDGE_Start();

    ESP_LOGI(TAG, "init complete - SPI bridge and WiFi tasks now own the rest of this device's runtime");
}
