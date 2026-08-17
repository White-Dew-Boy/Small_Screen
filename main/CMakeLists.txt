idf_component_register(
    SRCS 
        "main.c" 
        "ft6336.c" 
        "i2c_bus.c"
        "mpu6050.c"
        "lcd_driver.c" 
        "sd_card.c"
        "key.c"
        "rgb_led.c"
        "power.c"
    
    INCLUDE_DIRS 
        "."
                    
    REQUIRES 
        esp_lcd 
        esp_driver_ledc 
        driver 
        fatfs 
        sdmmc
        led_strip
)
