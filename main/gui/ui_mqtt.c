#include "ui_mqtt.h"
#include "lvgl.h"
#include "mqtt_manager.h"

/* Widgets refreshed by the LVGL timer callback. */
static lv_obj_t *state_label;
static lv_obj_t *broker_label;
static lv_obj_t *device_label;
static lv_obj_t *topic_label;
static lv_obj_t *count_label;

/**
 * @brief LVGL timer callback: refresh the MQTT status page.
 *        Runs inside lv_timer_handler(), safe to touch LVGL objects.
 */
static void mqtt_display_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (mqtt_manager_is_connected()) {
        lv_label_set_text(state_label, "MQTT: Connected");
        lv_obj_set_style_text_color(state_label, lv_color_hex(0x4CAF50), 0);
    } else {
        lv_label_set_text(state_label, "MQTT: Disconnected");
        lv_obj_set_style_text_color(state_label, lv_color_hex(0xF44336), 0);
    }

    lv_label_set_text_fmt(count_label, "Published: %lu msg(s)",
                          (unsigned long)mqtt_manager_get_publish_count());
}

/**
 * @brief Build the MQTT status page on its own screen.
 */
lv_obj_t *ui_mqtt_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "MQTT Status");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    /* Connection state (colored) */
    state_label = lv_label_create(scr);
    lv_label_set_text(state_label, "MQTT: Disconnected");
    lv_obj_set_style_text_color(state_label, lv_color_hex(0xF44336), 0);
    lv_obj_align(state_label, LV_ALIGN_CENTER, 0, -70);

    /* Broker endpoint */
    broker_label = lv_label_create(scr);
    lv_label_set_text(broker_label, CONFIG_MQTT_BROKER_URI);
    lv_obj_set_style_text_color(broker_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_set_style_text_align(broker_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(broker_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(broker_label, 220);
    lv_obj_align(broker_label, LV_ALIGN_CENTER, 0, -30);

    /* Device id */
    device_label = lv_label_create(scr);
    lv_label_set_text_fmt(device_label, "Device: %s",
                          mqtt_manager_get_device_id());
    lv_obj_set_style_text_color(device_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(device_label, LV_ALIGN_CENTER, 0, 10);

    /* Telemetry topic */
    topic_label = lv_label_create(scr);
    lv_label_set_text_fmt(topic_label, "Topic: %s",
                          mqtt_manager_get_telemetry_topic());
    lv_obj_set_style_text_color(topic_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_set_style_text_align(topic_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(topic_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(topic_label, 220);
    lv_obj_align(topic_label, LV_ALIGN_CENTER, 0, 45);

    /* Publish counter */
    count_label = lv_label_create(scr);
    lv_label_set_text(count_label, "Published: 0 msg(s)");
    lv_obj_set_style_text_color(count_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(count_label, LV_ALIGN_CENTER, 0, 90);

    /* Refresh every 500 ms from the LVGL thread */
    lv_timer_create(mqtt_display_timer_cb, 500, NULL);

    return scr;
}
