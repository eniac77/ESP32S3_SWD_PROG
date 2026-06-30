idf_component_register(
    SRCS "display_lcd.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_lcd
        esp_lvgl_port
        lvgl
        esp_lcd_ili9488
        esp_lcd_touch_gt911
        esp_lcd_touch
        input_enc
)
