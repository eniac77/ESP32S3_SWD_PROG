idf_component_register(
    SRCS "avr_isp.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer storage_lfs
)
