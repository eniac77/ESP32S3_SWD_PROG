idf_component_register(
    SRCS "avr_pdi.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer storage_lfs avr_image
)
