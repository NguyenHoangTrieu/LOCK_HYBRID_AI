/*
 * status_led.c - see status_led.h.
 *
 * Drives the ESP32-C5-DevKitC-1's onboard addressable RGB LED (WS2812-
 * type, GPIO27 - confirmed from the board's own pinout docs: "GPIO27 |
 * Strapping pin and onboard RGB LED") via the espressif/led_strip
 * component's RMT backend, same pattern as that component's own
 * led_strip_rmt_ws2812 example.
 */
#include "status_led.h"

#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "status_led";

#define STATUS_LED_GPIO 27

static led_strip_handle_t s_ledStrip = NULL;

void STATUS_LED_Init(void)
{
    led_strip_config_t stripConfig = {
        .strip_gpio_num  = STATUS_LED_GPIO,
        .max_leds        = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model       = LED_MODEL_WS2812,
        .flags = {
            .invert_out = false,
        },
    };
    led_strip_rmt_config_t rmtConfig = {
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags = {
            .with_dma = false,
        },
    };

    esp_err_t err = led_strip_new_rmt_device(&stripConfig, &rmtConfig, &s_ledStrip);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "led_strip_new_rmt_device() failed: %s - status LED disabled", esp_err_to_name(err));
        s_ledStrip = NULL;
        return;
    }
    (void)led_strip_clear(s_ledStrip);
}

void STATUS_LED_SetConfigMode(void)
{
    if (s_ledStrip == NULL)
    {
        return;
    }
    /* Solid blue - this project's convention for "WiFi setup/config
     * mode", chosen to be visually distinct from any other on-device
     * indicator (the MCXN947 side uses green/amber/red for its own
     * granted/working/denied states - see state_machine.c). */
    (void)led_strip_set_pixel(s_ledStrip, 0, 0, 0, 255);
    (void)led_strip_refresh(s_ledStrip);
}

void STATUS_LED_Off(void)
{
    if (s_ledStrip == NULL)
    {
        return;
    }
    (void)led_strip_clear(s_ledStrip);
}
