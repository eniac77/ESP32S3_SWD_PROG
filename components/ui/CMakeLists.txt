idf_component_register(
    SRCS "ui.c"
    INCLUDE_DIRS "."
    REQUIRES display_oled input_enc storage_lfs prog_session freertos
)
