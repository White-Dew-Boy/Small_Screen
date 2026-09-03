#include "ui_led_custom.h"
#include "lvgl.h"
#include "ui_led.h"

#include <stdio.h>

static lv_obj_t *s_sliders[3];
static lv_obj_t *s_val_labels[3];

/* Set while the sliders are being re-initialized programmatically, so the
 * VALUE_CHANGED callback does not re-apply an identical color. */
static bool s_refreshing = false;

/* Callback to leave the page (set by main.c, invoked from the LVGL thread) */
static void (*s_back_cb)(void) = NULL;

void ui_led_custom_set_back_cb(void (*cb)(void))
{
    s_back_cb = cb;
}

/* A channel slider moved: apply the new RGB to the LED target. */
static void slider_cb(lv_event_t *e)
{
    if (s_refreshing) {
        return;
    }
    lv_obj_t *slider = lv_event_get_target(e);

    int r = (int)lv_slider_get_value(s_sliders[0]);
    int g = (int)lv_slider_get_value(s_sliders[1]);
    int b = (int)lv_slider_get_value(s_sliders[2]);

    lv_label_set_text_fmt(s_val_labels[0], "%d", r);
    lv_label_set_text_fmt(s_val_labels[1], "%d", g);
    lv_label_set_text_fmt(s_val_labels[2], "%d", b);

    ui_led_apply_rgb(r, g, b);
    (void)slider;
}

/* Re-initialize the sliders from the LED page's selected target. */
void ui_led_custom_refresh(void)
{
    int r0, g0, b0;
    ui_led_get_rgb(&r0, &g0, &b0);

    s_refreshing = true;
    lv_slider_set_value(s_sliders[0], r0, LV_ANIM_OFF);
    lv_slider_set_value(s_sliders[1], g0, LV_ANIM_OFF);
    lv_slider_set_value(s_sliders[2], b0, LV_ANIM_OFF);
    s_refreshing = false;

    lv_label_set_text_fmt(s_val_labels[0], "%d", r0);
    lv_label_set_text_fmt(s_val_labels[1], "%d", g0);
    lv_label_set_text_fmt(s_val_labels[2], "%d", b0);
}

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_back_cb != NULL) {
        s_back_cb();
    }
}

lv_obj_t *ui_led_custom_create(void)
{
    /* Landscape 320x240 (matches the LED Control page it belongs to) */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 240);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Custom RGB");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    const char ch_names[3] = { 'R', 'G', 'B' };

    for (int ch = 0; ch < 3; ch++) {
        int y = 52 + ch * 52;

        lv_obj_t *ch_label = lv_label_create(scr);
        lv_label_set_text_fmt(ch_label, "%c", ch_names[ch]);
        lv_obj_set_style_text_color(ch_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(ch_label, LV_ALIGN_TOP_LEFT, 30, y + 8);

        s_sliders[ch] = lv_slider_create(scr);
        lv_obj_set_size(s_sliders[ch], 200, 18);
        lv_obj_align(s_sliders[ch], LV_ALIGN_TOP_LEFT, 56, y + 4);
        lv_slider_set_range(s_sliders[ch], 0, 255);
        lv_slider_set_value(s_sliders[ch], 0, LV_ANIM_OFF);
        lv_obj_add_event_cb(s_sliders[ch], slider_cb, LV_EVENT_VALUE_CHANGED,
                            NULL);

        s_val_labels[ch] = lv_label_create(scr);
        lv_label_set_text(s_val_labels[ch], "0");
        lv_obj_set_style_text_color(s_val_labels[ch], lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(s_val_labels[ch], LV_ALIGN_TOP_LEFT, 270, y + 8);
    }

    /* Load the current target color once at creation, and again on every
     * page entry via ui_led_custom_refresh() */
    ui_led_custom_refresh();

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
