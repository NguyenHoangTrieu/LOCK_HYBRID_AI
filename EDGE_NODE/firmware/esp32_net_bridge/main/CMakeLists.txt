idf_component_register(
    SRCS "net_bridge_main.c" "wifi_sta.c" "backend_client.c" "spi_bridge.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_client esp_driver_gpio esp_driver_spi esp_timer
    PRIV_REQUIRES cjson
)
