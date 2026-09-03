#include "ui_mqtt_config.h"
#include "lvgl.h"
#include "mqtt_manager.h"
#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ui_mqtt_config";

/* Widgets */
static lv_obj_t *s_field_list;   /* scrollable container with the fields */
static lv_obj_t *s_pass_ta;
static lv_obj_t *s_show_btn;     /* password visibility toggle */
static lv_obj_t *s_kb;
static lv_obj_t *s_msg_label;

/* Field textareas (order matters for tab-like focus traversal) */
static lv_obj_t *s_scheme_ta;
static lv_obj_t *s_host_ta;
static lv_obj_t *s_port_ta;
static lv_obj_t *s_path_ta;
static lv_obj_t *s_user_ta;
static lv_obj_t *s_client_ta;
static lv_obj_t *s_keepalive_ta;

/* Callback to leave the page (set by main.c, invoked from the LVGL thread) */
static void (*s_back_cb)(void) = NULL;

void ui_mqtt_config_set_back_cb(void (*cb)(void))
{
    s_back_cb = cb;
}

static void do_back(void)
{
    if (s_back_cb != NULL) {
        s_back_cb();
    }
}

/* Toggle password visibility; button text follows state. */
static void set_pass_visible(bool show)
{
    lv_textarea_set_password_mode(s_pass_ta, !show);
    lv_obj_t *lbl = lv_obj_get_child(s_show_btn, 0);
    if (lbl != NULL) {
        lv_label_set_text(lbl, show ? "HD" : "SH");
    }
}

static void show_click_cb(lv_event_t *e)
{
    (void)e;
    bool hidden = lv_textarea_get_password_mode(s_pass_ta);
    set_pass_visible(hidden);
}

/* Keyboard follows the focused input box. */
static void ta_focus_cb(lv_event_t *e)
{
    if (s_kb == NULL) {
        return;
    }
    lv_obj_t *ta = lv_event_get_target(e);
    lv_keyboard_set_textarea(s_kb, ta);
}

static lv_obj_t *make_field(lv_obj_t *parent, const char *placeholder,
                            uint32_t max_len, bool password)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_width(ta, 280);
    lv_obj_set_height(ta, 30);
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

/* Parse the current config into the field textareas. */
static void load_fields(void)
{
    mqtt_cfg_t cfg;
    mqtt_manager_get_cfg(&cfg);

    char scheme[16], host[64], path[32];
    int port = 0;
    const char *uri = cfg.uri;
    const char *sep = strstr(uri, "://");
    if (sep != NULL) {
        size_t n = (size_t)(sep - uri);
        if (n >= sizeof(scheme)) {
            n = sizeof(scheme) - 1;
        }
        memcpy(scheme, uri, n);
        scheme[n] = '\0';
        uri = sep + 3;
    } else {
        scheme[0] = '\0';
    }
    host[0] = '\0';
    path[0] = '\0';
    strlcpy(host, uri, sizeof(host));
    char *colon = strchr(host, ':');
    char *slash = colon ? strchr(colon, '/') : strchr(host, '/');
    if (colon != NULL && (slash == NULL || colon < slash)) {
        *colon = '\0';
        if (slash != NULL) {
            strlcpy(path, slash, sizeof(path));
        }
        port = atoi(colon + 1);
    } else if (slash != NULL) {
        *slash = '\0';
        strlcpy(path, slash, sizeof(path));
    }

    lv_textarea_set_text(s_scheme_ta, scheme);
    lv_textarea_set_text(s_host_ta, host);
    if (port > 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", port);
        lv_textarea_set_text(s_port_ta, buf);
    } else {
        lv_textarea_set_text(s_port_ta, "");
    }
    lv_textarea_set_text(s_path_ta, path);
    lv_textarea_set_text(s_user_ta, cfg.username);
    lv_textarea_set_text(s_pass_ta, cfg.password);
    lv_textarea_set_text(s_client_ta, cfg.client_id);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", cfg.keepalive_s);
    lv_textarea_set_text(s_keepalive_ta, buf);
}

/* Collect fields, validate, build the URI and apply. */
static void save_click_cb(lv_event_t *e)
{
    (void)e;
    const char *scheme = lv_textarea_get_text(s_scheme_ta);
    const char *host = lv_textarea_get_text(s_host_ta);
    const char *port = lv_textarea_get_text(s_port_ta);
    const char *path = lv_textarea_get_text(s_path_ta);

    if (scheme[0] == '\0') {
        lv_label_set_text(s_msg_label, "Protocol required");
        return;
    }
    if (host[0] == '\0') {
        lv_label_set_text(s_msg_label, "Address required");
        return;
    }
    if (port[0] != '\0' && atoi(port) <= 0) {
        lv_label_set_text(s_msg_label, "Invalid port");
        return;
    }

    char uri[160];
    snprintf(uri, sizeof(uri), "%s://%s", scheme, host);
    if (port[0] != '\0') {
        snprintf(uri + strlen(uri), sizeof(uri) - strlen(uri), ":%s", port);
    }
    if (path[0] != '\0') {
        snprintf(uri + strlen(uri), sizeof(uri) - strlen(uri), "%s", path);
    } else if (strncmp(scheme, "ws", 2) == 0) {
        snprintf(uri + strlen(uri), sizeof(uri) - strlen(uri), "/");
    }

    mqtt_cfg_t cfg = {0};
    strlcpy(cfg.uri, uri, sizeof(cfg.uri));
    strlcpy(cfg.username, lv_textarea_get_text(s_user_ta), sizeof(cfg.username));
    strlcpy(cfg.password, lv_textarea_get_text(s_pass_ta), sizeof(cfg.password));
    strlcpy(cfg.client_id, lv_textarea_get_text(s_client_ta), sizeof(cfg.client_id));
    cfg.keepalive_s = atoi(lv_textarea_get_text(s_keepalive_ta));

    ESP_LOGI(TAG, "Applying MQTT config: %s", uri);
    esp_err_t ret = mqtt_manager_set_cfg(&cfg);
    if (ret != ESP_OK) {
        lv_label_set_text_fmt(s_msg_label, "Error: %s", esp_err_to_name(ret));
        return;
    }

    lv_label_set_text(s_msg_label, "Saved, reconnecting...");
    lv_obj_set_style_text_color(s_msg_label, lv_color_hex(0x4CAF50), 0);
    do_back();
}

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    do_back();
}

/* Keyboard: Enter = save, X = back. */
static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        save_click_cb(e);
    } else if (code == LV_EVENT_CANCEL) {
        do_back();
    }
}

lv_obj_t *ui_mqtt_config_create(void)
{
    /* Landscape 320x240, like the MQTT status page it belongs to. */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 240);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "MQTT Config");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    /* Message line (errors / "Saved") below the title */
    s_msg_label = lv_label_create(scr);
    lv_label_set_text(s_msg_label, "");
    lv_obj_set_style_text_color(s_msg_label, lv_color_hex(0xF44336), 0);
    lv_obj_align(s_msg_label, LV_ALIGN_TOP_MID, 0, 22);

    /* Top action row: Back | SH/HD | Save */
    lv_obj_t *back_btn = lv_btn_create(scr);
    lv_obj_set_size(back_btn, 96, 28);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 12, 42);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(back_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, back_click_cb, LV_EVENT_CLICKED, NULL);

    s_show_btn = lv_btn_create(scr);
    lv_obj_set_size(s_show_btn, 96, 28);
    lv_obj_align(s_show_btn, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_set_style_bg_color(s_show_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(s_show_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *show_label = lv_label_create(s_show_btn);
    lv_label_set_text(show_label, "SH");
    lv_obj_center(show_label);
    lv_obj_add_event_cb(s_show_btn, show_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *save_btn = lv_btn_create(scr);
    lv_obj_set_size(save_btn, 96, 28);
    lv_obj_align(save_btn, LV_ALIGN_TOP_RIGHT, -12, 42);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x2E7D32), 0);
    lv_obj_set_style_text_color(save_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "Save");
    lv_obj_center(save_label);
    lv_obj_add_event_cb(save_btn, save_click_cb, LV_EVENT_CLICKED, NULL);

    /* Field list (scrollable; keyboard stays fixed at the bottom) */
    s_field_list = lv_obj_create(scr);
    lv_obj_set_size(s_field_list, 296, 62);
    lv_obj_set_pos(s_field_list, 12, 74);
    lv_obj_set_style_bg_color(s_field_list, lv_color_hex(0x101418), 0);
    lv_obj_set_style_border_width(s_field_list, 0, 0);
    lv_obj_set_style_pad_all(s_field_list, 2, 0);
    lv_obj_set_style_pad_row(s_field_list, 2, 0);
    lv_obj_set_scroll_dir(s_field_list, LV_DIR_VER);
    /* Stack the fields vertically (a plain lv_obj has no layout by
     * default, so children would all pile up at the origin) */
    lv_obj_set_flex_flow(s_field_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_field_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    s_scheme_ta = make_field(s_field_list, "Protocol (wss/mqtts/mqtt/ws)", 16, false);
    s_host_ta = make_field(s_field_list, "Address", 100, false);
    s_port_ta = make_field(s_field_list, "Port", 8, false);
    s_path_ta = make_field(s_field_list, "Path (/mqtt)", 32, false);
    s_user_ta = make_field(s_field_list, "Username (empty = anonymous)", 32, false);
    s_pass_ta = make_field(s_field_list, "Password (empty = anonymous)", 63, true);
    s_client_ta = make_field(s_field_list, "Client ID (empty = auto)", 47, false);
    s_keepalive_ta = make_field(s_field_list, "Keepalive s (0 = default 120)", 8, false);

    /* On-screen keyboard (bottom), follows the focused field */
    s_kb = lv_keyboard_create(scr);
    lv_obj_set_size(s_kb, 320, 100);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_kb, s_scheme_ta);
    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_CANCEL, NULL);

    load_fields();

    return scr;
}
