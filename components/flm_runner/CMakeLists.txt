idf_component_register(
    SRCS "flm_runner.c"
    INCLUDE_DIRS "."
    REQUIRES flm_blobs cortexm_debug adiv5 esp_timer
)
