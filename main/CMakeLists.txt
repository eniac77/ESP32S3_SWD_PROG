idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        storage_lfs
        display_lcd
        input_enc
        ui
        net_wifi
        web_ui
        ftp_srv
        target_serial
        target_state
        prog_session
        target_db
        swd_phy
        adiv5
        cortexm_debug
        flm_runner
        flm_blobs
        avr_isp
)

# LittleFS image generálás: a ../data fa tartalma a "storage" partícióra kerül
# (lásd partitions.csv). A FLASH_IN_PROJECT miatt az "idf.py flash" automatikusan
# felírja a képet. A függvényt a joltwallet/littlefs project_include.cmake adja.
littlefs_create_partition_image(storage ../data FLASH_IN_PROJECT)
