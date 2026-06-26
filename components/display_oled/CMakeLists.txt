idf_component_register(
    SRCS
        "display_oled.c"
        "oled_fonts.c"
    INCLUDE_DIRS "."
    REQUIRES driver
)
