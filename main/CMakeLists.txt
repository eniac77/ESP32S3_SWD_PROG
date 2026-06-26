idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        storage_lfs
        display_oled
        input_enc
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
)
