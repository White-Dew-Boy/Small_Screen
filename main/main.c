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
#include "drivers/rgb_led.h"
#include "drivers/time_manager.h"
#include "ui_shtc3.h"
#include "ui_wifi.h"
#include "ui_saved_wifi.h"
#include "ui_nearby_wifi.h"
#include "ui_mqtt.h"
#include "ui_mqtt_interval.h"
#include "ui_mqtt_history.h"
#include "ui_mqtt_config.h"
#include "ui_mqtt_pub.h"
#include "ui_mqtt_sub.h"
#include "ui_led.h"
#include "ui_led_preset.h"
#include "ui_led_custom.h"
#include "ui_home.h"
#include "ui_sd.h"
#include "ui_files.h"
#include "ui_sysinfo.h"

static const char *TAG = "app_main";

/* UI screens, created once and switched with KEY2 */
static lv_obj_t *scr_home;
static lv_obj_t *scr_shtc3;
static lv_obj_t *scr_wifi;
static lv_obj_t *scr_saved_wifi;
static lv_obj_t *scr_nearby_wifi;
static lv_obj_t *scr_mqtt;
static lv_obj_t *scr_mqtt_interval;
static lv_obj_t *scr_mqtt_history;
static lv_obj_t *scr_mqtt_config;
static lv_obj_t *scr_mqtt_pub;
static lv_obj_t *scr_mqtt_sub;
static lv_obj_t *scr_led;
static lv_obj_t *scr_led_preset;
static lv_obj_t *scr_led_custom;
static lv_obj_t *scr_sd;
static lv_obj_t *scr_sd_files;
static lv_obj_t *scr_sysinfo;
static lv_obj_t *scr_sysinfo_cpu;
static lv_obj_t *scr_sysinfo_stack;
static lv_obj_t *scr_sysinfo_about;

/* Page-switch request produced by the key task and consumed by the LVGL task
 * (LVGL is not thread-safe, so screens are switched from the LVGL thread). */
typedef enum {
    SWITCH_NONE = 0,
    SWITCH_HOME,
    SWITCH_SHTC3,
    SWITCH_WIFI,
    SWITCH_SAVED_WIFI,
    SWITCH_NEARBY_WIFI,
    SWITCH_MQTT,
    SWITCH_MQTT_INTERVAL,
    SWITCH_MQTT_HISTORY,
    SWITCH_MQTT_CONFIG,
    SWITCH_MQTT_PUB,
    SWITCH_MQTT_SUB,
    SWITCH_LED,
    SWITCH_LED_PRESET,
    SWITCH_LED_CUSTOM,
    SWITCH_SD,
    SWITCH_SD_FILES,
    SWITCH_SYSINFO,
    SWITCH_SYSINFO_CPU,
    SWITCH_SYSINFO_STACK,
    SWITCH_SYSINFO_ABOUT,
} switch_req_t;
static volatile switch_req_t s_switch_req = SWITCH_NONE;

/* Current page index of the KEY2 cycle (0 = SHTC3, 1 = WIFI, 2 = MQTT,
 * 3 = LED, 4 = SD, 5 = SYSINFO). Shared with the sub-page callbacks so the
 * cycle stays in sync (they return to their parent page). */
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

/* Called from the MQTT status page sub-buttons (LVGL thread) */
static void mqtt_interval_open(void)
{
    s_switch_req = SWITCH_MQTT_INTERVAL;
}

static void mqtt_history_open(void)
{
    s_switch_req = SWITCH_MQTT_HISTORY;
}

static void mqtt_config_open(void)
{
    s_switch_req = SWITCH_MQTT_CONFIG;
}

static void mqtt_pub_open(void)
{
    s_switch_req = SWITCH_MQTT_PUB;
}

static void mqtt_sub_open(void)
{
    s_switch_req = SWITCH_MQTT_SUB;
}

/* Called from the MQTT sub-pages back/save (LVGL thread) */
static void mqtt_sub_close(void)
{
    s_cur_page = 2; /* back to MQTT status page */
    s_switch_req = SWITCH_MQTT;
}

/* Called from the LED page "Preset Colors" / "Custom RGB" buttons */
static void led_preset_open(void)
{
    s_switch_req = SWITCH_LED_PRESET;
}

static void led_custom_open(void)
{
    /* Sync the R/G/B sliders with the LED page's selected target before
     * switching (the page is created once, so this must be refreshed on
     * every entry). */
    ui_led_custom_refresh();
    s_switch_req = SWITCH_LED_CUSTOM;
}

/* Called from the LED sub-pages back / color picked (LVGL thread) */
static void led_sub_close(void)
{
    s_cur_page = 3; /* back to LED control page */
    s_switch_req = SWITCH_LED;
}

/* Called from the System Info page buttons (LVGL thread) */
static void sysinfo_cpu_open(void)
{
    s_switch_req = SWITCH_SYSINFO_CPU;
}

static void sysinfo_stack_open(void)
{
    s_switch_req = SWITCH_SYSINFO_STACK;
}

static void sysinfo_about_open(void)
{
    s_switch_req = SWITCH_SYSINFO_ABOUT;
}

/* Called from the System Info sub-pages back button (LVGL thread) */
static void sysinfo_sub_close(void)
{
    s_switch_req = SWITCH_SYSINFO;
}

/* Called from the SD status page "Browse Files" button (LVGL thread) */
static void sd_files_open(void)
{
    ui_files_refresh(); /* re-read the current directory before showing */
    s_switch_req = SWITCH_SD_FILES;
}

/* Called from the file browser Back button (LVGL thread) */
static void sd_files_close(void)
{
    s_cur_page = 4; /* back to SD status page */
    s_switch_req = SWITCH_SD;
}

/* Called from the home menu: open the selected page (LVGL thread) */
static void home_open_page(int page)
{
    s_cur_page = page;
    switch (page) {
    case HOME_PAGE_SENSOR:
        s_switch_req = SWITCH_SHTC3;
        break;
    case HOME_PAGE_WIFI:
        s_switch_req = SWITCH_WIFI;
        break;
    case HOME_PAGE_MQTT:
        s_switch_req = SWITCH_MQTT;
        break;
    case HOME_PAGE_SD:
        s_switch_req = SWITCH_SD;
        break;
    case HOME_PAGE_SYSINFO:
        s_switch_req = SWITCH_SYSINFO;
        break;
    default:
        s_switch_req = SWITCH_LED;
        break;
    }
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
            if (req == SWITCH_HOME) {
                ui_home_reset_hint(); /* clear stale press-count text */
                lv_scr_load(scr_home);
            } else if (req == SWITCH_SHTC3) {
                lv_scr_load(scr_shtc3);
            } else if (req == SWITCH_WIFI) {
                lv_scr_load(scr_wifi);
            } else if (req == SWITCH_SAVED_WIFI) {
                lv_scr_load(scr_saved_wifi);
            } else if (req == SWITCH_NEARBY_WIFI) {
                lv_scr_load(scr_nearby_wifi);
            } else if (req == SWITCH_MQTT) {
                lv_scr_load(scr_mqtt);
            } else if (req == SWITCH_MQTT_INTERVAL) {
                lv_scr_load(scr_mqtt_interval);
            } else if (req == SWITCH_MQTT_HISTORY) {
                lv_scr_load(scr_mqtt_history);
            } else if (req == SWITCH_MQTT_CONFIG) {
                lv_scr_load(scr_mqtt_config);
            } else if (req == SWITCH_MQTT_PUB) {
                lv_scr_load(scr_mqtt_pub);
            } else if (req == SWITCH_MQTT_SUB) {
                lv_scr_load(scr_mqtt_sub);
            } else if (req == SWITCH_LED) {
                lv_scr_load(scr_led);
            } else if (req == SWITCH_LED_PRESET) {
                lv_scr_load(scr_led_preset);
            } else if (req == SWITCH_LED_CUSTOM) {
                lv_scr_load(scr_led_custom);
            } else if (req == SWITCH_SD) {
                lv_scr_load(scr_sd);
            } else if (req == SWITCH_SD_FILES) {
                lv_scr_load(scr_sd_files);
            } else if (req == SWITCH_SYSINFO) {
                lv_scr_load(scr_sysinfo);
            } else if (req == SWITCH_SYSINFO_CPU) {
                lv_scr_load(scr_sysinfo_cpu);
            } else if (req == SWITCH_SYSINFO_STACK) {
                lv_scr_load(scr_sysinfo_stack);
            } else if (req == SWITCH_SYSINFO_ABOUT) {
                lv_scr_load(scr_sysinfo_about);
            }
        }

        lv_timer_handler();
        vTaskDelay(delay_ticks);
    }
}

/* Scan the two buttons; page switches are applied by the LVGL task.
 * KEY2 cycles through pages: sensor -> wifi -> mqtt -> led -> sd ->
 * sysinfo -> sensor ...  KEY3 jumps back to the home menu. */
static void key_task(void *arg)
{
    (void)arg;

    while (1) {
        key_scan();
        if (key_pressed_edge(KEY_ID_2)) {
            s_cur_page = (s_cur_page + 1) % 6;
            switch (s_cur_page) {
            case 0:
                s_switch_req = SWITCH_SHTC3;
                break;
            case 1:
                s_switch_req = SWITCH_WIFI;
                break;
            case 2:
                s_switch_req = SWITCH_MQTT;
                break;
            case 3:
                s_switch_req = SWITCH_LED;
                break;
            case 4:
                s_switch_req = SWITCH_SD;
                break;
            default:
                s_switch_req = SWITCH_SYSINFO;
                break;
            }
        } else if (key_pressed_edge(KEY_ID_3)) {
            s_switch_req = SWITCH_HOME;
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
    ESP_ERROR_CHECK(power_on(POWER_ID_IMU));

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

    // SNTP time sync (starts on first WiFi connection, TZ from Kconfig)
    // Non-fatal: the clock on the Home page stays "Syncing..." offline.
    esp_err_t time_ret = time_manager_init();
    if (time_ret != ESP_OK) {
        ESP_LOGE(TAG, "Time manager init failed: %s", esp_err_to_name(time_ret));
    } else {
        ESP_LOGI(TAG, "Time manager started");
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

    // Buttons (KEY2: cycle pages, KEY3: home menu)
    ESP_ERROR_CHECK(key_init());

    // RGB LED strip (3x WS2812). Non-fatal: the UI still works without it.
    esp_err_t led_ret = rgb_led_init();
    if (led_ret != ESP_OK) {
        ESP_LOGW(TAG, "RGB LED init failed (%s), LED page disabled",
                 esp_err_to_name(led_ret));
    } else {
        ESP_LOGI(TAG, "RGB LED initialized");
    }

    // LVGL
    lv_init();
    ESP_ERROR_CHECK(lv_port_tick_init());
    ESP_ERROR_CHECK(lv_port_disp_init());
    ESP_ERROR_CHECK(lv_port_indev_init());

    // Build all pages, start on the home menu
    scr_home = ui_home_create();
    scr_shtc3 = ui_shtc3_create();
    scr_wifi = ui_wifi_create();
    scr_saved_wifi = ui_saved_wifi_create();
    scr_nearby_wifi = ui_nearby_wifi_create();
    scr_mqtt = ui_mqtt_create();
    scr_mqtt_interval = ui_mqtt_interval_create();
    scr_mqtt_history = ui_mqtt_history_create();
    scr_mqtt_config = ui_mqtt_config_create();
    scr_mqtt_pub = ui_mqtt_pub_create();
    scr_mqtt_sub = ui_mqtt_sub_create();
    scr_led = ui_led_create();
    scr_led_preset = ui_led_preset_create();
    scr_led_custom = ui_led_custom_create();
    scr_sd = ui_sd_create();
    scr_sd_files = ui_files_create();
    scr_sysinfo = ui_sysinfo_create();
    scr_sysinfo_cpu = ui_sysinfo_cpu_create();
    scr_sysinfo_stack = ui_sysinfo_stack_create();
    scr_sysinfo_about = ui_sysinfo_about_create();
    lv_scr_load(scr_home);

    // Home menu entries -> pages; deep sleep button (implemented in power.c)
    ui_home_set_page_cb(home_open_page);
    ui_home_set_deepsleep_cb(power_deep_sleep);

    // WiFi status page buttons -> saved / nearby pages
    ui_wifi_set_saved_cb(saved_wifi_open);
    ui_wifi_set_nearby_cb(nearby_wifi_open);
    // saved / nearby pages back / confirm -> WiFi status page
    ui_saved_wifi_set_back_cb(wifi_sub_close);
    ui_nearby_wifi_set_back_cb(wifi_sub_close);

    // MQTT status page buttons -> interval / history / config / pub / sub pages
    ui_mqtt_set_interval_cb(mqtt_interval_open);
    ui_mqtt_set_history_cb(mqtt_history_open);
    ui_mqtt_set_config_cb(mqtt_config_open);
    ui_mqtt_set_pub_cb(mqtt_pub_open);
    ui_mqtt_set_sub_cb(mqtt_sub_open);
    // MQTT sub-pages back / save -> MQTT status page
    ui_mqtt_interval_set_back_cb(mqtt_sub_close);
    ui_mqtt_history_set_back_cb(mqtt_sub_close);
    ui_mqtt_config_set_back_cb(mqtt_sub_close);
    ui_mqtt_pub_set_back_cb(mqtt_sub_close);
    ui_mqtt_sub_set_back_cb(mqtt_sub_close);

    // LED page buttons -> preset / custom color pages
    ui_led_set_preset_cb(led_preset_open);
    ui_led_set_custom_cb(led_custom_open);
    // LED sub-pages back / color picked -> LED control page
    ui_led_preset_set_back_cb(led_sub_close);
    ui_led_custom_set_back_cb(led_sub_close);

    // System Info page buttons -> CPU load / stack HWM / about pages
    ui_sysinfo_set_cpu_cb(sysinfo_cpu_open);
    ui_sysinfo_set_stack_cb(sysinfo_stack_open);
    ui_sysinfo_set_about_cb(sysinfo_about_open);
    // System Info sub-pages back -> System Info page
    ui_sysinfo_cpu_set_back_cb(sysinfo_sub_close);
    ui_sysinfo_stack_set_back_cb(sysinfo_sub_close);
    ui_sysinfo_about_set_back_cb(sysinfo_sub_close);

    // SD status page "Browse Files" -> file browser; back -> SD status page
    ui_sd_set_browse_cb(sd_files_open);
    ui_files_set_back_cb(sd_files_close);

    ESP_LOGI(TAG, "Free heap: internal=%lu KB, PSRAM=%lu KB",
             (unsigned long)esp_get_free_internal_heap_size() / 1024,
             (unsigned long)esp_get_free_heap_size() / 1024);

    /* 16 KB stack: the LVGL task also executes the upload server's SD
     * file I/O (directory listing via opendir/readdir/stat/qsort inside
     * the 10 ms upload timer) on top of LVGL's own render/layout stack
     * usage. 8 KB overflowed when the browser hit the upload page. */
    xTaskCreatePinnedToCore(lvgl_task, "lvgl_task", 16384, NULL, 5, NULL, 0);
    xTaskCreate(key_task, "key_task", 2048, NULL, 6, NULL);
}
