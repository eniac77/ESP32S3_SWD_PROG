idf_component_register(
    SRCS "ftp_srv.c"
    INCLUDE_DIRS "."
    REQUIRES storage_lfs lwip esp_netif freertos
)
