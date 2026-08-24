#include "ui_home.h"
#include "lvgl.h"

/* Menu entries: name and accent color */
static const struct {
    const char *name;
    uint32_t color;
} s_entries[] = {
    { "Sensor",   0x1565C0 }, /* blue   */
    { "WiFi",     0x1565C0 },
    { "MQTT",     0x2E7D32 }, /* green  */
    { "LED",      0xE65100 }, /* orange */
    { "SD Card",  0x00695C }, /* teal   */
    { "System",   0x6A1B9A }, /* purple */
};

/* Callback to open a page (set by main.c, invoked from the LVGL thread) */
static void (*s_page_cb)(int page) = NULL;
static void (*s_deepsleep_cb)(void) = NULL;

/* Bottom hint label, reused to show the deep-sleep press count */
static lv_obj_t *s_hint_label;

/* Deep sleep needs 3 presses within a 3 s window (accident protection).
 * A one-shot LVGL timer implements the window: it is (re)started on each
 * press and, on expiry, restores the hint. */
#define DS_PRESS_REQUIRED 3
#define DS_PRESS_WINDOW_MS 3000
static int s_ds_count = 0;
static lv_timer_t *s_ds_timer = NULL;

void ui_home_set_page_cb(void (*cb)(int page))
{
    s_page_cb = cb;
}

void ui_home_set_deepsleep_cb(void (*cb)(void))
{
    s_deepsleep_cb = cb;
}

void ui_home_reset_hint(void)
{
    s_ds_count = 0;
    if (s_hint_label != NULL) {
        lv_label_set_text(s_hint_label, "KEY3: back to Home");
    }
}

static void entry_click_cb(lv_event_t *e)
{
    int page = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_page_cb != NULL) {
        s_page_cb(page);
    }
}

/* The 3 s press window expired without a third press: restore the hint. */
static void ds_timeout_cb(lv_timer_t *timer)
{
    (void)timer;
    ui_home_reset_hint();
    lv_timer_pause(s_ds_timer);
}

/* Deep sleep fires after 3 presses inside the window; each press updates
 * the hint and (re)starts the timeout. */
static void deepsleep_click_cb(lv_event_t *e)
{
    (void)e;
    s_ds_count++;

    if (s_ds_count < DS_PRESS_REQUIRED) {
        if (s_hint_label != NULL) {
            lv_label_set_text_fmt(s_hint_label, "Press %d more to sleep",
                                  DS_PRESS_REQUIRED - s_ds_count);
        }
        /* (Re)start the 3 s window */
        lv_timer_reset(s_ds_timer);
        lv_timer_resume(s_ds_timer);
        return;
    }

    /* Third press: go */
    lv_timer_pause(s_ds_timer);
    ui_home_reset_hint();
    if (s_deepsleep_cb != NULL) {
        s_deepsleep_cb();
    }
}

lv_obj_t *ui_home_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Home");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    /* 2x3 grid of page entries (same height as the Deep Sleep button) */
    for (int i = 0; i < 6; i++) {
        int col = i % 2;
        int row = i / 2;

        lv_obj_t *btn = lv_btn_create(scr);
        lv_obj_set_size(btn, 104, 40);
        lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 12 + col * 112, 56 + row * 48);
        lv_obj_set_style_bg_color(btn, lv_color_hex(s_entries[i].color), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_radius(btn, 8, 0);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, s_entries[i].name);
        lv_obj_center(lbl);

        lv_obj_add_event_cb(btn, entry_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }

    /* Deep sleep button (full width, below the 3rd grid row) */
    lv_obj_t *ds_btn = lv_btn_create(scr);
    lv_obj_set_size(ds_btn, 216, 40);
    lv_obj_align(ds_btn, LV_ALIGN_TOP_MID, 0, 210);
    lv_obj_set_style_bg_color(ds_btn, lv_color_hex(0x4527A0), 0);
    lv_obj_set_style_text_color(ds_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(ds_btn, 8, 0);
    lv_obj_t *ds_label = lv_label_create(ds_btn);
    lv_label_set_text(ds_label, "Deep Sleep");
    lv_obj_center(ds_label);
    lv_obj_add_event_cb(ds_btn, deepsleep_click_cb, LV_EVENT_CLICKED, NULL);

    /* Hint at the bottom */
    s_hint_label = lv_label_create(scr);
    lv_label_set_text(s_hint_label, "KEY3: back to Home");
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, -8);

    /* One-shot timer for the deep-sleep 3-press window (starts paused) */
    s_ds_timer = lv_timer_create(ds_timeout_cb, DS_PRESS_WINDOW_MS, NULL);
    lv_timer_pause(s_ds_timer);

    return scr;
}
