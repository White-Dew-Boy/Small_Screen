idf_component_register(SRCS "main.c" "ft6336.c" "lcd_driver.c" "sd_card.c"
                           "ili9341_lvgl.c" "ft6336_lvgl.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd esp_driver_ledc driver fatfs sdmmc lvgl)
