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
static lv_obj_t *s_radio_btn;
static lv_obj_t *s_radio_label;
static lv_obj_t *s_nearby_btn;
/* Last radio state drawn: the toggle and the Nearby WiFi button are only
 * touched when it changes, so the page does not redraw for nothing. */
static bool s_radio_drawn = true;

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

/* Toggle the WiFi radio (RF) itself, not just the link:
 * off = esp_wifi_stop() (RF powered down, scanning disabled),
 * on  = esp_wifi_start() and the manager reconnects with the saved
 *       credentials. */
static void radio_click_cb(lv_event_t *e)
{
    (void)e;

    const bool turn_on = !wifi_manager_radio_is_on();
    const esp_err_t ret = wifi_manager_set_radio(turn_on);
    if (ret != ESP_OK) {
        lv_label_set_text_fmt(reason_label, "Radio %s failed: %s",
                              turn_on ? "on" : "off", esp_err_to_name(ret));
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
    case WIFI_STATE_OFF:
        lv_label_set_text(state_label, "Status: Radio OFF");
        lv_obj_set_style_text_color(state_label, lv_color_hex(0x9E9E9E), 0);
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
            lv_label_set_text(reason_label,
                              wifi_manager_reason_to_str(info.last_reason));
        } else {
            lv_label_set_text(reason_label, "");
        }
    }

    lv_label_set_text_fmt(retry_label, "Auto reconnect: %lu time(s)",
                          (unsigned long)info.reconnect_cnt);

    /* Radio toggle label/colour and the Nearby WiFi button availability.
     * Scanning is disabled while the RF is off, so the button is greyed
     * out — only repainted when the radio state actually changes. */
    const bool radio_on = wifi_manager_radio_is_on();
    if (radio_on != s_radio_drawn) {
        s_radio_drawn = radio_on;
        lv_label_set_text(s_radio_label, radio_on ? "WiFi Off" : "WiFi On");
        lv_obj_set_style_bg_color(s_radio_btn,
                                  lv_color_hex(radio_on ? 0xC62828 : 0x2E7D32),
                                  0);
        if (radio_on) {
            lv_obj_clear_state(s_nearby_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_nearby_btn, LV_STATE_DISABLED);
        }
    }
}

/**
 * @brief Build the WiFi status page on its own screen.
 */
lv_obj_t *ui_wifi_create(void)
{
    /* Landscape 320x240, like the Home/PC-Perf pages: main.c rotates the
     * whole display to landscape before this screen is loaded. */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 240);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "WiFi Status");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    /* Status lines, evenly spaced below the title */
    state_label = lv_label_create(scr);
    lv_label_set_text(state_label, "Status: Idle");
    lv_obj_set_style_text_color(state_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(state_label, LV_ALIGN_TOP_MID, 0, 36);

    ssid_label = lv_label_create(scr);
    lv_label_set_text(ssid_label, "SSID: --");
    lv_obj_set_style_text_color(ssid_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(ssid_label, LV_ALIGN_TOP_MID, 0, 58);

    ip_label = lv_label_create(scr);
    lv_label_set_text(ip_label, "IP: --");
    lv_obj_set_style_text_color(ip_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(ip_label, LV_ALIGN_TOP_MID, 0, 80);

    rssi_label = lv_label_create(scr);
    lv_label_set_text(rssi_label, "RSSI: --");
    lv_obj_set_style_text_color(rssi_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(rssi_label, LV_ALIGN_TOP_MID, 0, 102);

    retry_label = lv_label_create(scr);
    lv_label_set_text(retry_label, "Auto reconnect: 0 time(s)");
    lv_obj_set_style_text_color(retry_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(retry_label, LV_ALIGN_TOP_MID, 0, 124);

    /* Disconnect reason (shown when not connected, e.g. "AP not found") */
    reason_label = lv_label_create(scr);
    lv_label_set_text(reason_label, "");
    lv_obj_set_style_text_color(reason_label, lv_color_hex(0xFFB74D), 0);
    lv_obj_align(reason_label, LV_ALIGN_TOP_MID, 0, 146);

    /* Bottom row: Saved WiFi | Disconnect | Nearby WiFi */
    lv_obj_t *saved_btn = lv_btn_create(scr);
    lv_obj_set_size(saved_btn, 93, 40);
    lv_obj_align(saved_btn, LV_ALIGN_TOP_LEFT, 12, 182);
    lv_obj_set_style_bg_color(saved_btn, lv_color_hex(0x1565C0), 0);
    lv_obj_set_style_text_color(saved_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *saved_label = lv_label_create(saved_btn);
    lv_label_set_text(saved_label, "Saved WiFi");
    lv_obj_center(saved_label);
    lv_obj_add_event_cb(saved_btn, saved_click_cb, LV_EVENT_CLICKED, NULL);

    /* Radio (RF) toggle (red = tap to switch WiFi off) */
    s_radio_btn = lv_btn_create(scr);
    lv_obj_set_size(s_radio_btn, 93, 40);
    lv_obj_align(s_radio_btn, LV_ALIGN_TOP_LEFT, 113, 182);
    lv_obj_set_style_bg_color(s_radio_btn, lv_color_hex(0xC62828), 0);
    lv_obj_set_style_text_color(s_radio_btn, lv_color_hex(0xFFFFFF), 0);
    s_radio_label = lv_label_create(s_radio_btn);
    lv_label_set_text(s_radio_label, "WiFi Off");
    lv_obj_center(s_radio_label);
    lv_obj_add_event_cb(s_radio_btn, radio_click_cb, LV_EVENT_CLICKED, NULL);

    /* Nearby WiFi: scanning needs the radio, so it is disabled while off */
    s_nearby_btn = lv_btn_create(scr);
    lv_obj_set_size(s_nearby_btn, 93, 40);
    lv_obj_align(s_nearby_btn, LV_ALIGN_TOP_LEFT, 214, 182);
    lv_obj_set_style_bg_color(s_nearby_btn, lv_color_hex(0x2E7D32), 0);
    lv_obj_set_style_text_color(s_nearby_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(s_nearby_btn, lv_color_hex(0x2A323A),
                              LV_STATE_DISABLED);
    lv_obj_set_style_text_color(s_nearby_btn, lv_color_hex(0x6E767E),
                                LV_STATE_DISABLED);
    lv_obj_t *nearby_label = lv_label_create(s_nearby_btn);
    lv_label_set_text(nearby_label, "Nearby WiFi");
    lv_obj_center(nearby_label);
    lv_obj_add_event_cb(s_nearby_btn, nearby_click_cb, LV_EVENT_CLICKED, NULL);

    /* Refresh every 500 ms from the LVGL thread */
    lv_timer_create(wifi_display_timer_cb, 500, NULL);

    return scr;
}
