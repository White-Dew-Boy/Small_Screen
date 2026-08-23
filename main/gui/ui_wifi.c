#include "ui_wifi.h"
#include "lvgl.h"
#include "wifi_manager.h"

/* Widgets refreshed by the LVGL timer callback. */
static lv_obj_t *state_label;
static lv_obj_t *ssid_label;
static lv_obj_t *ip_label;
static lv_obj_t *rssi_label;
static lv_obj_t *retry_label;
static lv_obj_t *reason_label;

/* Callbacks to open the saved/nearby WiFi pages (set by main.c, invoked
 * from the LVGL thread). */
static void (*s_saved_cb)(void) = NULL;
static void (*s_nearby_cb)(void) = NULL;

void ui_wifi_set_saved_cb(void (*cb)(void))
{
    s_saved_cb = cb;
}

void ui_wifi_set_nearby_cb(void (*cb)(void))
{
    s_nearby_cb = cb;
}

static void saved_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_saved_cb != NULL) {
        s_saved_cb();
    }
}

static void nearby_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_nearby_cb != NULL) {
        s_nearby_cb();
    }
}

/* Convert RSSI [dBm] to a signal strength bar string.
 * >= -50 excellent, >= -65 good, >= -75 fair, else poor. */
static const char *rssi_to_bar(int8_t rssi)
{
    if (rssi >= -50) return "****";
    if (rssi >= -65) return "*** ";
    if (rssi >= -75) return "**  ";
    return "*   ";
}

/* Human-readable text for a WiFi disconnect reason. */
static const char *reason_to_str(int16_t reason)
{
    switch (reason) {
    case 201: return "AP not found (2.4GHz?)";
    case 202: return "Wrong password";
    case 203: return "Association failed";
    case 204: return "Handshake timeout";
    case 205: return "Connection lost";
    case 15:  return "4-way handshake timeout";
    case 210: return "No compatible security";
    case 211: return "No AP in auth mode";
    case 212: return "No AP in RSSI range";
    default:  return "Unknown";
    }
}

/**
 * @brief LVGL timer callback: refresh the WiFi status page.
 *        Runs inside lv_timer_handler(), reads wifi_manager snapshot
 *        (thread-safe, guarded by an internal mutex).
 */
static void wifi_display_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    wifi_info_t info;
    wifi_manager_get_info(&info);

    switch (info.state) {
    case WIFI_STATE_CONNECTED:
        lv_label_set_text(state_label, "Status: Connected");
        lv_obj_set_style_text_color(state_label, lv_color_hex(0x4CAF50), 0);
        break;
    case WIFI_STATE_CONNECTING:
        lv_label_set_text(state_label, "Status: Connecting...");
        lv_obj_set_style_text_color(state_label, lv_color_hex(0xFFC107), 0);
        break;
    case WIFI_STATE_DISCONNECTED:
        lv_label_set_text(state_label, "Status: Disconnected");
        lv_obj_set_style_text_color(state_label, lv_color_hex(0xF44336), 0);
        break;
    default:
        lv_label_set_text(state_label, "Status: Idle");
        lv_obj_set_style_text_color(state_label, lv_color_hex(0x9E9E9E), 0);
        break;
    }

    if (info.state == WIFI_STATE_CONNECTED) {
        lv_label_set_text_fmt(ssid_label, "SSID: %s", info.ssid);
        lv_label_set_text_fmt(ip_label, "IP: %s", info.ip);
        lv_label_set_text_fmt(rssi_label, "RSSI: %d dBm %s", info.rssi,
                              rssi_to_bar(info.rssi));
        lv_label_set_text(reason_label, "");
    } else {
        lv_label_set_text(ssid_label, "SSID: --");
        lv_label_set_text(ip_label, "IP: --");
        lv_label_set_text(rssi_label, "RSSI: --");
        if (info.last_reason != 0) {
            lv_label_set_text_fmt(reason_label, "Reason %d: %s",
                                  (int)info.last_reason,
                                  reason_to_str(info.last_reason));
        } else {
            lv_label_set_text(reason_label, "");
        }
    }

    lv_label_set_text_fmt(retry_label, "Auto reconnect: %lu time(s)",
                          (unsigned long)info.reconnect_cnt);
}

/**
 * @brief Build the WiFi status page on its own screen.
 */
lv_obj_t *ui_wifi_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "WiFi Status");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    /* Status line (colored by state) */
    state_label = lv_label_create(scr);
    lv_label_set_text(state_label, "Status: Idle");
    lv_obj_set_style_text_color(state_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(state_label, LV_ALIGN_CENTER, 0, -60);

    ssid_label = lv_label_create(scr);
    lv_label_set_text(ssid_label, "SSID: --");
    lv_obj_set_style_text_color(ssid_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(ssid_label, LV_ALIGN_CENTER, 0, -25);

    ip_label = lv_label_create(scr);
    lv_label_set_text(ip_label, "IP: --");
    lv_obj_set_style_text_color(ip_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(ip_label, LV_ALIGN_CENTER, 0, 0);

    rssi_label = lv_label_create(scr);
    lv_label_set_text(rssi_label, "RSSI: --");
    lv_obj_set_style_text_color(rssi_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(rssi_label, LV_ALIGN_CENTER, 0, 25);

    retry_label = lv_label_create(scr);
    lv_label_set_text(retry_label, "Auto reconnect: 0 time(s)");
    lv_obj_set_style_text_color(retry_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(retry_label, LV_ALIGN_CENTER, 0, 55);

    /* Disconnect reason (shown when not connected, e.g. "Reason 201: AP not found") */
    reason_label = lv_label_create(scr);
    lv_label_set_text(reason_label, "");
    lv_obj_set_style_text_color(reason_label, lv_color_hex(0xFFB74D), 0);
    lv_obj_align(reason_label, LV_ALIGN_CENTER, 0, 78);

    /* Bottom buttons: Saved WiFi | Nearby WiFi */
    lv_obj_t *saved_btn = lv_btn_create(scr);
    lv_obj_set_size(saved_btn, 102, 38);
    lv_obj_align(saved_btn, LV_ALIGN_BOTTOM_LEFT, 12, -12);
    lv_obj_set_style_bg_color(saved_btn, lv_color_hex(0x1565C0), 0);
    lv_obj_set_style_text_color(saved_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *saved_label = lv_label_create(saved_btn);
    lv_label_set_text(saved_label, "Saved WiFi");
    lv_obj_center(saved_label);
    lv_obj_add_event_cb(saved_btn, saved_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *nearby_btn = lv_btn_create(scr);
    lv_obj_set_size(nearby_btn, 102, 38);
    lv_obj_align(nearby_btn, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
    lv_obj_set_style_bg_color(nearby_btn, lv_color_hex(0x2E7D32), 0);
    lv_obj_set_style_text_color(nearby_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *nearby_label = lv_label_create(nearby_btn);
    lv_label_set_text(nearby_label, "Nearby WiFi");
    lv_obj_center(nearby_label);
    lv_obj_add_event_cb(nearby_btn, nearby_click_cb, LV_EVENT_CLICKED, NULL);

    /* Refresh every 500 ms from the LVGL thread */
    lv_timer_create(wifi_display_timer_cb, 500, NULL);

    return scr;
}
