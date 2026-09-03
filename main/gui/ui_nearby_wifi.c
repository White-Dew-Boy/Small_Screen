#include "ui_nearby_wifi.h"
#include "lvgl.h"
#include "wifi_manager.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_nearby_wifi";

/* Page states */
typedef enum {
    NF_LIST = 0,      /* scanning or showing the AP list */
    NF_SCANNING,      /* a scan is in progress */
    NF_PASSWORD,      /* entering the password for a selected AP */
} nf_mode_t;

/* List-view widgets */
static lv_obj_t *s_title;
static lv_obj_t *s_scan_label;
static lv_obj_t *s_ap_list;
static lv_obj_t *s_back_btn;

/* Password-view widgets (all children of s_pass_view) */
static lv_obj_t *s_pass_view;
static lv_obj_t *s_pass_label;
static lv_obj_t *s_pass_ta;
static lv_obj_t *s_pass_toggle_btn;
static lv_obj_t *s_msg_label;
static lv_obj_t *s_kb;

/* Callback to leave the page (set by main.c, invoked from the LVGL thread) */
static void (*s_back_cb)(void) = NULL;

static nf_mode_t s_mode = NF_LIST;
static wifi_scan_result_t s_ap_results[WIFI_SCAN_MAX_RESULTS];
static size_t s_ap_count = 0;
static char s_sel_ssid[33];

void ui_nearby_wifi_set_back_cb(void (*cb)(void))
{
    s_back_cb = cb;
}

/* Convert RSSI [dBm] to a signal strength bar string. */
static const char *rssi_bar(int8_t rssi)
{
    if (rssi >= -50) return "****";
    if (rssi >= -65) return "*** ";
    if (rssi >= -75) return "**  ";
    return "*   ";
}

/* Show the list view (scan label + AP list + Back button). */
static void set_view_list(void)
{
    lv_obj_add_flag(s_pass_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_scan_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ap_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
}

/* Show the password-entry view. The main title is hidden too, because the
 * password view draws its own "Password for <ssid>" line at the top. */
static void set_view_pass(void)
{
    lv_obj_clear_flag(s_pass_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_scan_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ap_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
}

/* Set password visibility; keeps the toggle button text in sync
 * ("SH" = hidden, tap to show / "HD" = shown, tap to hide). */
static void set_pass_visible(bool show)
{
    lv_textarea_set_password_mode(s_pass_ta, !show);
    lv_obj_t *lbl = lv_obj_get_child(s_pass_toggle_btn, 0);
    if (lbl != NULL) {
        lv_label_set_text(lbl, show ? "HD" : "SH");
    }
}

static void pass_toggle_cb(lv_event_t *e)
{
    (void)e;
    bool hidden = lv_textarea_get_password_mode(s_pass_ta);
    set_pass_visible(hidden); /* currently hidden -> show it */
}

static void ap_click_cb(lv_event_t *e);

/* Fill the list with the scan results. */
static void populate_list(void)
{
    lv_obj_clean(s_ap_list);

    s_ap_count = wifi_manager_scan_get_results(s_ap_results,
                                               WIFI_SCAN_MAX_RESULTS);
    if (s_ap_count == 0) {
        lv_list_add_text(s_ap_list, "No networks found");
        return;
    }

    for (size_t i = 0; i < s_ap_count; i++) {
        char ssid_short[17];
        strlcpy(ssid_short, s_ap_results[i].ssid, sizeof(ssid_short));
        char text[48];
        snprintf(text, sizeof(text), "%s  %s", ssid_short,
                 rssi_bar(s_ap_results[i].rssi));

        lv_obj_t *btn = lv_list_add_btn(s_ap_list, NULL, text);
        lv_obj_set_style_pad_all(btn, 6, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E242B), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), 0);
        lv_obj_add_event_cb(btn, ap_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }
}

/* An AP was tapped: switch to the password view for it. */
static void ap_click_cb(lv_event_t *e)
{
    size_t idx = (size_t)lv_event_get_user_data(e);
    if (idx >= s_ap_count) {
        return;
    }

    strlcpy(s_sel_ssid, s_ap_results[idx].ssid, sizeof(s_sel_ssid));
    lv_label_set_text_fmt(s_pass_label, "Password for %s", s_sel_ssid);
    lv_textarea_set_text(s_pass_ta, "");
    set_pass_visible(false); /* start hidden */
    lv_keyboard_set_textarea(s_kb, s_pass_ta);
    lv_label_set_text(s_msg_label, "");

    s_mode = NF_PASSWORD;
    set_view_pass();
}

/* Confirm: save credentials and connect, then leave the page. */
static void confirm_click_cb(lv_event_t *e)
{
    (void)e;
    const char *pass = lv_textarea_get_text(s_pass_ta);

    ESP_LOGI(TAG, "Connecting to \"%s\"", s_sel_ssid);
    esp_err_t ret = wifi_manager_set_credentials(s_sel_ssid, pass);
    if (ret != ESP_OK) {
        lv_label_set_text_fmt(s_msg_label, "Error: %s", esp_err_to_name(ret));
        return;
    }

    lv_label_set_text(s_msg_label, "Saved, connecting...");
    if (s_back_cb != NULL) {
        s_back_cb();
    }
}

/* Password-view Back: return to the AP list. */
static void pass_back_cb(lv_event_t *e)
{
    (void)e;
    s_mode = NF_LIST;
    lv_label_set_text(s_scan_label, "");
    set_view_list();
}

/* List-view Back: leave the page. */
static void list_back_cb(lv_event_t *e)
{
    (void)e;
    if (s_back_cb != NULL) {
        s_back_cb();
    }
}

/* Keyboard: Enter = confirm, X = back to the list. */
static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        confirm_click_cb(e);
    } else if (code == LV_EVENT_CANCEL) {
        pass_back_cb(e);
    }
}

/* LVGL timer: poll until the scan finishes, then fill the list. */
static void scan_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_mode != NF_SCANNING) {
        return;
    }
    if (wifi_manager_scan_in_progress()) {
        return;
    }
    populate_list();
    s_mode = NF_LIST;
    lv_label_set_text(s_scan_label, "");
}

void ui_nearby_wifi_start_scan(void)
{
    if (wifi_manager_scan_in_progress()) {
        return;
    }

    esp_err_t ret = wifi_manager_scan_start();
    if (ret != ESP_OK) {
        lv_label_set_text_fmt(s_scan_label, "Scan error: %s",
                              esp_err_to_name(ret));
        s_mode = NF_LIST;
        return;
    }

    s_mode = NF_SCANNING;
    lv_label_set_text(s_scan_label, "Scanning...");
    lv_obj_clean(s_ap_list);
    set_view_list();
}

lv_obj_t *ui_nearby_wifi_create(void)
{
    /* Landscape 320x240, like the WiFi status page it belongs to. */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 240);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Nearby WiFi");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    s_title = title;

    /* --- List view --- */
    s_scan_label = lv_label_create(scr);
    lv_label_set_text(s_scan_label, "");
    lv_obj_set_style_text_color(s_scan_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(s_scan_label, LV_ALIGN_TOP_MID, 0, 30);

    s_ap_list = lv_list_create(scr);
    lv_obj_set_size(s_ap_list, 296, 142);
    lv_obj_align(s_ap_list, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_bg_color(s_ap_list, lv_color_hex(0x1E242B), 0);
    lv_obj_set_style_border_color(s_ap_list, lv_color_hex(0x3A444E), 0);
    lv_obj_set_style_pad_all(s_ap_list, 4, 0);
    lv_obj_set_style_pad_row(s_ap_list, 3, 0);

    s_back_btn = lv_btn_create(scr);
    lv_obj_set_size(s_back_btn, 100, 36);
    lv_obj_align(s_back_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(s_back_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(s_back_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *back_label = lv_label_create(s_back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(s_back_btn, list_back_cb, LV_EVENT_CLICKED, NULL);

    /* --- Password view (hidden until an AP is tapped) --- */
    s_pass_view = lv_obj_create(scr);
    lv_obj_set_size(s_pass_view, 320, 240);
    lv_obj_set_pos(s_pass_view, 0, 0);
    lv_obj_set_style_bg_opa(s_pass_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pass_view, 0, 0);
    lv_obj_add_flag(s_pass_view, LV_OBJ_FLAG_HIDDEN);

    s_pass_label = lv_label_create(s_pass_view);
    lv_label_set_text(s_pass_label, "Password");
    lv_obj_set_style_text_color(s_pass_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_pass_label, LV_ALIGN_TOP_MID, 0, 4);

    s_pass_ta = lv_textarea_create(s_pass_view);
    lv_obj_set_size(s_pass_ta, 200, 32);
    lv_obj_align(s_pass_ta, LV_ALIGN_TOP_LEFT, 12, 20);
    lv_obj_set_style_bg_color(s_pass_ta, lv_color_hex(0x1E242B), 0);
    lv_obj_set_style_text_color(s_pass_ta, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_color(s_pass_ta, lv_color_hex(0x3A444E), 0);
    lv_textarea_set_one_line(s_pass_ta, true);
    lv_textarea_set_max_length(s_pass_ta, 63);
    lv_textarea_set_placeholder_text(s_pass_ta, "Password");
    lv_textarea_set_password_mode(s_pass_ta, true);

    s_pass_toggle_btn = lv_btn_create(s_pass_view);
    lv_obj_set_size(s_pass_toggle_btn, 40, 32);
    lv_obj_align(s_pass_toggle_btn, LV_ALIGN_TOP_LEFT, 216, 20);
    lv_obj_set_style_bg_color(s_pass_toggle_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(s_pass_toggle_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *toggle_label = lv_label_create(s_pass_toggle_btn);
    lv_label_set_text(toggle_label, "SH");
    lv_obj_center(toggle_label);
    lv_obj_add_event_cb(s_pass_toggle_btn, pass_toggle_cb, LV_EVENT_CLICKED, NULL);

    s_msg_label = lv_label_create(s_pass_view);
    lv_label_set_text(s_msg_label, "");
    lv_obj_set_style_text_color(s_msg_label, lv_color_hex(0xF44336), 0);
    lv_obj_align(s_msg_label, LV_ALIGN_TOP_MID, 0, 56);

    lv_obj_t *pass_back_btn = lv_btn_create(s_pass_view);
    lv_obj_set_size(pass_back_btn, 140, 32);
    lv_obj_align(pass_back_btn, LV_ALIGN_TOP_LEFT, 12, 72);
    lv_obj_set_style_bg_color(pass_back_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(pass_back_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *pback_label = lv_label_create(pass_back_btn);
    lv_label_set_text(pback_label, "Back");
    lv_obj_center(pback_label);
    lv_obj_add_event_cb(pass_back_btn, pass_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *confirm_btn = lv_btn_create(s_pass_view);
    lv_obj_set_size(confirm_btn, 140, 32);
    lv_obj_align(confirm_btn, LV_ALIGN_TOP_RIGHT, -12, 72);
    lv_obj_set_style_bg_color(confirm_btn, lv_color_hex(0x2E7D32), 0);
    lv_obj_set_style_text_color(confirm_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *confirm_label = lv_label_create(confirm_btn);
    lv_label_set_text(confirm_label, "Confirm");
    lv_obj_center(confirm_label);
    lv_obj_add_event_cb(confirm_btn, confirm_click_cb, LV_EVENT_CLICKED, NULL);

    s_kb = lv_keyboard_create(s_pass_view);
    lv_obj_set_size(s_kb, 320, 132);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_kb, s_pass_ta);
    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_CANCEL, NULL);

    /* Poll for scan completion from the LVGL thread */
    lv_timer_create(scan_poll_cb, 200, NULL);

    return scr;
}
