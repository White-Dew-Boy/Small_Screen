#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lv_port_disp.h"
#include "lcd_driver.h"
#include "sd_card.h"
#include "power.h"
#include "drivers/i2c_bus.h"
#include "drivers/mpu6050.h"
#include "drivers/shtc3.h"

static const char *TAG = "app_main";

/* SHTC3 sensor data shared with LVGL (written by shtc3_task, read by LVGL timer) */
static volatile float s_temp_c = 0.0f;
static volatile float s_humi_rh = 0.0f;
static volatile bool s_shtc3_valid = false;

static lv_obj_t *temp_label;
static lv_obj_t *humi_label;

static void lvgl_task(void *arg)
{
    (void)arg;
    bool hwm_logged = false;

    /* With CONFIG_FREERTOS_HZ=100, pdMS_TO_TICKS(5) truncates to 0 ticks and
     * vTaskDelay(0) never blocks: this task would then keep CPU0 busy forever,
     * starve the IDLE0 task, and trip the task watchdog every 5 s.
     * Guarantee at least one tick of real blocking. */
    const TickType_t delay_ticks = pdMS_TO_TICKS(5) > 0 ? pdMS_TO_TICKS(5) : 1;

    while (1) {
        if (!hwm_logged) {
            hwm_logged = true;
            ESP_LOGI(TAG, "LVGL task stack high water: %lu",
                     (unsigned long)uxTaskGetStackHighWaterMark(NULL));
        }
        lv_timer_handler();
        vTaskDelay(delay_ticks);
    }
}

/**
 * @brief LVGL timer callback: refresh temperature/humidity labels on screen.
 *        Runs inside lv_timer_handler(), so it is safe to touch LVGL objects here.
 */
static void shtc3_display_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_shtc3_valid) {
        return;
    }

    /* LV_SPRINTF_USE_FLOAT is disabled, format float manually as "int.dec" */
    int temp_int = (int)s_temp_c;
    int temp_dec = (int)((s_temp_c - temp_int) * 10);
    if (temp_dec < 0) temp_dec = -temp_dec;

    int humi_int = (int)s_humi_rh;
    int humi_dec = (int)((s_humi_rh - humi_int) * 10);
    if (humi_dec < 0) humi_dec = -humi_dec;

    lv_label_set_text_fmt(temp_label, "%d.%d C", temp_int, temp_dec);
    lv_label_set_text_fmt(humi_label, "%d.%d %%RH", humi_int, humi_dec);
}

/**
 * @brief Build the SHTC3 sensor dashboard on the LCD.
 */
static void ui_shtc3_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SHTC3 Sensor");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    temp_label = lv_label_create(scr);
    lv_label_set_text(temp_label, "--.- C");
    lv_obj_align(temp_label, LV_ALIGN_CENTER, 0, -30);

    humi_label = lv_label_create(scr);
    lv_label_set_text(humi_label, "--.- %RH");
    lv_obj_align(humi_label, LV_ALIGN_CENTER, 0, 20);

    /* Refresh labels every 500ms from the LVGL thread */
    lv_timer_create(shtc3_display_timer_cb, 500, NULL);
}

static void shtc3_task(void *arg)
{
    (void)arg;

    uint16_t raw_humi = 0, raw_temp = 0;
    float humi = 0.0f, temp = 0.0f;

    while (1) {
        esp_err_t read_ret = shtc3_getdata(&raw_humi, &raw_temp);
        if (read_ret != ESP_OK) {
            ESP_LOGE(TAG, "SHTC3 read failed: %s", esp_err_to_name(read_ret));
            s_shtc3_valid = false;
        } else {
            shtc3_caculate_data(&raw_humi, &raw_temp, &humi, &temp);
            ESP_LOGI(TAG, "SHTC3 data - Temperature: %.2f C, Humidity: %.2f %%RH", temp, humi);

            /* Publish to the LVGL thread via globals (LVGL is not thread-safe) */
            s_temp_c = temp;
            s_humi_rh = humi;
            s_shtc3_valid = true;
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(power_init());
    ESP_ERROR_CHECK(power_on(POWER_ID_TEMP));
    ESP_ERROR_CHECK(power_on(POWER_ID_LCD));

    // Initialize I2C bus
    ESP_ERROR_CHECK(i2c_bus_init());

    // Initialize SHTC3 temperature & humidity sensor
    esp_err_t shtc3_ret = shtc3_init();
    if (shtc3_ret != ESP_OK) {
        ESP_LOGE(TAG, "SHTC3 initialization failed: %s", esp_err_to_name(shtc3_ret));
    } else {
        ESP_LOGI(TAG, "SHTC3 initialized successfully");
        xTaskCreate(shtc3_task, "shtc3_task", 4096, NULL, 5, NULL);
    }

    // SD card shares the SPI bus with the LCD — must be initialized first.
    // Non-fatal: if no SD card is present, the LCD still works.
    esp_err_t sd_ret = sd_card_init();
    if (sd_ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card not available (%s), continuing without SD",
                 esp_err_to_name(sd_ret));
    }

    // LCD (uses the SPI bus created by sd_card_init())
    ESP_ERROR_CHECK(lcd_init());

    // LVGL
    lv_init();
    ESP_ERROR_CHECK(lv_port_tick_init());
    ESP_ERROR_CHECK(lv_port_disp_init());

    ui_shtc3_create();

    ESP_LOGI(TAG, "Free heap: internal=%lu KB, PSRAM=%lu KB",
             (unsigned long)esp_get_free_internal_heap_size() / 1024,
             (unsigned long)esp_get_free_heap_size() / 1024);

    xTaskCreatePinnedToCore(lvgl_task, "lvgl_task", 7168, NULL, 5, NULL, 0);
}
