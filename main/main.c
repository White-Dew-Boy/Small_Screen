#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"
#include "lcd_driver.h"
#include "sd_card.h"
#include "power.h"
#include "drivers/i2c_bus.h"
#include "drivers/shtc3.h"
#include "drivers/ft6336.h"
#include "drivers/key.h"
#include "drivers/wifi_manager.h"
#include "drivers/mqtt_manager.h"
#if CONFIG_ENABLE_MPU6050
#include "drivers/mpu6050.h"
#endif
#include "ui_shtc3.h"
#include "ui_wifi.h"
#include "ui_saved_wifi.h"
#include "ui_nearby_wifi.h"
#include "ui_mqtt.h"

static const char *TAG = "app_main";

/* UI screens, created once and switched with KEY2 / KEY3 */
static lv_obj_t *scr_shtc3;
static lv_obj_t *scr_wifi;
static lv_obj_t *scr_saved_wifi;
static lv_obj_t *scr_nearby_wifi;
static lv_obj_t *scr_mqtt;

/* Page-switch request produced by the key task and consumed by the LVGL task
 * (LVGL is not thread-safe, so screens are switched from the LVGL thread). */
typedef enum {
    SWITCH_NONE = 0,
    SWITCH_SHTC3,
    SWITCH_WIFI,
    SWITCH_SAVED_WIFI,
    SWITCH_NEARBY_WIFI,
    SWITCH_MQTT,
} switch_req_t;
static volatile switch_req_t s_switch_req = SWITCH_NONE;

/* Current page index of the KEY2 cycle (0 = SHTC3, 1 = WIFI, 2 = MQTT).
 * Shared with the saved/nearby WiFi page callbacks so the cycle stays
 * in sync (they return to the WIFI status page). */
static int s_cur_page = 0;

/* Called from the WiFi status page "Saved WiFi" button (LVGL thread) */
static void saved_wifi_open(void)
{
    ui_saved_wifi_refresh();
    s_switch_req = SWITCH_SAVED_WIFI;
}

/* Called from the WiFi status page "Nearby WiFi" button (LVGL thread) */
static void nearby_wifi_open(void)
{
    ui_nearby_wifi_start_scan();
    s_switch_req = SWITCH_NEARBY_WIFI;
}

/* Called from the saved/nearby WiFi pages back/confirm (LVGL thread) */
static void wifi_sub_close(void)
{
    s_cur_page = 1; /* back to WIFI status page */
    s_switch_req = SWITCH_WIFI;
}

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

        /* Handle pending page switch from the LVGL thread */
        switch_req_t req = s_switch_req;
        if (req != SWITCH_NONE) {
            s_switch_req = SWITCH_NONE;
            if (req == SWITCH_SHTC3) {
                lv_scr_load(scr_shtc3);
            } else if (req == SWITCH_WIFI) {
                lv_scr_load(scr_wifi);
            } else if (req == SWITCH_SAVED_WIFI) {
                lv_scr_load(scr_saved_wifi);
            } else if (req == SWITCH_NEARBY_WIFI) {
                lv_scr_load(scr_nearby_wifi);
            } else if (req == SWITCH_MQTT) {
                lv_scr_load(scr_mqtt);
            }
        }

        lv_timer_handler();
        vTaskDelay(delay_ticks);
    }
}

/* Scan the two buttons; page switches are applied by the LVGL task.
 * KEY2 cycles through pages: sensor -> wifi -> mqtt -> sensor ...
 * KEY3 jumps back to the sensor dashboard. */
static void key_task(void *arg)
{
    (void)arg;

    while (1) {
        key_scan();
        if (key_pressed_edge(KEY_ID_2)) {
            s_cur_page = (s_cur_page + 1) % 3;
            if (s_cur_page == 0) {
                s_switch_req = SWITCH_SHTC3;
            } else if (s_cur_page == 1) {
                s_switch_req = SWITCH_WIFI;
            } else {
                s_switch_req = SWITCH_MQTT;
            }
        } else if (key_pressed_edge(KEY_ID_3)) {
            s_cur_page = 0;
            s_switch_req = SWITCH_SHTC3;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
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
            ui_shtc3_set_invalid();
        } else {
            shtc3_caculate_data(&raw_humi, &raw_temp, &humi, &temp);
            ESP_LOGI(TAG, "SHTC3 data - Temperature: %.2f C, Humidity: %.2f %%RH", temp, humi);

            /* Publish to the LVGL thread (LVGL is not thread-safe) */
            ui_shtc3_set_data(temp, humi);

            /* Publish to the MQTT broker (rate-limited internally) */
            mqtt_manager_publish_telemetry(temp, humi);
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(power_init());
    ESP_ERROR_CHECK(power_on(POWER_ID_TEMP));
    ESP_ERROR_CHECK(power_on(POWER_ID_LCD));

    // Initialize WiFi (STA mode, connects in background, auto-reconnect)
    // Non-fatal: the device keeps working without network access.
    esp_err_t wifi_ret = wifi_manager_init();
    if (wifi_ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi manager init failed: %s", esp_err_to_name(wifi_ret));
    } else {
        ESP_LOGI(TAG, "WiFi manager started");
    }

    // MQTT client (starts automatically once WiFi is connected)
    // Non-fatal: telemetry just stays local if the broker is unreachable.
    esp_err_t mqtt_ret = mqtt_manager_init();
    if (mqtt_ret != ESP_OK) {
        ESP_LOGE(TAG, "MQTT manager init failed: %s", esp_err_to_name(mqtt_ret));
    } else {
        ESP_LOGI(TAG, "MQTT manager started");
    }

    // Initialize I2C bus
    ESP_ERROR_CHECK(i2c_bus_init());

    // Initialize FT6336 capacitive touch controller (non-fatal)
    esp_err_t ft6336_ret = ft6336_init();
    if (ft6336_ret != ESP_OK) {
        ESP_LOGW(TAG, "FT6336 touch not available (%s), touch disabled",
                 esp_err_to_name(ft6336_ret));
    } else {
        ESP_LOGI(TAG, "FT6336 touch initialized");
    }

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

    // Buttons (KEY2: cycle pages, KEY3: sensor page)
    ESP_ERROR_CHECK(key_init());

    // LVGL
    lv_init();
    ESP_ERROR_CHECK(lv_port_tick_init());
    ESP_ERROR_CHECK(lv_port_disp_init());
    ESP_ERROR_CHECK(lv_port_indev_init());

    // Build all pages, start on the sensor dashboard
    scr_shtc3 = ui_shtc3_create();
    scr_wifi = ui_wifi_create();
    scr_saved_wifi = ui_saved_wifi_create();
    scr_nearby_wifi = ui_nearby_wifi_create();
    scr_mqtt = ui_mqtt_create();
    lv_scr_load(scr_shtc3);

    // WiFi status page buttons -> saved / nearby pages
    ui_wifi_set_saved_cb(saved_wifi_open);
    ui_wifi_set_nearby_cb(nearby_wifi_open);
    // saved / nearby pages back / confirm -> WiFi status page
    ui_saved_wifi_set_back_cb(wifi_sub_close);
    ui_nearby_wifi_set_back_cb(wifi_sub_close);

    ESP_LOGI(TAG, "Free heap: internal=%lu KB, PSRAM=%lu KB",
             (unsigned long)esp_get_free_internal_heap_size() / 1024,
             (unsigned long)esp_get_free_heap_size() / 1024);

    xTaskCreatePinnedToCore(lvgl_task, "lvgl_task", 7168, NULL, 5, NULL, 0);
    xTaskCreate(key_task, "key_task", 2048, NULL, 6, NULL);
}
