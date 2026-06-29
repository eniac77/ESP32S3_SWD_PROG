idf_component_register(
    SRCS "storage_usb.c"
    INCLUDE_DIRS "."
    REQUIRES usb fatfs driver usb_host_msc
)
