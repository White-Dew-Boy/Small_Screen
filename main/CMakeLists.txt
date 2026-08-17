idf_component_register(
    SRCS 
        "main.c" 
        "gui/lv_port_disp.c"
        "gui/lv_port_indev.c"
        "drivers/ft6336.c"
        "drivers/i2c_bus.c"
        "drivers/key.c"
        "drivers/lcd_driver.c"
        "drivers/power.c"
        "drivers/rgb_led.c"
        "drivers/sd_card.c"
    
    INCLUDE_DIRS 
        "."
        "gui"
        "drivers"
                    
    REQUIRES 
        esp_lcd 
        esp_driver_ledc 
        driver 
        fatfs 
        sdmmc
        led_strip
        esp_timer
)
