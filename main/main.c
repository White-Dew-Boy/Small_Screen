#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_driver.h"
#include "ft6336.h"
#include "sd_card.h"
#include "serial_file.h"
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "app_main";

#define PICTURE_W  240
#define PICTURE_H  320

static void display_picture_from_sd(void)
{
    FILE *f = fopen("/sdcard/picture1.bin", "rb");
    if (!f) {
        ESP_LOGW(TAG, "picture1.bin not found on SD card");
        return;
    }

    int count = PICTURE_W * PICTURE_H;
    uint16_t *buf = malloc(count * sizeof(uint16_t));
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate picture buffer");
        fclose(f);
        return;
    }

    size_t read = fread(buf, sizeof(uint16_t), count, f);
    fclose(f);

    if (read == count) {
        esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, PICTURE_W, PICTURE_H, buf);
        ESP_LOGI(TAG, "Picture displayed from SD card");
    } else {
        ESP_LOGE(TAG, "Failed to read picture (got %d/%d pixels)", read, count);
    }

    free(buf);
}

void app_main(void)
{
    esp_err_t sd_ret = sd_card_init();
    if (sd_ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card not available (%s), continuing without SD",
                 esp_err_to_name(sd_ret));
    }

    lcd_init();

    if (sd_ret == ESP_OK) {
        display_picture_from_sd();
        serial_file_init();
    }

    ft6336_init();
    ESP_LOGI(TAG, "Ready");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
