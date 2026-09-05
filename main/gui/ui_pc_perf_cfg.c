/* PC Config page: live BLE/MQTT link state + data-source selection.
 * Reached from the PC Performance page (top-right "Config" button). */

#include "ui_pc_perf_cfg.h"
#include "lvgl.h"
#include "ble_perf.h"
#include "pc_perf_mqtt.h"
#include "pc_perf_src.h"
#include "esp_timer.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

/* Refresh period of the link-state lines (ms) */
#define CFG_REFRESH_MS 1000

/* Link state is "fresh" when a frame arrived within this window */
#define CFG_STALE_MS 8000

/* Widgets */
static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_ble_lbl;
static lv_obj_t *s_mqtt_lbl;
static lv_obj_t *s_btn[3];
static lv_obj_t *s_msg;

static void (*s_back_cb)(void) = NULL;

void ui_pc_perf_cfg_set_back_cb(void (*cb)(void))
{
    s_back_cb = cb;
}

/* Paint the selected option green, the others grey. */
static void apply_highlight(pc_perf_src_t cur)
{
    static const uint32_t sel   = 0x2E7D32; /* green */
    static const uint32_t unsel = 0x2A323A; /* grey  */

    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(s_btn[i],
                                  lv_color_hex(i == (int)cur ? sel : unsel), 0);
    }
}

/* One option tapped: save and return to the PC-Perf page. */
static void option_click_cb(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    pc_perf_src_t src = (pc_perf_src_t)idx;

    esp_err_t ret = pc_perf_src_set(src);
    if (ret != ESP_OK) {
        lv_label_set_text_fmt(s_msg, "Error: %s", esp_err_to_name(ret));
        lv_obj_set_style_text_color(s_msg, lv_color_hex(0xF44336), 0);
        return;
    }

    apply_highlight(src);
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

/*---------------------------------------------------------------------------
 * Link-state lines (BLE and MQTT, refreshed while this page is visible)
 *---------------------------------------------------------------------------*/

static void set_line(lv_obj_t *lbl, const char *text, uint32_t color)
{
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
}

static void set_line_fmt(lv_obj_t *lbl, uint32_t color, const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    set_line(lbl, buf, color);
}

static void conn_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_scr == NULL || lv_disp_get_scr_act(NULL) != s_scr) {
        return; /* page not visible */
    }

    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    const pc_perf_src_t src = pc_perf_src_get();

    /* ---- BLE link ---- */
    pc_perf_data_t ble;
    pc_perf_get_data(&ble);
    if (src != PC_PERF_SRC_BLE) {
        /* BLE is not the active source: the link was deliberately
         * disconnected and advertising stopped, so it is NOT "waiting for
         * a PC". */
        set_line(s_ble_lbl, src == PC_PERF_SRC_OFF
                                ? "BLE: OFF (source off)"
                                : "BLE: OFF (source MQTT)",
                 0x9E9E9E);
    } else if (!ble.connected) {
        /* Source is BLE. Selecting BLE returns to the PC-Perf page, which
         * starts the peripheral on its first tick, so "no PC yet" really
         * means it is waiting for a connection. */
        set_line(s_ble_lbl, "BLE: waiting for PC...", 0x9E9E9E);
    } else if (!ble.valid ||
               (now_ms - ble.last_update_ms) >= CFG_STALE_MS) {
        set_line(s_ble_lbl, "BLE: connected, no fresh data", 0xFFB300);
    } else {
        set_line_fmt(s_ble_lbl, 0x66BB6A, "BLE: connected (%lu s ago)",
                     (unsigned long)((now_ms - ble.last_update_ms) / 1000));
    }

    /* ---- MQTT link ---- */
    pc_perf_data_t mq;
    pc_perf_mqtt_get_data(&mq); /* connected = broker link up */
    if (!mq.connected) {
        set_line(s_mqtt_lbl, "MQTT: not connected", 0x9E9E9E);
    } else if (!mq.valid) {
        set_line(s_mqtt_lbl, "MQTT: waiting for PC...", 0x9E9E9E);
    } else if ((now_ms - mq.last_update_ms) >= CFG_STALE_MS) {
        set_line(s_mqtt_lbl, "MQTT: connected, no fresh data", 0xFFB300);
    } else {
        set_line_fmt(s_mqtt_lbl, 0x66BB6A, "MQTT: connected (%lu s ago)",
                     (unsigned long)((now_ms - mq.last_update_ms) / 1000));
    }
}

/*---------------------------------------------------------------------------
 * Create
 *---------------------------------------------------------------------------*/

lv_obj_t *ui_pc_perf_cfg_create(void)
{
    /* Landscape 320x240, like the PC Performance page it belongs to. */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 240);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
    s_scr = scr;

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Monitor Config");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    /* Section: connection status (blue heading) */
    lv_obj_t *conn_sec = lv_label_create(scr);
    lv_label_set_text(conn_sec, "Connection Status");
    lv_obj_set_style_text_color(conn_sec, lv_color_hex(0x8AB4F8), 0);
    lv_obj_align(conn_sec, LV_ALIGN_TOP_LEFT, 12, 30);

    /* Connection info: BLE and MQTT link state (color-coded) */
    s_ble_lbl = lv_label_create(scr);
    lv_obj_align(s_ble_lbl, LV_ALIGN_TOP_LEFT, 12, 50);
    set_line(s_ble_lbl, "BLE: off (not started)", 0x9E9E9E);

    s_mqtt_lbl = lv_label_create(scr);
    lv_obj_align(s_mqtt_lbl, LV_ALIGN_TOP_LEFT, 12, 70);
    set_line(s_mqtt_lbl, "MQTT: not connected", 0x9E9E9E);

    lv_timer_create(conn_timer_cb, CFG_REFRESH_MS, NULL);

    /* Section: data source */
    lv_obj_t *sec = lv_label_create(scr);
    lv_label_set_text(sec, "Data source");
    lv_obj_set_style_text_color(sec, lv_color_hex(0x8AB4F8), 0);
    lv_obj_align(sec, LV_ALIGN_TOP_LEFT, 12, 96);

    s_msg = lv_label_create(scr);
    lv_label_set_text(s_msg, "");
    lv_obj_set_style_text_color(s_msg, lv_color_hex(0xF44336), 0);
    lv_obj_align(s_msg, LV_ALIGN_BOTTOM_RIGHT, -12, -10);

    /* Option buttons in ONE row, index == pc_perf_src_t, tucked right
     * under the "Data source" heading */
    static const char *const names[3] = { "MQTT", "BLE", "Off" };
    const int btn_w = 76;  /* 3 x 76 + 2 x 8 gaps = 244, centred */
    const int btn_h = 32;
    const int gap = 8;
    const int row_x = (320 - (3 * btn_w + 2 * gap)) / 2;
    const int row_y = 116;

    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = lv_btn_create(scr);
        lv_obj_set_size(btn, btn_w, btn_h);
        lv_obj_align(btn, LV_ALIGN_TOP_LEFT, row_x + i * (btn_w + gap), row_y);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), 0);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, names[i]);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn, option_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        s_btn[i] = btn;
    }
    apply_highlight(pc_perf_src_get());

    /* Back button */
    lv_obj_t *back_btn = lv_btn_create(scr);
    lv_obj_set_size(back_btn, 110, 34);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_LEFT, 12, -8);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(back_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, back_click_cb, LV_EVENT_CLICKED, NULL);

    return scr;
}
