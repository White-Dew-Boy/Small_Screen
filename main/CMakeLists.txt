idf_component_register(SRCS "ft6336.c" "main.c" "lcd_driver.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd esp_driver_ledc lvgl driver)
