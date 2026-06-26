idf_component_register(
    SRCS "target_serial.c"
    INCLUDE_DIRS "."
    REQUIRES driver target_state storage_lfs esp_timer freertos
)
