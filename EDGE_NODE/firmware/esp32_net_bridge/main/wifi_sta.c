/*
 * wifi_sta.c - see wifi_sta.h.
 *
 * Standard esp_wifi station bring-up (event group signals IP-acquired,
 * auto-reconnect on disconnect up to a retry cap) plus a one-time SoftAP
 * captive portal (wifi_captive_portal.c) for setting the site's real WiFi
 * SSID/password without reflashing - see that file for why a plain
 * browser-facing captive portal was used instead of the
 * network_provisioning component (that one targets Espressif's companion
 * app/CLI, not a browser auto-popup).
 *
 * No WiFi SSID/password is compiled into this firmware. "Provisioned"
 * state is tracked in this project's own NVS flag (wifi_cfg/done) -
 * esp_wifi persists the actual SSID/password to its own NVS storage the
 * moment wifi_captive_portal.c calls esp_wifi_set_config(), so every boot
 * after that just starts the station and connects.
 */
#include "wifi_sta.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "status_led.h"
#include "wifi_captive_portal.h"
#include <string.h>

static const char *TAG = "wifi_sta";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_MAX_RETRY      10

#define WIFI_PROV_NVS_NAMESPACE "wifi_cfg"
#define WIFI_PROV_NVS_DONE_KEY  "done"

/* ESP32-C5-DevKitC-1's onboard BOOT button - confirmed from the board's
 * schematic/pinout docs (GPIO28, "Boot strapping pin and BOOT button").
 * Only read as a plain GPIO input here, well after the bootloader's own
 * strap-sampling at power-on has already finished - same pattern every
 * ESP-IDF "hold BOOT to factory-reset" example uses. */
#define WIFI_PROV_RESET_BUTTON_GPIO 28

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

static void ClearProvisioned(void);
static void SetProvisioned(void);

/* Set right before WIFI_Init() falls into WIFI_CAPTIVE_PORTAL_RunForever()
 * (that call never returns on its own - it only exits via esp_restart()),
 * so ButtonMonitorTask can tell which of the two meanings a press should
 * have: enter setup mode, or - if already in it - leave it again. */
static volatile bool s_inSetupMode = false;

/* No external resistor needed - the board ties this pin to the BOOT
 * button (pressed = pulled low) and the internal pull-up is enough to
 * read a stable HIGH when not pressed. */
static void ConfigureButtonGpio(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << WIFI_PROV_RESET_BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cfg);
}

#define BUTTON_POLL_MS      50U
#define BUTTON_DEBOUNCE_MS 150U

/* Polls the BOOT button continuously for as long as the device is
 * running (not just at power-on) - a plain tap is enough to toggle WiFi
 * setup mode, no need to hold it through a reset the way the first
 * version of this required (confirmed confusing on real use: a user
 * naturally tried "just press BOOT" first, like every other ESP32 BOOT-
 * button convention, not realizing this code only checked the pin once
 * at the very start of WIFI_Init() and had long since moved past that
 * check by the time they pressed it). Debounces by requiring the level
 * to still read low after BUTTON_DEBOUNCE_MS, same threshold a single
 * accidental bump of the board wouldn't hold.
 *
 * A press toggles rather than always entering setup: normal mode ->
 * setup (clears the "provisioned" flag only - esp_wifi's own stored
 * SSID/password are untouched), setup -> normal (restores the flag so
 * the next boot reconnects with whatever SSID/password are already
 * sitting in esp_wifi's NVS, without making the user re-type them into
 * the portal). Confirmed on real hardware that without this, pressing
 * BOOT again while already in setup mode just re-cleared the same flag
 * and rebooted straight back into setup - there was no way back to
 * normal mode short of resubmitting the form. */
static void ButtonMonitorTask(void *arg)
{
    (void)arg;
    for (;;)
    {
        if (gpio_get_level(WIFI_PROV_RESET_BUTTON_GPIO) == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
            if (gpio_get_level(WIFI_PROV_RESET_BUTTON_GPIO) == 0)
            {
                if (s_inSetupMode)
                {
                    ESP_LOGW(TAG, "BOOT button pressed - leaving setup mode, rebooting into normal mode with previously stored WiFi credentials");
                    SetProvisioned();
                }
                else
                {
                    ESP_LOGW(TAG, "BOOT button pressed - rebooting into WiFi setup mode");
                    ClearProvisioned();
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

static bool IsProvisioned(void)
{
    nvs_handle_t nvsHandle;
    if (nvs_open(WIFI_PROV_NVS_NAMESPACE, NVS_READONLY, &nvsHandle) != ESP_OK)
    {
        return false; /* namespace never created yet == never provisioned */
    }
    uint8_t done = 0U;
    esp_err_t err = nvs_get_u8(nvsHandle, WIFI_PROV_NVS_DONE_KEY, &done);
    nvs_close(nvsHandle);
    return (err == ESP_OK) && (done != 0U);
}

static void ClearProvisioned(void)
{
    nvs_handle_t nvsHandle;
    if (nvs_open(WIFI_PROV_NVS_NAMESPACE, NVS_READWRITE, &nvsHandle) == ESP_OK)
    {
        (void)nvs_erase_key(nvsHandle, WIFI_PROV_NVS_DONE_KEY);
        (void)nvs_commit(nvsHandle);
        nvs_close(nvsHandle);
    }
}

/* Re-sets the "provisioned" flag without touching esp_wifi's own stored
 * SSID/password - used by the setup-mode -> normal-mode toggle, which
 * relies on those credentials still being whatever was last saved (or
 * never set, if the device has never been through the portal at all -
 * in that case the next boot just fails to connect with an empty SSID,
 * and a BOOT press from there falls back to ClearProvisioned() again). */
static void SetProvisioned(void)
{
    nvs_handle_t nvsHandle;
    if (nvs_open(WIFI_PROV_NVS_NAMESPACE, NVS_READWRITE, &nvsHandle) == ESP_OK)
    {
        (void)nvs_set_u8(nvsHandle, WIFI_PROV_NVS_DONE_KEY, 1U);
        (void)nvs_commit(nvsHandle);
        nvs_close(nvsHandle);
    }
}

void WIFI_Init(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap(); /* needed if the captive portal runs below - cheap to always create */

    wifi_init_config_t initConfig = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&initConfig));

    ConfigureButtonGpio();
    xTaskCreate(ButtonMonitorTask, "wifi_prov_btn", 2560, NULL, 5, NULL);

    if (!IsProvisioned())
    {
        ESP_LOGI(TAG, "not provisioned - starting WiFi setup captive portal");
        s_inSetupMode = true; /* lets ButtonMonitorTask know a press now means "leave setup", not "enter setup" */
        WIFI_CAPTIVE_PORTAL_RunForever(); /* does not return - reboots once credentials are saved, or BOOT toggles back to normal mode */
    }

    /* Registered only once provisioned, not up above - EventHandler's
     * WIFI_EVENT_STA_START case calls esp_wifi_connect() unconditionally,
     * and the captive portal above also brings the STA netif up (via
     * esp_wifi_set_mode(WIFI_MODE_APSTA) in SavePostHandler()) to write
     * the new config into. Registering this handler before that point let
     * it fire esp_wifi_connect() with the STA interface's still-default
     * config the instant APSTA mode started it, which put the driver into
     * "connecting" state a few lines before SavePostHandler()'s own
     * esp_wifi_set_config(WIFI_IF_STA, ...) call - confirmed on real
     * hardware as the exact cause of that call's ESP_ERR_WIFI_STATE abort
     * every time a form was submitted. */
    s_wifiEventGroup = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &EventHandler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &EventHandler, NULL));

    /* The WS2812 keeps displaying whatever color it last got, independent
     * of the MCU resetting - a toggle back from setup mode (solid blue)
     * is a soft esp_restart(), not a power cycle, so without this the LED
     * would stay lit blue in normal mode too. */
    STATUS_LED_Init();
    STATUS_LED_Off();

    ESP_LOGI(TAG, "already provisioned - connecting with stored credentials");
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting...");
    xEventGroupWaitBits(s_wifiEventGroup, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(20000));
}

bool WIFI_IsConnected(void)
{
    return s_connected;
}
