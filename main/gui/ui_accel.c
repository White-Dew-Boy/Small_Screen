#include "ui_accel.h"
#include "lvgl.h"
#include <stdio.h>
#include "jy901s.h"

static lv_obj_t *ax_val;
static lv_obj_t *ay_val;
static lv_obj_t *az_val;

/* Callback to return to the Sensor page (set by main.c, LVGL thread) */
static void (*s_back_cb)(void) = NULL;

void ui_accel_set_back_cb(void (*cb)(void))
{
    s_back_cb = cb;
}

/**
 * @brief Format a float as "int.frac" (2 decimals).
 *        LV_SPRINTF_USE_FLOAT is disabled in this project, so floats are
 *        formatted manually.
 */
static void fmt_float(char *buf, size_t len, float v)
{
    int vi = (int)v;
    int frac = (int)((v - vi) * 100);
    if (frac < 0) {
        frac = -frac;
    }
    snprintf(buf, len, "%d.%02d", vi, frac);
}

/**
 * @brief Create one data row: name label (left, blue) + value label (right, white).
 */
static lv_obj_t *make_row(lv_obj_t *scr, const char *name, int y_off)
{
    lv_obj_t *name_lbl = lv_label_create(scr);
    lv_label_set_text(name_lbl, name);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(0x8AB4F8), 0);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 30, y_off);

    lv_obj_t *val_lbl = lv_label_create(scr);
    lv_label_set_text(val_lbl, "--.-- g");
    lv_obj_set_style_text_color(val_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(val_lbl, LV_ALIGN_RIGHT_MID, -30, y_off);
    return val_lbl;
}

static void accel_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    jy901s_data_t d;
    char b[12];

    if (jy901s_get_data(&d) != ESP_OK) {
        return;
    }
    if (!jy901s_is_online(1000)) {
        lv_label_set_text(ax_val, "--.-- g");
        lv_label_set_text(ay_val, "--.-- g");
        lv_label_set_text(az_val, "--.-- g");
        return;
    }

    fmt_float(b, sizeof(b), d.ax);
    lv_label_set_text_fmt(ax_val, "%s g", b);
    fmt_float(b, sizeof(b), d.ay);
    lv_label_set_text_fmt(ay_val, "%s g", b);
    fmt_float(b, sizeof(b), d.az);
    lv_label_set_text_fmt(az_val, "%s g", b);
}

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_back_cb != NULL) {
        s_back_cb();
    }
}

lv_obj_t *ui_accel_create(void)
{
    /* Landscape 320x240, like the Sensor page it belongs to. */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 240);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Acceleration");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    /* Rows are full-width name/value lines; y_off is relative to the
     * vertical middle (120 in landscape). */
    ax_val = make_row(scr, "Ax", -45);
    ay_val = make_row(scr, "Ay",   0);
    az_val = make_row(scr, "Az",  45);

    lv_obj_t *back_btn = lv_btn_create(scr);
    lv_obj_set_size(back_btn, 140, 40);
    lv_obj_align(back_btn, LV_ALIGN_TOP_MID, 0, 186);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x1565C0), 0);
    lv_obj_set_style_text_color(back_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, back_click_cb, LV_EVENT_CLICKED, NULL);

    /* Refresh every 500ms from the LVGL thread */
    lv_timer_create(accel_timer_cb, 500, NULL);

    return scr;
}
