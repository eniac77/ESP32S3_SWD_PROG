idf_component_register(
    SRCS "prog_session.c"
    INCLUDE_DIRS "."
    REQUIRES cortexm_debug adiv5 target_db flm_runner flm_blobs storage_lfs swd_phy esp_timer freertos
)
