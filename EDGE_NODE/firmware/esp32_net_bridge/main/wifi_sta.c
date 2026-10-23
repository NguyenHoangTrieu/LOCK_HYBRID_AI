/*
 * wifi_sta.c - see wifi_sta.h. Standard esp_wifi station bring-up,
 * same shape as every ESP-IDF WiFi station example (event group signals
 * IP-acquired, auto-reconnect on disconnect up to a retry cap then gives
 * up until the next explicit event).
 */
#include "wifi_sta.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include <string.h>

static const char *TAG = "wifi_sta";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_MAX_RETRY      10

static EventGroupHandle_t s_wifiEventGroup;
static int s_retryCount = 0;
static volatile bool s_connected = false;

static void EventHandler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        s_connected = false;
        if (s_retryCount < WIFI_MAX_RETRY)
        {
            esp_wifi_connect();
            s_retryCount++;
            ESP_LOGW(TAG, "disconnected, retry %d/%d", s_retryCount, WIFI_MAX_RETRY);
        }
        else
        {
            ESP_LOGE(TAG, "giving up after %d retries - will retry again on the next reconnect poll", WIFI_MAX_RETRY);
        }
        xEventGroupClearBits(s_wifiEventGroup, WIFI_CONNECTED_BIT);
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        s_retryCount = 0;
        s_connected  = true;
        xEventGroupSetBits(s_wifiEventGroup, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "connected, IP acquired");
    }
}

void WIFI_Init(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t initConfig = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&initConfig));

    s_wifiEventGroup = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &EventHandler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &EventHandler, NULL));

    wifi_config_t wifiConfig = {0};
    strlcpy((char *)wifiConfig.sta.ssid, CONFIG_LOCK_HYBRID_WIFI_SSID, sizeof(wifiConfig.sta.ssid));
    strlcpy((char *)wifiConfig.sta.password, CONFIG_LOCK_HYBRID_WIFI_PASSWORD, sizeof(wifiConfig.sta.password));
    wifiConfig.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifiConfig));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to SSID '%s'...", CONFIG_LOCK_HYBRID_WIFI_SSID);
    xEventGroupWaitBits(s_wifiEventGroup, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(20000));
}

bool WIFI_IsConnected(void)
{
    return s_connected;
}
