#include "ui_mqtt_interval.h"
#include "lvgl.h"
#include "mqtt_manager.h"

#include <stdio.h>

/* Selectable interval steps in seconds */
static const int s_steps[] = {1, 2, 5, 10, 15, 30, 60, 300, 600};
#define STEP_COUNT (sizeof(s_steps) / sizeof(s_steps[0]))

/* Widgets */
static lv_obj_t *s_value_label;
static lv_obj_t *s_msg_label;
static lv_obj_t *s_back_btn;

/* Callback to leave the page (set by main.c, invoked from the LVGL thread) */
static void (*s_back_cb)(void) = NULL;

static int s_cur_idx = 0;

void ui_mqtt_interval_set_back_cb(void (*cb)(void))
{
    s_back_cb = cb;
}

static void update_value_label(void)
{
    lv_label_set_text_fmt(s_value_label, "%d s", s_steps[s_cur_idx]);
}

/* Find the step index closest to the current interval. */
static int find_step_index(int seconds)
{
    int best = 0;
    int best_diff = 0x7FFFFFFF;
    for (size_t i = 0; i < STEP_COUNT; i++) {
        int diff = seconds - s_steps[i];
        if (diff < 0) {
            diff = -diff;
        }
        if (diff < best_diff) {
            best_diff = diff;
            best = (int)i;
        }
    }
    return best;
}

static void minus_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_cur_idx > 0) {
        s_cur_idx--;
    }
    update_value_label();
    lv_label_set_text(s_msg_label, "");
}

static void plus_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_cur_idx < (int)STEP_COUNT - 1) {
        s_cur_idx++;
    }
    update_value_label();
    lv_label_set_text(s_msg_label, "");
}

static void save_click_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t ret = mqtt_manager_set_interval(s_steps[s_cur_idx]);
    if (ret != ESP_OK) {
        lv_label_set_text_fmt(s_msg_label, "Error: %s", esp_err_to_name(ret));
        return;
    }
    lv_label_set_text(s_msg_label, "Saved");
    lv_obj_set_style_text_color(s_msg_label, lv_color_hex(0x4CAF50), 0);
    if (s_back_cb != NULL) {
        s_back_cb();
    }
}

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_back_cb != NULL) {
        s_back_cb();
    }
}

lv_obj_t *ui_mqtt_interval_create(void)
{
    /* Landscape 320x240, like the MQTT status page it belongs to. */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 240);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Telemetry Interval");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    /* Current value */
    s_value_label = lv_label_create(scr);
    s_cur_idx = find_step_index(mqtt_manager_get_interval());
    update_value_label();
    lv_obj_set_style_text_color(s_value_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_value_label, LV_ALIGN_CENTER, 0, -30);

    /* Message line */
    s_msg_label = lv_label_create(scr);
    lv_label_set_text(s_msg_label, "");
    lv_obj_set_style_text_color(s_msg_label, lv_color_hex(0xF44336), 0);
    lv_obj_align(s_msg_label, LV_ALIGN_CENTER, 0, 25);

    /* Minus / Plus buttons */
    lv_obj_t *minus_btn = lv_btn_create(scr);
    lv_obj_set_size(minus_btn, 90, 44);
    lv_obj_align(minus_btn, LV_ALIGN_CENTER, -80, -30);
    lv_obj_set_style_bg_color(minus_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(minus_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *minus_label = lv_label_create(minus_btn);
    lv_label_set_text(minus_label, "-");
    lv_obj_center(minus_label);
    lv_obj_add_event_cb(minus_btn, minus_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *plus_btn = lv_btn_create(scr);
    lv_obj_set_size(plus_btn, 90, 44);
    lv_obj_align(plus_btn, LV_ALIGN_CENTER, 80, -30);
    lv_obj_set_style_bg_color(plus_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(plus_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *plus_label = lv_label_create(plus_btn);
    lv_label_set_text(plus_label, "+");
    lv_obj_center(plus_label);
    lv_obj_add_event_cb(plus_btn, plus_click_cb, LV_EVENT_CLICKED, NULL);

    /* Save / Back buttons */
    lv_obj_t *save_btn = lv_btn_create(scr);
    lv_obj_set_size(save_btn, 110, 36);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, -12, -10);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x2E7D32), 0);
    lv_obj_set_style_text_color(save_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "Save");
    lv_obj_center(save_label);
    lv_obj_add_event_cb(save_btn, save_click_cb, LV_EVENT_CLICKED, NULL);

    s_back_btn = lv_btn_create(scr);
    lv_obj_set_size(s_back_btn, 110, 36);
    lv_obj_align(s_back_btn, LV_ALIGN_BOTTOM_LEFT, 12, -10);
    lv_obj_set_style_bg_color(s_back_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(s_back_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *back_label = lv_label_create(s_back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(s_back_btn, back_click_cb, LV_EVENT_CLICKED, NULL);

    return scr;
}
