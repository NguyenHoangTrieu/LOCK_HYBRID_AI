idf_component_register(
    SRCS "net_bridge_main.c" "wifi_sta.c" "wifi_captive_portal.c" "status_led.c" "backend_client.c" "spi_bridge.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_client esp_driver_gpio esp_driver_spi esp_timer esp_http_server
    PRIV_REQUIRES cjson dns_server led_strip
)
