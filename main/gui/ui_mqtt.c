#include "ui_mqtt.h"
#include "lvgl.h"
#include "mqtt_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Widgets refreshed by the LVGL timer callback. */
static lv_obj_t *state_label;
static lv_obj_t *server_label;
static lv_obj_t *port_label;
static lv_obj_t *device_label;
static lv_obj_t *path_label;
static lv_obj_t *user_label;
static lv_obj_t *connect_btn;
static lv_obj_t *connect_btn_label;

/* Callbacks to open sub-pages (set by main.c, invoked from LVGL thread) */
static void (*s_interval_cb)(void) = NULL;
static void (*s_history_cb)(void) = NULL;
static void (*s_config_cb)(void) = NULL;

void ui_mqtt_set_interval_cb(void (*cb)(void))
{
    s_interval_cb = cb;
}

void ui_mqtt_set_history_cb(void (*cb)(void))
{
    s_history_cb = cb;
}

void ui_mqtt_set_config_cb(void (*cb)(void))
{
    s_config_cb = cb;
}

/* Connect / Disconnect toggle */
static void connect_click_cb(lv_event_t *e)
{
    (void)e;
    if (mqtt_manager_is_connected()) {
        mqtt_manager_disconnect();
    } else {
        mqtt_manager_connect();
    }
}

static void interval_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_interval_cb != NULL) {
        s_interval_cb();
    }
}

static void history_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_history_cb != NULL) {
        s_history_cb();
    }
}

static void config_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_config_cb != NULL) {
        s_config_cb();
    }
}

/**
 * @brief Parse "scheme://host:port/path" into parts.
 *        scheme keeps the "://" suffix, path keeps the leading "/".
 */
static void parse_uri(const char *uri, char *scheme, size_t scheme_cap,
                      char *host, size_t host_cap, char *path, size_t path_cap,
                      int *port)
{
    const char *p = uri;
    const char *sep = strstr(uri, "://");
    if (sep != NULL) {
        size_t n = (size_t)(sep - uri) + 3; /* scheme + "://" */
        if (n >= scheme_cap) {
            n = scheme_cap - 1;
        }
        memcpy(scheme, uri, n);
        scheme[n] = '\0';
        p = sep + 3;
    } else {
        scheme[0] = '\0';
    }

    char rest[128];
    strlcpy(rest, p, sizeof(rest));

    char *colon = strchr(rest, ':');
    char *slash = strchr(rest, '/');

    if (colon != NULL && (slash == NULL || colon < slash)) {
        *colon = '\0';
        if (slash != NULL) {
            strlcpy(path, slash, path_cap); /* keep leading '/' */
        } else {
            path[0] = '\0';
        }
        strlcpy(host, rest, host_cap);
        *port = atoi(colon + 1);
    } else {
        if (slash != NULL) {
            strlcpy(path, slash, path_cap);
        } else {
            path[0] = '\0';
        }
        strlcpy(host, rest, host_cap);
        *port = 0;
    }
}

/* Refresh the broker info lines from the current runtime config. Called
 * periodically so changes made on the Config page show up immediately
 * (the labels are not one-time snapshots). */
static void update_broker_info(void)
{
    mqtt_cfg_t cfg;
    mqtt_manager_get_cfg(&cfg);

    char scheme[16], host[64], path[32];
    int port = 0;
    parse_uri(cfg.uri, scheme, sizeof(scheme),
              host, sizeof(host), path, sizeof(path), &port);

    lv_label_set_text_fmt(server_label, "Server: %s%s", scheme, host);

    if (port > 0) {
        lv_label_set_text_fmt(port_label, "Port: %d", port);
    } else {
        lv_label_set_text(port_label, "Port: --");
    }

    lv_label_set_text_fmt(path_label, "Path: %s",
                          path[0] ? path : "/");

    lv_label_set_text_fmt(user_label, "Username: %s",
                          cfg.username[0] ? cfg.username : "(none)");
}

/**
 * @brief LVGL timer callback: refresh the MQTT status page.
 *        Runs inside lv_timer_handler(), safe to touch LVGL objects.
 */
static void mqtt_display_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    update_broker_info();

    bool connected = mqtt_manager_is_connected();
    if (connected) {
        lv_label_set_text(state_label, "MQTT: Connected");
        lv_obj_set_style_text_color(state_label, lv_color_hex(0x4CAF50), 0);
        lv_label_set_text(connect_btn_label, "Disconnect");
    } else {
        lv_label_set_text(state_label, "MQTT: Disconnected");
        lv_obj_set_style_text_color(state_label, lv_color_hex(0xF44336), 0);
        lv_label_set_text(connect_btn_label, "Connect");
    }
}

/**
 * @brief Build the MQTT status page on its own screen.
 */
lv_obj_t *ui_mqtt_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "MQTT Status");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    /* Connection state (colored) */
    state_label = lv_label_create(scr);
    lv_label_set_text(state_label, "MQTT: Disconnected");
    lv_obj_set_style_text_color(state_label, lv_color_hex(0xF44336), 0);
    lv_obj_align(state_label, LV_ALIGN_TOP_MID, 0, 36);

    server_label = lv_label_create(scr);
    lv_label_set_text(server_label, "Server: --");
    lv_obj_set_style_text_color(server_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(server_label, LV_ALIGN_TOP_LEFT, 12, 62);

    port_label = lv_label_create(scr);
    lv_label_set_text(port_label, "Port: --");
    lv_obj_set_style_text_color(port_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(port_label, LV_ALIGN_TOP_LEFT, 12, 86);

    device_label = lv_label_create(scr);
    lv_label_set_text_fmt(device_label, "Device ID: %s",
                          mqtt_manager_get_device_id());
    lv_obj_set_style_text_color(device_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(device_label, LV_ALIGN_TOP_LEFT, 12, 110);

    path_label = lv_label_create(scr);
    lv_label_set_text(path_label, "Path: /");
    lv_obj_set_style_text_color(path_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(path_label, LV_ALIGN_TOP_LEFT, 12, 134);

    user_label = lv_label_create(scr);
    lv_label_set_text(user_label, "Username: --");
    lv_obj_set_style_text_color(user_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(user_label, LV_ALIGN_TOP_LEFT, 12, 158);

    /* Fill the broker info lines with the current config */
    update_broker_info();

    /* Bottom action buttons (2 x 2):
     *   Connect/Disconnect | Interval
     *   History            | Config      */
    connect_btn = lv_btn_create(scr);
    lv_obj_set_size(connect_btn, 102, 38);
    lv_obj_align(connect_btn, LV_ALIGN_BOTTOM_LEFT, 12, -10);
    lv_obj_set_style_bg_color(connect_btn, lv_color_hex(0x1565C0), 0);
    lv_obj_set_style_text_color(connect_btn, lv_color_hex(0xFFFFFF), 0);
    connect_btn_label = lv_label_create(connect_btn);
    lv_label_set_text(connect_btn_label, "Connect");
    lv_obj_center(connect_btn_label);
    lv_obj_add_event_cb(connect_btn, connect_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *interval_btn = lv_btn_create(scr);
    lv_obj_set_size(interval_btn, 102, 38);
    lv_obj_align(interval_btn, LV_ALIGN_BOTTOM_RIGHT, -12, -10);
    lv_obj_set_style_bg_color(interval_btn, lv_color_hex(0x2E7D32), 0);
    lv_obj_set_style_text_color(interval_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *interval_label = lv_label_create(interval_btn);
    lv_label_set_text(interval_label, "Interval");
    lv_obj_center(interval_label);
    lv_obj_add_event_cb(interval_btn, interval_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *history_btn = lv_btn_create(scr);
    lv_obj_set_size(history_btn, 102, 38);
    lv_obj_align(history_btn, LV_ALIGN_BOTTOM_LEFT, 12, -56);
    lv_obj_set_style_bg_color(history_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(history_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *history_label = lv_label_create(history_btn);
    lv_label_set_text(history_label, "History");
    lv_obj_center(history_label);
    lv_obj_add_event_cb(history_btn, history_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *config_btn = lv_btn_create(scr);
    lv_obj_set_size(config_btn, 102, 38);
    lv_obj_align(config_btn, LV_ALIGN_BOTTOM_RIGHT, -12, -56);
    lv_obj_set_style_bg_color(config_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(config_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *config_label = lv_label_create(config_btn);
    lv_label_set_text(config_label, "Config");
    lv_obj_center(config_label);
    lv_obj_add_event_cb(config_btn, config_click_cb, LV_EVENT_CLICKED, NULL);

    /* Refresh every 500 ms from the LVGL thread */
    lv_timer_create(mqtt_display_timer_cb, 500, NULL);

    return scr;
}
