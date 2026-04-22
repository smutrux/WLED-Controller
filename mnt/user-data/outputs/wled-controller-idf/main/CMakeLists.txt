idf_component_register(
    SRCS
        "main.c"
        "display.c"
        "touch_gt911.c"
        "ui.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_lcd
        esp_timer
        driver
        esp_psram
        freertos
        log
        lvgl__lvgl          # component manager name for lvgl/lvgl
)
