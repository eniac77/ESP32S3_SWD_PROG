idf_component_register(
    SRCS "adiv5.c"
    INCLUDE_DIRS "."
    REQUIRES swd_phy esp_timer
)
