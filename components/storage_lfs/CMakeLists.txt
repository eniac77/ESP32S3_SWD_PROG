idf_component_register(
    SRCS "storage_lfs.c" "storage_src.c"
    INCLUDE_DIRS "."
    REQUIRES littlefs storage_usb
)
