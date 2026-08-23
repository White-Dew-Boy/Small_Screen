#include "ui_saved_wifi.h"
#include "lvgl.h"
#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

/* States of the page */
typedef enum {
    SAVED_IDLE = 0,      /* showing the saved list */
    SAVED_CONNECTING,    /* a connect attempt is in progress */
    SAVED_DONE,          /* success or failure shown */
} saved_mode_t;

/* Widgets */
static lv_obj_t *s_list;
static lv_obj_t *s_msg_label;
static lv_obj_t *s_back_btn;

/* Callback to leave the page (set by main.c, invoked from the LVGL thread) */
static void (*s_back_cb)(void) = NULL;

static saved_mode_t s_mode = SAVED_IDLE;

/* Human-readable text for a WiFi disconnect reason. */
static const char *reason_to_str(int16_t reason)
{
    switch (reason) {
    case 201: return "Cannot find this WiFi";
    case 202: return "Wrong password";
    case 203: return "Association failed";
    case 204: return "Handshake timeout";
    case 205: return "Connection lost";
    case 15:  return "4-way handshake timeout";
    default:  return "Connection failed";
    }
}

void ui_saved_wifi_set_back_cb(void (*cb)(void))
{
    s_back_cb = cb;
}

static void do_back(void)
{
    s_mode = SAVED_IDLE;
    if (s_back_cb != NULL) {
        s_back_cb();
    }
}

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    do_back();
}

/* A saved network was tapped: connect to it. */
static void saved_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0) {
        return;
    }
    const wifi_cred_t *cred = wifi_manager_cred_get(idx);
    if (cred == NULL) {
        return;
    }

    esp_err_t ret = wifi_manager_connect_saved(idx);
    if (ret != ESP_OK) {
        lv_label_set_text_fmt(s_msg_label, "Error: %s", esp_err_to_name(ret));
        return;
    }

    s_mode = SAVED_CONNECTING;
    lv_label_set_text_fmt(s_msg_label, "Connecting to \"%s\"...", cred->ssid);
    lv_obj_set_style_text_color(s_msg_label, lv_color_hex(0xFFC107), 0);
}

/* Poll the connection result while connecting. */
static void poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_mode != SAVED_CONNECTING) {
        return;
    }

    wifi_info_t info;
    wifi_manager_get_info(&info);

    if (info.state == WIFI_STATE_CONNECTED) {
        s_mode = SAVED_DONE;
        lv_label_set_text_fmt(s_msg_label, "Connected to \"%s\"",
                              info.ssid);
        lv_obj_set_style_text_color(s_msg_label, lv_color_hex(0x4CAF50), 0);
    } else if (info.last_reason != 0) {
        /* The link failed; wifi_manager keeps retrying in the background,
         * but we surface the reason now. */
        s_mode = SAVED_DONE;
        lv_label_set_text_fmt(s_msg_label, "Failed: %s",
                              reason_to_str(info.last_reason));
        lv_obj_set_style_text_color(s_msg_label, lv_color_hex(0xF44336), 0);
    }
}

/* Rebuild the list from the saved credential list. */
static void build_saved_list(void)
{
    lv_obj_clean(s_list);

    int count = wifi_manager_cred_count();
    if (count == 0) {
        lv_list_add_text(s_list, "No saved WiFi");
        return;
    }

    for (int i = 0; i < count; i++) {
        const wifi_cred_t *cred = wifi_manager_cred_get(i);
        if (cred == NULL) {
            continue;
        }
        lv_obj_t *btn = lv_list_add_btn(s_list, NULL, cred->ssid);
        lv_obj_set_style_pad_all(btn, 6, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E242B), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), 0);
        lv_obj_add_event_cb(btn, saved_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }
}

void ui_saved_wifi_refresh(void)
{
    s_mode = SAVED_IDLE;
    lv_label_set_text(s_msg_label, "");
    build_saved_list();
}

lv_obj_t *ui_saved_wifi_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Saved WiFi");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    /* Message / status line */
    s_msg_label = lv_label_create(scr);
    lv_label_set_text(s_msg_label, "");
    lv_obj_set_style_text_color(s_msg_label, lv_color_hex(0xFFC107), 0);
    lv_obj_set_style_text_align(s_msg_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_msg_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_msg_label, 220);
    lv_obj_align(s_msg_label, LV_ALIGN_TOP_MID, 0, 36);

    /* Saved network list */
    s_list = lv_list_create(scr);
    lv_obj_set_size(s_list, 240, 200);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x1E242B), 0);
    lv_obj_set_style_border_color(s_list, lv_color_hex(0x3A444E), 0);
    lv_obj_set_style_pad_all(s_list, 4, 0);
    lv_obj_set_style_pad_row(s_list, 3, 0);

    /* Back button */
    s_back_btn = lv_btn_create(scr);
    lv_obj_set_size(s_back_btn, 100, 36);
    lv_obj_align(s_back_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(s_back_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(s_back_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *back_label = lv_label_create(s_back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(s_back_btn, back_click_cb, LV_EVENT_CLICKED, NULL);

    build_saved_list();

    /* Poll connect results every 500 ms */
    lv_timer_create(poll_timer_cb, 500, NULL);

    return scr;
}
