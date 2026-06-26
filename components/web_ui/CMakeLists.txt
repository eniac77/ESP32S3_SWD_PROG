idf_component_register(
    SRCS "web_ui.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server storage_lfs target_state target_serial json esp_timer
)
