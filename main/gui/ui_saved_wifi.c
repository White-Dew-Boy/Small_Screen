#include "ui_saved_wifi.h"
#include "lvgl.h"
#include "wifi_manager.h"

#include <string.h>

/* Widgets */
static lv_obj_t *s_list;
static lv_obj_t *s_msg_label;
static lv_obj_t *s_back_btn;
static lv_obj_t *s_connect_btn;
static lv_obj_t *s_delete_btn;

/* Callback to leave the page (set by main.c, invoked from the LVGL thread) */
static void (*s_back_cb)(void) = NULL;

/* Currently selected network (row). -1 / NULL = nothing selected, so the
 * Connect / Delete buttons stay disabled. */
static int s_sel_idx = -1;
static lv_obj_t *s_sel_btn = NULL;
static char s_sel_ssid[33];

/* A connect attempt started from this page is in progress */
static bool s_connecting = false;

void ui_saved_wifi_set_back_cb(void (*cb)(void))
{
    s_back_cb = cb;
}

/* Enable / disable the bottom Connect + Delete buttons. Disabled buttons
 * are grayed out and do not react to touches. */
static void set_actions_enabled(bool en)
{
    lv_obj_t *c_lbl = lv_obj_get_child(s_connect_btn, 0);
    lv_obj_t *d_lbl = lv_obj_get_child(s_delete_btn, 0);

    if (en) {
        lv_obj_clear_state(s_connect_btn, LV_STATE_DISABLED);
        lv_obj_clear_state(s_delete_btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(s_connect_btn, lv_color_hex(0x2E7D32), 0);
        lv_obj_set_style_bg_color(s_delete_btn, lv_color_hex(0xC62828), 0);
        if (c_lbl != NULL) {
            lv_obj_set_style_text_color(c_lbl, lv_color_hex(0xFFFFFF), 0);
        }
        if (d_lbl != NULL) {
            lv_obj_set_style_text_color(d_lbl, lv_color_hex(0xFFFFFF), 0);
        }
    } else {
        lv_obj_add_state(s_connect_btn, LV_STATE_DISABLED);
        lv_obj_add_state(s_delete_btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(s_connect_btn, lv_color_hex(0x333B44), 0);
        lv_obj_set_style_bg_color(s_delete_btn, lv_color_hex(0x333B44), 0);
        if (c_lbl != NULL) {
            lv_obj_set_style_text_color(c_lbl, lv_color_hex(0x8A94A0), 0);
        }
        if (d_lbl != NULL) {
            lv_obj_set_style_text_color(d_lbl, lv_color_hex(0x8A94A0), 0);
        }
    }
}

/* Select (highlight) the tapped network row. */
static void select_row(lv_obj_t *btn, int idx)
{
    /* Un-highlight the previous row */
    if (s_sel_btn != NULL && s_sel_btn != btn) {
        lv_obj_set_style_bg_color(s_sel_btn, lv_color_hex(0x1E242B), 0);
    }
    s_sel_btn = btn;
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2F5D8A), 0);

    const wifi_cred_t *cred = wifi_manager_cred_get(idx);
    if (cred != NULL) {
        strlcpy(s_sel_ssid, cred->ssid, sizeof(s_sel_ssid));
    }

    s_sel_idx = idx;
    s_connecting = false;
    lv_label_set_text(s_msg_label, "");
    set_actions_enabled(true);
}

static void saved_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= wifi_manager_cred_count()) {
        return;
    }
    /* Tapping a saved name only SELECTS it (no connection is started);
     * the user then presses Connect explicitly. */
    select_row(lv_event_get_current_target(e), idx);
}

static void connect_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_sel_idx < 0) {
        return;
    }
    if (s_connecting) {
        return; /* a connect attempt is already running */
    }

    esp_err_t ret = wifi_manager_connect_saved(s_sel_idx);
    if (ret != ESP_OK) {
        lv_label_set_text_fmt(s_msg_label, "Error: %s", esp_err_to_name(ret));
        lv_obj_set_style_text_color(s_msg_label, lv_color_hex(0xF44336), 0);
        return;
    }

    s_connecting = true;
    lv_label_set_text_fmt(s_msg_label, "Connecting to \"%s\"...", s_sel_ssid);
    lv_obj_set_style_text_color(s_msg_label, lv_color_hex(0xFFC107), 0);
}

/* Delete the selected network. */
static void do_delete_selected(void)
{
    if (s_sel_idx < 0) {
        return;
    }
    char removed[33];
    strlcpy(removed, s_sel_ssid, sizeof(removed));

    esp_err_t ret = wifi_manager_forget(s_sel_idx);
    ui_saved_wifi_refresh(); /* rebuild (also clears the selection) */

    if (ret == ESP_OK) {
        lv_label_set_text_fmt(s_msg_label, "Removed \"%s\"", removed);
        lv_obj_set_style_text_color(s_msg_label, lv_color_hex(0x4CAF50), 0);
    } else {
        lv_label_set_text_fmt(s_msg_label, "Remove failed: %s",
                              esp_err_to_name(ret));
        lv_obj_set_style_text_color(s_msg_label, lv_color_hex(0xF44336), 0);
    }
}

/* Message-box event: button 0 = Delete, 1 = Cancel. */
static void delete_mbox_cb(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_current_target(e);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        if (lv_msgbox_get_active_btn(mbox) == 0) {
            do_delete_selected();
        }
        lv_msgbox_close(mbox);
    }
}

static void delete_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_sel_idx < 0) {
        return;
    }

    static const char *btns[] = { "Delete", "Cancel", "" };
    lv_obj_t *mbox = lv_msgbox_create(lv_scr_act(), "Forget WiFi",
                                      s_sel_ssid, btns, false);
    /* Dark styling to match the app */
    lv_obj_set_style_bg_color(mbox, lv_color_hex(0x1C232B), 0);
    lv_obj_set_style_text_color(mbox, lv_color_hex(0xE8ECF0), 0);
    lv_obj_add_event_cb(mbox, delete_mbox_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(mbox);
}

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    s_connecting = false;
    if (s_back_cb != NULL) {
        s_back_cb();
    }
}

/* Poll the connection result while connecting. */
static void poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_connecting) {
        return;
    }

    wifi_info_t info;
    wifi_manager_get_info(&info);

    if (info.state == WIFI_STATE_CONNECTED) {
        s_connecting = false;
        lv_label_set_text_fmt(s_msg_label, "Connected to \"%s\"", info.ssid);
        lv_obj_set_style_text_color(s_msg_label, lv_color_hex(0x4CAF50), 0);
    } else if (info.last_reason != 0) {
        /* The link failed; wifi_manager keeps retrying in the background,
         * but we surface the reason now so the user can delete the
         * network (e.g. wrong password) instead of waiting. */
        s_connecting = false;
        lv_label_set_text_fmt(s_msg_label, "Failed: %s",
                              wifi_manager_reason_to_str(info.last_reason));
        lv_obj_set_style_text_color(s_msg_label, lv_color_hex(0xF44336), 0);
    }
}

/* Rebuild the list from the saved credential list. */
static void build_saved_list(void)
{
    lv_obj_clean(s_list);
    s_sel_btn = NULL; /* old row objects are gone */
    s_sel_idx = -1;
    set_actions_enabled(false);

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
    s_connecting = false;
    lv_label_set_text(s_msg_label, "");
    lv_obj_set_style_text_color(s_msg_label, lv_color_hex(0xFFC107), 0);
    build_saved_list();
}

lv_obj_t *ui_saved_wifi_create(void)
{
    /* Landscape 320x240, like the WiFi status page it belongs to. */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 240);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Saved WiFi");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    /* Message / status line */
    s_msg_label = lv_label_create(scr);
    lv_label_set_text(s_msg_label, "");
    lv_obj_set_style_text_color(s_msg_label, lv_color_hex(0xFFC107), 0);
    lv_obj_set_style_text_align(s_msg_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_msg_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_msg_label, 296);
    lv_obj_align(s_msg_label, LV_ALIGN_TOP_MID, 0, 30);

    /* Saved network list */
    s_list = lv_list_create(scr);
    lv_obj_set_size(s_list, 296, 140);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x1E242B), 0);
    lv_obj_set_style_border_color(s_list, lv_color_hex(0x3A444E), 0);
    lv_obj_set_style_pad_all(s_list, 4, 0);
    lv_obj_set_style_pad_row(s_list, 3, 0);

    /* Bottom action row: Back | Connect | Delete.
     * Connect / Delete are grayed out until a network is selected. */
    const lv_coord_t y = 196; /* list ends at 192 */

    s_back_btn = lv_btn_create(scr);
    lv_obj_set_size(s_back_btn, 84, 36);
    lv_obj_set_pos(s_back_btn, 12, y);
    lv_obj_set_style_bg_color(s_back_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(s_back_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *back_label = lv_label_create(s_back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(s_back_btn, back_click_cb, LV_EVENT_CLICKED, NULL);

    s_connect_btn = lv_btn_create(scr);
    lv_obj_set_size(s_connect_btn, 100, 36);
    lv_obj_set_pos(s_connect_btn, 104, y);
    lv_obj_set_style_bg_color(s_connect_btn, lv_color_hex(0x333B44), 0);
    lv_obj_set_style_text_color(s_connect_btn, lv_color_hex(0x8A94A0), 0);
    lv_obj_t *connect_label = lv_label_create(s_connect_btn);
    lv_label_set_text(connect_label, "Connect");
    lv_obj_center(connect_label);
    lv_obj_add_event_cb(s_connect_btn, connect_click_cb, LV_EVENT_CLICKED, NULL);

    s_delete_btn = lv_btn_create(scr);
    lv_obj_set_size(s_delete_btn, 96, 36);
    lv_obj_set_pos(s_delete_btn, 212, y);
    lv_obj_set_style_bg_color(s_delete_btn, lv_color_hex(0x333B44), 0);
    lv_obj_set_style_text_color(s_delete_btn, lv_color_hex(0x8A94A0), 0);
    lv_obj_t *delete_label = lv_label_create(s_delete_btn);
    lv_label_set_text(delete_label, "Delete");
    lv_obj_center(delete_label);
    lv_obj_add_event_cb(s_delete_btn, delete_click_cb, LV_EVENT_CLICKED, NULL);

    /* Start with nothing selected: gray out Connect / Delete */
    set_actions_enabled(false);

    build_saved_list();

    /* Poll connect results every 500 ms */
    lv_timer_create(poll_timer_cb, 500, NULL);

    return scr;
}
