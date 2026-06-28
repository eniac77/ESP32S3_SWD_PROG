idf_component_register(
    SRCS "web_ui.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server storage_lfs target_state target_serial prog_session avr_isp json esp_timer
    EMBED_FILES
        "../../data/www/index.html"
        "../../data/www/app.css"
        "../../data/www/app.js"
)
