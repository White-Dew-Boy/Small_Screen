idf_component_register(
    SRCS 
        "main.c" 
        "ft6336.c" 
        "lcd_driver.c" 
        "sd_card.c"
        "key.c"
    
    INCLUDE_DIRS 
        "."
                    
    REQUIRES 
        esp_lcd 
        esp_driver_ledc 
        driver 
        fatfs 
        sdmmc
)
