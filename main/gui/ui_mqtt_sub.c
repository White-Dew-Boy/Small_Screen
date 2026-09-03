#include "ui_mqtt_sub.h"
#include "lvgl.h"
#include "mqtt_manager.h"

/* Widgets */
static lv_obj_t *s_cmd_topic_label;

/* Callback to leave the page (set by main.c, invoked from the LVGL thread) */
static void (*s_back_cb)(void) = NULL;

void ui_mqtt_sub_set_back_cb(void (*cb)(void))
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

lv_obj_t *ui_mqtt_sub_create(void)
{
    /* Landscape 320x240, like the MQTT status page it belongs to. */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 240);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Subscribe Topic");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    /* Subscribed command topic (left half) */
    lv_obj_t *cmd_hdr = lv_label_create(scr);
    lv_label_set_text(cmd_hdr, "Cmd Topic (QoS 1)");
    lv_obj_set_style_text_color(cmd_hdr, lv_color_hex(0x8A94A0), 0);
    lv_obj_set_style_text_font(cmd_hdr, &lv_font_montserrat_12, 0);
    lv_obj_align(cmd_hdr, LV_ALIGN_TOP_LEFT, 12, 36);

    s_cmd_topic_label = lv_label_create(scr);
    lv_label_set_text(s_cmd_topic_label, mqtt_manager_get_cmd_topic());
    lv_obj_set_style_text_color(s_cmd_topic_label, lv_color_hex(0x9FB3C8), 0);
    lv_obj_set_style_text_font(s_cmd_topic_label, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(s_cmd_topic_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_cmd_topic_label, 140);
    lv_obj_align(s_cmd_topic_label, LV_ALIGN_TOP_LEFT, 12, 52);

    /* Supported commands (right half, so examples have room to breathe) */
    lv_obj_t *help_hdr = lv_label_create(scr);
    lv_label_set_text(help_hdr, "Supported Commands");
    lv_obj_set_style_text_color(help_hdr, lv_color_hex(0x8A94A0), 0);
    lv_obj_set_style_text_font(help_hdr, &lv_font_montserrat_12, 0);
    lv_obj_align(help_hdr, LV_ALIGN_TOP_LEFT, 172, 36);

    static const char *cmd_examples[] = {
        "{\"cmd\":\"ping\"}",
        "{\"cmd\":\"reboot\"}",
        "{\"cmd\":\"set_interval\",\"value\":5}",
    };
    lv_coord_t y = 56;
    for (size_t i = 0; i < sizeof(cmd_examples) / sizeof(cmd_examples[0]); i++) {
        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, cmd_examples[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x80CBC4), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 172, y);
        y += 20;
    }

    /* Note line */
    lv_obj_t *note = lv_label_create(scr);
    lv_label_set_text(note, "Reply is published on cmd_resp topic");
    lv_obj_set_style_text_color(note, lv_color_hex(0x8A94A0), 0);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_12, 0);
    lv_obj_align(note, LV_ALIGN_TOP_LEFT, 172, 124);

    /* Back button */
    lv_obj_t *back_btn = lv_btn_create(scr);
    lv_obj_set_size(back_btn, 100, 36);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(back_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, back_click_cb, LV_EVENT_CLICKED, NULL);

    return scr;
}
