idf_component_register(
    SRCS "ui.c"
    INCLUDE_DIRS "."
    REQUIRES display_lcd input_enc storage_lfs prog_session avr_isp
             target_state esp_lvgl_port lvgl freertos
)
