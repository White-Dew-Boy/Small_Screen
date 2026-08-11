#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_driver.h"
#include "ft6336.h"
#include "sd_card.h"
#include "picture1.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_main";

static void display_picture(void)
{
    int count = picture1_W * picture1_H;
    uint16_t *buf = malloc(count * sizeof(uint16_t));
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate picture buffer");
        return;
    }
    memcpy(buf, picture1_data, count * sizeof(uint16_t));
    gb_swap_buf(buf, count);
    esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, picture1_W, picture1_H, buf);
    free(buf);
    ESP_LOGI(TAG, "Picture displayed");
}

void app_main(void)
{
    esp_err_t sd_ret = sd_card_init();
    if (sd_ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card not available (%s), continuing without SD",
                 esp_err_to_name(sd_ret));
    }

    lcd_init();
    display_picture();
    ft6336_init();
    ESP_LOGI(TAG, "Ready");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
