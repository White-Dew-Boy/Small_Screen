#include "ui_shtc3.h"
#include "lvgl.h"

/* SHTC3 sensor data shared with LVGL (written by the sensor task, read by the
 * LVGL timer callback). LVGL is not thread-safe, so communication happens
 * through these volatile globals only. */
static volatile float s_temp_c = 0.0f;
static volatile float s_humi_rh = 0.0f;
static volatile bool s_shtc3_valid = false;

static lv_obj_t *temp_label;
static lv_obj_t *humi_label;

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
 * @brief Build the SHTC3 sensor dashboard on its own screen.
 */
lv_obj_t *ui_shtc3_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
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

    return scr;
}

void ui_shtc3_set_data(float temp_c, float humi_rh)
{
    s_temp_c = temp_c;
    s_humi_rh = humi_rh;
    s_shtc3_valid = true;
}

void ui_shtc3_set_invalid(void)
{
    s_shtc3_valid = false;
}
