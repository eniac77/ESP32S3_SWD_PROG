idf_component_register(
    SRCS "avr_updi.c"
    INCLUDE_DIRS "."
    REQUIRES driver storage_lfs avr_image
)
