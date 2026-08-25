#include "ui_mqtt_pub.h"
#include "lvgl.h"
#include "mqtt_manager.h"

#include <stdio.h>
#include <string.h>

/* Widgets */
static lv_obj_t *s_count_label;
static lv_obj_t *s_tel_topic_label;
static lv_obj_t *s_status_topic_label;
static lv_obj_t *s_rsp_topic_label;

/* Callback to leave the page (set by main.c, invoked from the LVGL thread) */
static void (*s_back_cb)(void) = NULL;

void ui_mqtt_pub_set_back_cb(void (*cb)(void))
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

/* Create a small-font, wrap-enabled label for one topic line. */
static lv_obj_t *make_topic_label(lv_obj_t *parent, lv_coord_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_color(label, lv_color_hex(0x9FB3C8), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, 216);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 12, y);
    return label;
}

/* LVGL timer: refresh the publish counter. */
static void pub_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    char buf[48];
    snprintf(buf, sizeof(buf), "Published: %lu msg(s)",
             (unsigned long)mqtt_manager_get_publish_count());
    if (strcmp(lv_label_get_text(s_count_label), buf) != 0) {
        lv_label_set_text(s_count_label, buf);
    }
}

lv_obj_t *ui_mqtt_pub_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Publish Topics");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    /* Publish counter */
    s_count_label = lv_label_create(scr);
    lv_label_set_text(s_count_label, "Published: 0 msg(s)");
    lv_obj_set_style_text_color(s_count_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_count_label, LV_ALIGN_TOP_MID, 0, 34);

    /* Telemetry */
    lv_obj_t *tel_hdr = lv_label_create(scr);
    lv_label_set_text(tel_hdr, "Telemetry (every interval, QoS 0)");
    lv_obj_set_style_text_color(tel_hdr, lv_color_hex(0x8A94A0), 0);
    lv_obj_set_style_text_font(tel_hdr, &lv_font_montserrat_12, 0);
    lv_obj_align(tel_hdr, LV_ALIGN_TOP_LEFT, 12, 60);
    s_tel_topic_label = make_topic_label(scr, 78);
    lv_label_set_text(s_tel_topic_label, mqtt_manager_get_telemetry_topic());

    /* Status (online/offline + LWT) */
    lv_obj_t *status_hdr = lv_label_create(scr);
    lv_label_set_text(status_hdr, "Status (retained + LWT, QoS 1)");
    lv_obj_set_style_text_color(status_hdr, lv_color_hex(0x8A94A0), 0);
    lv_obj_set_style_text_font(status_hdr, &lv_font_montserrat_12, 0);
    lv_obj_align(status_hdr, LV_ALIGN_TOP_LEFT, 12, 106);
    s_status_topic_label = make_topic_label(scr, 124);
    lv_label_set_text(s_status_topic_label, mqtt_manager_get_status_topic());

    /* Command response */
    lv_obj_t *rsp_hdr = lv_label_create(scr);
    lv_label_set_text(rsp_hdr, "Command Response (QoS 1)");
    lv_obj_set_style_text_color(rsp_hdr, lv_color_hex(0x8A94A0), 0);
    lv_obj_set_style_text_font(rsp_hdr, &lv_font_montserrat_12, 0);
    lv_obj_align(rsp_hdr, LV_ALIGN_TOP_LEFT, 12, 152);
    s_rsp_topic_label = make_topic_label(scr, 170);
    lv_label_set_text(s_rsp_topic_label, mqtt_manager_get_cmd_resp_topic());

    /* Back button */
    lv_obj_t *back_btn = lv_btn_create(scr);
    lv_obj_set_size(back_btn, 100, 36);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(back_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, back_click_cb, LV_EVENT_CLICKED, NULL);

    /* Refresh the counter every 500 ms from the LVGL thread */
    lv_timer_create(pub_timer_cb, 500, NULL);

    return scr;
}
