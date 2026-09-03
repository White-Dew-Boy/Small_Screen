#include "ui_mqtt_history.h"
#include "lvgl.h"
#include "mqtt_manager.h"

#include <stdio.h>
#include <string.h>

/* Widgets */
static lv_obj_t *s_scr;
static lv_obj_t *s_count_label;
static lv_obj_t *s_list;
static lv_obj_t *s_back_btn;

/* Callback to leave the page (set by main.c, invoked from the LVGL thread) */
static void (*s_back_cb)(void) = NULL;

void ui_mqtt_history_set_back_cb(void (*cb)(void))
{
    s_back_cb = cb;
}

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_back_cb != NULL) {
        s_back_cb();
    }
}

/* Rebuild the command list from the ring buffer. */
static void build_history_list(void)
{
    lv_obj_clean(s_list);

    mqtt_cmd_entry_t entries[MQTT_CMD_HISTORY_MAX];
    size_t n = mqtt_manager_get_cmd_history(entries, MQTT_CMD_HISTORY_MAX);

    if (n == 0) {
        lv_list_add_text(s_list, "No commands received");
        return;
    }

    for (size_t i = 0; i < n; i++) {
        lv_obj_t *row = lv_list_add_text(s_list, entries[i].text);
        lv_obj_set_style_text_color(row, lv_color_hex(0xFFFFFF), 0);
    }
}

/* LVGL timer: refresh publish counter and command list. Only runs while
 * the page is on screen — otherwise a hidden page rebuilds its list on
 * every MQTT publish (~6 s), adding pointless widget churn to the LVGL
 * task (see the visibility gate pattern in ui_sd.c). */
static void history_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (s_scr == NULL || lv_disp_get_scr_act(NULL) != s_scr) {
        return; /* page not visible */
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "Published: %lu msg(s)",
             (unsigned long)mqtt_manager_get_publish_count());
    if (strcmp(lv_label_get_text(s_count_label), buf) != 0) {
        lv_label_set_text(s_count_label, buf);
        build_history_list();
    }
}

lv_obj_t *ui_mqtt_history_create(void)
{
    /* Landscape 320x240, like the MQTT status page it belongs to. */
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_set_size(scr, 320, 240);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "MQTT History");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    /* Publish counter */
    s_count_label = lv_label_create(scr);
    lv_label_set_text(s_count_label, "Published: 0 msg(s)");
    lv_obj_set_style_text_color(s_count_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_count_label, LV_ALIGN_TOP_MID, 0, 30);

    /* Command history list */
    s_list = lv_list_create(scr);
    lv_obj_set_size(s_list, 296, 142);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x1E242B), 0);
    lv_obj_set_style_border_color(s_list, lv_color_hex(0x3A444E), 0);
    lv_obj_set_style_pad_all(s_list, 4, 0);
    lv_obj_set_style_pad_row(s_list, 3, 0);

    /* Back button */
    s_back_btn = lv_btn_create(scr);
    lv_obj_set_size(s_back_btn, 100, 36);
    lv_obj_align(s_back_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(s_back_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(s_back_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *back_label = lv_label_create(s_back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(s_back_btn, back_click_cb, LV_EVENT_CLICKED, NULL);

    build_history_list();

    /* Refresh every 500 ms from the LVGL thread */
    lv_timer_create(history_timer_cb, 500, NULL);

    return scr;
}
