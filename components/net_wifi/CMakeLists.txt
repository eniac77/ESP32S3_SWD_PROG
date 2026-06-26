idf_component_register(
    SRCS "net_wifi.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif nvs_flash esp_event mdns
)
