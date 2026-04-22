idf_component_register(
    SRCS
        "main.c"
        "display/display.c"
        "ui.c"        
        "touch/touch_ft6336.c"
        "board/tca9554.c"
        "encoder/encoder.c"
        "encoder/encoder_nav.c"
    INCLUDE_DIRS "."
    REQUIRES
        lvgl
        esp_timer
        driver
        esp_lcd
        esp_psram
        esp_driver_pcnt
)