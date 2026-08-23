#include "ui_wifi_config.h"
#include "lvgl.h"
#include "wifi_manager.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_wifi_config";

/* View mode of the config page: keyboard (manual entry) vs. AP list. */
typedef enum {
    CFG_MODE_MANUAL = 0, /* keyboard visible, manual SSID/password entry */
    CFG_MODE_SCANNING,   /* scan started, list shows "Scanning..." */
    CFG_MODE_LIST,       /* scan finished, list shows nearby APs */
} cfg_mode_t;

/* Widgets */
static lv_obj_t *ssid_ta;
static lv_obj_t *pass_ta;
static lv_obj_t *msg_label;
static lv_obj_t *s_kb;
static lv_obj_t *s_ap_list;
static lv_obj_t *s_scan_label;
static lv_obj_t *s_pass_toggle_btn; /* password show/hide button */

/* Callback to leave the page (set by main.c, invoked from the LVGL thread) */
static void (*s_back_cb)(void) = NULL;

/* UI-side copy of the last scan results (click handlers need the SSID) */
static wifi_scan_result_t s_ap_results[WIFI_SCAN_MAX_RESULTS];
static size_t s_ap_count = 0;
static cfg_mode_t s_mode = CFG_MODE_MANUAL;

void ui_wifi_config_set_back_cb(void (*cb)(void))
{
    s_back_cb = cb;
}

/* Show either the keyboard (manual entry) or the scan list view. */
static void set_view(bool keyboard)
{
    if (keyboard) {
        lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ap_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_scan_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_ap_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_scan_label, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Leave the config page */
static void do_back(void)
{
    s_mode = CFG_MODE_MANUAL;
    set_view(true); /* restore the keyboard view */
    if (s_back_cb != NULL) {
        s_back_cb();
    }
}

/* Start connecting with the entered credentials. */
static void do_connect(void)
{
    const char *ssid = lv_textarea_get_text(ssid_ta);
    const char *pass = lv_textarea_get_text(pass_ta);

    if (ssid == NULL || strlen(ssid) == 0) {
        lv_label_set_text(msg_label, "SSID is empty");
        return;
    }

    ESP_LOGI(TAG, "Connecting to \"%s\"", ssid);
    esp_err_t ret = wifi_manager_set_credentials(ssid, pass);
    if (ret != ESP_OK) {
        lv_label_set_text_fmt(msg_label, "Error: %s", esp_err_to_name(ret));
        return;
    }

    lv_label_set_text(msg_label, "Saved, connecting...");
    do_back();
}

static void connect_click_cb(lv_event_t *e)
{
    (void)e;
    do_connect();
}

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    do_back();
}

/* Keyboard: Enter key = connect, X key = back */
static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        do_connect();
    } else if (code == LV_EVENT_CANCEL) {
        do_back();
    }
}

/* Set password visibility; keeps the toggle button text in sync
 * ("SH" = hidden, tap to show / "HD" = shown, tap to hide). */
static void set_pass_visible(bool show)
{
    lv_textarea_set_password_mode(pass_ta, !show);
    if (s_pass_toggle_btn != NULL) {
        lv_obj_t *label = lv_obj_get_child(s_pass_toggle_btn, 0);
        if (label != NULL) {
            lv_label_set_text(label, show ? "HD" : "SH");
        }
    }
}

/* Toggle password visibility on button press. */
static void pass_toggle_cb(lv_event_t *e)
{
    (void)e;
    bool hidden = lv_textarea_get_password_mode(pass_ta);
    set_pass_visible(hidden); /* currently hidden -> show it */
}

/* Keyboard follows the focused input box: clicking the SSID field types
 * into SSID, clicking the password field types into the password. */
static void ta_focus_cb(lv_event_t *e)
{
    if (s_kb == NULL) {
        return;
    }
    lv_obj_t *ta = lv_event_get_target(e);
    lv_keyboard_set_textarea(s_kb, ta);
}

/* Helper to style a one-line textarea */
static lv_obj_t *make_textarea(lv_obj_t *parent, const char *placeholder,
                               uint32_t max_len, bool password)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_style_bg_color(ta, lv_color_hex(0x1E242B), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(0x3A444E), 0);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max_len);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_password_mode(ta, password);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    return ta;
}

/* Convert RSSI [dBm] to a signal strength bar string. */
static const char *rssi_bar(int8_t rssi)
{
    if (rssi >= -50) return "****";
    if (rssi >= -65) return "*** ";
    if (rssi >= -75) return "**  ";
    return "*   ";
}

/* A network was picked from the list: fill the SSID field, then let the
 * user type only the password. */
static void ap_click_cb(lv_event_t *e)
{
    size_t idx = (size_t)lv_event_get_user_data(e);
    if (idx >= s_ap_count) {
        return;
    }

    lv_textarea_set_text(ssid_ta, s_ap_results[idx].ssid);
    lv_textarea_set_text(pass_ta, "");
    set_pass_visible(false); /* reset to hidden */
    lv_keyboard_set_textarea(s_kb, pass_ta);

    s_mode = CFG_MODE_MANUAL;
    set_view(true);
    lv_label_set_text(msg_label, "Enter password, then Connect");
}

/* Fallback row: go back to typing the SSID by hand. */
static void manual_click_cb(lv_event_t *e)
{
    (void)e;
    lv_textarea_set_text(ssid_ta, "");
    lv_keyboard_set_textarea(s_kb, ssid_ta);

    s_mode = CFG_MODE_MANUAL;
    set_view(true);
    lv_label_set_text(msg_label, "Enter SSID and password");
}

/* Fill the list with the scan results (plus a manual-entry row). */
static void populate_list(void)
{
    lv_obj_clean(s_ap_list);

    lv_obj_t *manual_btn = lv_list_add_btn(s_ap_list, NULL, "Enter SSID manually");
    lv_obj_set_style_pad_all(manual_btn, 6, 0);
    lv_obj_set_style_bg_color(manual_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(manual_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_add_event_cb(manual_btn, manual_click_cb, LV_EVENT_CLICKED, NULL);

    s_ap_count = wifi_manager_scan_get_results(s_ap_results, WIFI_SCAN_MAX_RESULTS);
    if (s_ap_count == 0) {
        lv_list_add_text(s_ap_list, "No networks found");
        return;
    }

    for (size_t i = 0; i < s_ap_count; i++) {
        /* Keep each row short enough for the 240 px screen */
        char ssid_short[17];
        strlcpy(ssid_short, s_ap_results[i].ssid, sizeof(ssid_short));
        char text[48];
        snprintf(text, sizeof(text), "%s  %s", ssid_short,
                 rssi_bar(s_ap_results[i].rssi));

        lv_obj_t *btn = lv_list_add_btn(s_ap_list, NULL, text);
        lv_obj_set_style_pad_all(btn, 6, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E242B), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), 0);
        lv_obj_add_event_cb(btn, ap_click_cb, LV_EVENT_CLICKED, (void *)i);
    }
}

/* Start an asynchronous scan and show the list view. */
static void scan_click_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t ret = wifi_manager_scan_start();
    if (ret != ESP_OK) {
        lv_label_set_text_fmt(msg_label, "Scan error: %s", esp_err_to_name(ret));
        return;
    }

    s_mode = CFG_MODE_SCANNING;
    lv_label_set_text(msg_label, "");
    lv_label_set_text(s_scan_label, "Scanning...");
    lv_obj_clean(s_ap_list);
    set_view(false);
}

/* LVGL timer: while scanning, poll until the driver finishes, then fill
 * the AP list. Runs from the LVGL thread, so LVGL calls here are safe. */
static void scan_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_mode != CFG_MODE_SCANNING) {
        return;
    }
    if (wifi_manager_scan_in_progress()) {
        return;
    }
    populate_list();
    s_mode = CFG_MODE_LIST;
}

lv_obj_t *ui_wifi_config_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "WiFi Config");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    /* SSID input */
    ssid_ta = make_textarea(scr, "SSID", 32, false);
    lv_obj_set_size(ssid_ta, 216, 34);
    lv_obj_align(ssid_ta, LV_ALIGN_TOP_MID, 0, 32);

    /* Password input + show/hide toggle */
    pass_ta = make_textarea(scr, "Password", 63, true);
    lv_obj_set_size(pass_ta, 172, 34);
    lv_obj_align(pass_ta, LV_ALIGN_TOP_LEFT, 12, 72);

    s_pass_toggle_btn = lv_btn_create(scr);
    lv_obj_set_size(s_pass_toggle_btn, 40, 34);
    lv_obj_align(s_pass_toggle_btn, LV_ALIGN_TOP_RIGHT, -12, 72);
    lv_obj_set_style_bg_color(s_pass_toggle_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(s_pass_toggle_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *toggle_label = lv_label_create(s_pass_toggle_btn);
    lv_label_set_text(toggle_label, "SH"); /* password starts hidden */
    lv_obj_center(toggle_label);
    lv_obj_add_event_cb(s_pass_toggle_btn, pass_toggle_cb, LV_EVENT_CLICKED, NULL);

    /* Status/error message */
    msg_label = lv_label_create(scr);
    lv_label_set_text(msg_label, "");
    lv_obj_set_style_text_color(msg_label, lv_color_hex(0xF44336), 0);
    lv_obj_align(msg_label, LV_ALIGN_TOP_MID, 0, 112);

    /* Action buttons: Scan | Back | Connect */
    lv_obj_t *scan_btn = lv_btn_create(scr);
    lv_obj_set_size(scan_btn, 66, 36);
    lv_obj_align(scan_btn, LV_ALIGN_TOP_LEFT, 12, 134);
    lv_obj_set_style_bg_color(scan_btn, lv_color_hex(0x1565C0), 0);
    lv_obj_set_style_text_color(scan_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *scan_label = lv_label_create(scan_btn);
    lv_label_set_text(scan_label, "Scan");
    lv_obj_center(scan_label);
    lv_obj_add_event_cb(scan_btn, scan_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_btn = lv_btn_create(scr);
    lv_obj_set_size(back_btn, 66, 36);
    lv_obj_align(back_btn, LV_ALIGN_TOP_MID, 0, 134);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(back_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, back_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *connect_btn = lv_btn_create(scr);
    lv_obj_set_size(connect_btn, 66, 36);
    lv_obj_align(connect_btn, LV_ALIGN_TOP_RIGHT, -12, 134);
    lv_obj_set_style_bg_color(connect_btn, lv_color_hex(0x2E7D32), 0);
    lv_obj_set_style_text_color(connect_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *connect_label = lv_label_create(connect_btn);
    lv_label_set_text(connect_label, "Connect");
    lv_obj_center(connect_label);
    lv_obj_add_event_cb(connect_btn, connect_click_cb, LV_EVENT_CLICKED, NULL);

    /* Scan status line (shown above the AP list while scanning) */
    s_scan_label = lv_label_create(scr);
    lv_label_set_text(s_scan_label, "");
    lv_obj_set_style_text_color(s_scan_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(s_scan_label, LV_ALIGN_TOP_MID, 0, 172);
    lv_obj_add_flag(s_scan_label, LV_OBJ_FLAG_HIDDEN);

    /* AP list (replaces the keyboard while scanning / listing) */
    s_ap_list = lv_list_create(scr);
    lv_obj_set_size(s_ap_list, 240, 130);
    lv_obj_align(s_ap_list, LV_ALIGN_TOP_MID, 0, 190);
    lv_obj_set_style_bg_color(s_ap_list, lv_color_hex(0x1E242B), 0);
    lv_obj_set_style_border_color(s_ap_list, lv_color_hex(0x3A444E), 0);
    lv_obj_set_style_pad_all(s_ap_list, 4, 0);
    lv_obj_set_style_pad_row(s_ap_list, 3, 0);
    lv_obj_add_flag(s_ap_list, LV_OBJ_FLAG_HIDDEN);

    /* On-screen keyboard (bottom); follows the focused input box.
     * Start bound to the SSID field. */
    s_kb = lv_keyboard_create(scr);
    lv_obj_set_size(s_kb, 240, 150);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_kb, ssid_ta);
    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_CANCEL, NULL);

    /* Poll for scan completion from the LVGL thread */
    lv_timer_create(scan_poll_cb, 200, NULL);

    return scr;
}
