#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "lcd_driver.h"
#include "ft6336.h"

#define LCD_W  240
#define LCD_H  320

static const char *TAG = "app_main";
static uint16_t *fb;
static TaskHandle_t touch_task;

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    int x1 = x < 0 ? 0 : x;
    int y1 = y < 0 ? 0 : y;
    int x2 = x + w > LCD_W ? LCD_W : x + w;
    int y2 = y + h > LCD_H ? LCD_H : y + h;
    for (int row = y1; row < y2; row++) {
        for (int col = x1; col < x2; col++) {
            fb[row * LCD_W + col] = color;
        }
    }
}

static uint32_t touch_event_count;

static void IRAM_ATTR touch_isr(void *arg)
{
    touch_event_count++;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(touch_task, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void app_main(void)
{
    fb = calloc(1, LCD_W * LCD_H * sizeof(uint16_t));
    assert(fb);

    lcd_driver_init();
    lcd_draw_bitmap(fb, 0, 0, LCD_W, LCD_H);

    ft6336_init();

    gpio_config_t int_cfg = {
        .pin_bit_mask = BIT64(FT6336_INT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&int_cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(FT6336_INT, touch_isr, NULL);

    ft6336_enter_monitor();

    touch_task = xTaskGetCurrentTaskHandle();
    ESP_LOGI(TAG, "Waiting for touch (monitor mode)...");

    int16_t last_tx = -1, last_ty = -1;
    bool frozen = false;

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        vTaskDelay(pdMS_TO_TICKS(15));

        ft6336_touch_data_t touch;
        int retry = 3;
        do {
            ft6336_read(&touch);
            if (touch.num_touches > 0) break;
            vTaskDelay(pdMS_TO_TICKS(5));
        } while (--retry > 0);

        if (frozen) {
            ESP_LOGI(TAG, "INT #%lu  frozen, skip", (unsigned long)touch_event_count);
            continue;
        }

        if (touch.num_touches > 0) {
            int16_t tx = LCD_W - 1 - touch.points[0].x;
            int16_t ty = LCD_H - 1 - touch.points[0].y;

            ESP_LOGI(TAG, "INT #%lu  raw=(%d,%d)  tx=%d ty=%d",
                     (unsigned long)touch_event_count,
                     touch.points[0].x, touch.points[0].y, tx, ty);

            if (tx != last_tx || ty != last_ty) {
                if (last_tx >= 0) {
                    fill_rect(last_tx - 20, last_ty - 2, 40, 4, 0x0000);
                    fill_rect(last_tx - 2, last_ty - 20, 4, 40, 0x0000);
                    lcd_draw_bitmap(fb, last_tx - 20, last_ty - 20, 40, 40);
                }
                fill_rect(tx - 20, ty - 2, 40, 4, 0xF800);
                fill_rect(tx - 2, ty - 20, 4, 40, 0xF800);
                lcd_draw_bitmap(fb, tx - 20, ty - 20, 40, 40);
                last_tx = tx;
                last_ty = ty;
                frozen = true;
                ESP_LOGI(TAG, "Screen frozen");
            }

            vTaskDelay(pdMS_TO_TICKS(2));
        } else {
            ESP_LOGI(TAG, "INT #%lu  released", (unsigned long)touch_event_count);
            if (last_tx >= 0) {
                fill_rect(last_tx - 20, last_ty - 2, 40, 4, 0x0000);
                fill_rect(last_tx - 2, last_ty - 20, 4, 40, 0x0000);
                lcd_draw_bitmap(fb, last_tx - 20, last_ty - 20, 40, 40);
                last_tx = -1;
                last_ty = -1;
            }
            ft6336_enter_monitor();
        }
    }
}
