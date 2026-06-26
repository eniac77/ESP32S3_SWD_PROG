idf_component_register(
    SRCS "cortexm_debug.c"
    INCLUDE_DIRS "."
    REQUIRES adiv5 swd_phy esp_timer
)
