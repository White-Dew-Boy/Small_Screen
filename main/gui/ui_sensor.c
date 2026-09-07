#include "ui_sensor.h"
#include "lvgl.h"
#include <stdio.h>

/* SHTC3 sensor data shared with LVGL (written by the sensor task, read by the
 * LVGL timer callback). LVGL is not thread-safe, so communication happens
 * through these volatile globals only. */
static volatile float s_temp_c = 0.0f;
static volatile float s_humi_rh = 0.0f;
static volatile bool s_shtc3_valid = false;

/* JY901S IMU data (same sharing scheme as above) */
static volatile float s_roll = 0.0f;
static volatile float s_pitch = 0.0f;
static volatile float s_yaw = 0.0f;
static volatile float s_imu_temp = 0.0f;
static volatile bool s_imu_valid = false;

/* Value labels (name labels are created once and never change) */
static lv_obj_t *temp_val;
static lv_obj_t *humi_val;
static lv_obj_t *imu_temp_val;
static lv_obj_t *roll_val;
static lv_obj_t *pitch_val;
static lv_obj_t *yaw_val;

/* Callbacks to open the accel / gyro sub-pages (set by main.c, LVGL thread) */
static void (*s_accel_cb)(void) = NULL;
static void (*s_gyro_cb)(void) = NULL;

void ui_sensor_set_accel_cb(void (*cb)(void))
{
    s_accel_cb = cb;
}

void ui_sensor_set_gyro_cb(void (*cb)(void))
{
    s_gyro_cb = cb;
}

/**
 * @brief Format a float as "int.frac" (one decimal) into a buffer.
 *        LV_SPRINTF_USE_FLOAT is disabled in this project, so floats are
 *        formatted manually.
 */
static void fmt_float(char *buf, size_t len, float v)
{
    int vi = (int)v;
    int frac = (int)((v - vi) * 10);
    if (frac < 0) {
        frac = -frac;
    }
    snprintf(buf, len, "%d.%d", vi, frac);
}

/**
 * @brief Create one data row: a name label on the left and a value label on
 *        the right. Returns the value label so the timer callback can update it.
 */
static lv_obj_t *make_row(lv_obj_t *scr, const char *name, int y_off)
{
    lv_obj_t *name_lbl = lv_label_create(scr);
    lv_label_set_text(name_lbl, name);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(0x8AB4F8), 0);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 30, y_off);

    lv_obj_t *val_lbl = lv_label_create(scr);
    lv_label_set_text(val_lbl, "--.-");
    lv_obj_set_style_text_color(val_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(val_lbl, LV_ALIGN_RIGHT_MID, -30, y_off);
    return val_lbl;
}

/**
 * @brief LVGL timer callback: refresh all sensor labels on screen.
 *        Runs inside lv_timer_handler(), so it is safe to touch LVGL objects here.
 */
static void sensor_display_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    char buf[12];

    /* SHTC3 temperature / humidity */
    if (s_shtc3_valid) {
        fmt_float(buf, sizeof(buf), s_temp_c);
        lv_label_set_text_fmt(temp_val, "%s C", buf);
        fmt_float(buf, sizeof(buf), s_humi_rh);
        lv_label_set_text_fmt(humi_val, "%s %%RH", buf);
    } else {
        lv_label_set_text(temp_val, "--.- C");
        lv_label_set_text(humi_val, "--.- %RH");
    }

    /* JY901S attitude + module temperature */
    if (s_imu_valid) {
        fmt_float(buf, sizeof(buf), s_imu_temp);
        lv_label_set_text_fmt(imu_temp_val, "%s C", buf);

        fmt_float(buf, sizeof(buf), s_roll);
        lv_label_set_text_fmt(roll_val, "%s deg", buf);
        fmt_float(buf, sizeof(buf), s_pitch);
        lv_label_set_text_fmt(pitch_val, "%s deg", buf);
        fmt_float(buf, sizeof(buf), s_yaw);
        lv_label_set_text_fmt(yaw_val, "%s deg", buf);
    } else {
        lv_label_set_text(imu_temp_val, "--.- C");
        lv_label_set_text(roll_val, "--.- deg");
        lv_label_set_text(pitch_val, "--.- deg");
        lv_label_set_text(yaw_val, "--.- deg");
    }
}

/* Accel / Gyro sub-page entry buttons */
static void accel_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_accel_cb != NULL) {
        s_accel_cb();
    }
}

static void gyro_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_gyro_cb != NULL) {
        s_gyro_cb();
    }
}

/**
 * @brief Build the sensor dashboard (SHTC3 + JY901S) on its own screen.
 *        SHTC3 and attitude rows use a name/value layout; accel/gyro live on
 *        their own sub-pages reached via the Accel / Gyro buttons.
 */
lv_obj_t *ui_sensor_create(void)
{
    /* Landscape 320x240, like the Home/PC-Perf pages: main.c rotates the
     * whole display to landscape before this screen is loaded. */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 240);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    /* The default LVGL theme renders text in gray — set white explicitly
     * so the labels are clearly readable on the dark background. */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Sensor");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    /* Data rows are full-width name/value lines. y_off is relative to the
     * screen's vertical middle (120 in landscape); rows step 26 px:
     * 6 rows span y=48..178, leaving the bottom strip for the buttons. */
    temp_val = make_row(scr, "Temperature", -72);
    humi_val = make_row(scr, "Humidity",    -46);

    /* JY901S rows */
    imu_temp_val = make_row(scr, "IMU Temp",  -20);
    roll_val     = make_row(scr, "Roll",        6);
    pitch_val    = make_row(scr, "Pitch",      32);
    yaw_val      = make_row(scr, "Yaw",        58);

    /* Sub-page entry buttons (bottom corners) */
    lv_obj_t *accel_btn = lv_btn_create(scr);
    lv_obj_set_size(accel_btn, 140, 40);
    lv_obj_align(accel_btn, LV_ALIGN_TOP_LEFT, 12, 190);
    lv_obj_set_style_bg_color(accel_btn, lv_color_hex(0x1565C0), 0);
    lv_obj_set_style_text_color(accel_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *accel_label = lv_label_create(accel_btn);
    lv_label_set_text(accel_label, "Accel");
    lv_obj_center(accel_label);
    lv_obj_add_event_cb(accel_btn, accel_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *gyro_btn = lv_btn_create(scr);
    lv_obj_set_size(gyro_btn, 140, 40);
    lv_obj_align(gyro_btn, LV_ALIGN_TOP_RIGHT, -12, 190);
    lv_obj_set_style_bg_color(gyro_btn, lv_color_hex(0x2E7D32), 0);
    lv_obj_set_style_text_color(gyro_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *gyro_label = lv_label_create(gyro_btn);
    lv_label_set_text(gyro_label, "Gyro");
    lv_obj_center(gyro_label);
    lv_obj_add_event_cb(gyro_btn, gyro_click_cb, LV_EVENT_CLICKED, NULL);

    /* Refresh labels every 500ms from the LVGL thread */
    lv_timer_create(sensor_display_timer_cb, 500, NULL);

    return scr;
}

void ui_sensor_set_data(float temp_c, float humi_rh)
{
    s_temp_c = temp_c;
    s_humi_rh = humi_rh;
    s_shtc3_valid = true;
}

void ui_sensor_set_imu_data(const jy901s_data_t *imu, bool online)
{
    s_roll     = imu->roll;
    s_pitch    = imu->pitch;
    s_yaw      = imu->yaw;
    s_imu_temp = imu->temp;
    s_imu_valid = online;
}

void ui_sensor_set_invalid(void)
{
    s_shtc3_valid = false;
}
