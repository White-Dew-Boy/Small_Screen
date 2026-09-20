#include "ui_home.h"
#include "lvgl.h"
#include "time_manager.h"
#include <time.h>

/* Home menu entries, in layout order (indexes 0..6 are referenced by the
 * add_entry_btn() calls at the bottom). Names only: every button uses the
 * same accent colour, applied in add_entry_btn(). */
static const char *s_entries[] = {
    "Sensor",
    "WiFi",
    "MQTT",
    "LED",
    "SD Card",
    "System",
    "PC Performance"
};

/* Callback to open a page (set by main.c, invoked from the LVGL thread) */
static void (*s_page_cb)(int page) = NULL;
static void (*s_deepsleep_cb)(void) = NULL;

/* Home screen + wall-clock label (updated by a 1 s LVGL timer; also
 * reused to show the deep-sleep press-count feedback) */
static lv_obj_t *s_home_scr = NULL;
static lv_obj_t *s_clock_label = NULL;

static void clock_update_label(void); /* forward decl (used by reset_hint) */

/* Deep sleep needs 3 presses within a 3 s window (accident protection).
 * A one-shot LVGL timer implements the window: it is (re)started on each
 * press and, on expiry, resets the press counter. */
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
    clock_update_label(); /* restore the clock after the press feedback */
}

static void entry_click_cb(lv_event_t *e)
{
    int page = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_page_cb != NULL) {
        s_page_cb(page);
    }
}

/* The 3 s press window expired without a third press: reset the counter. */
static void ds_timeout_cb(lv_timer_t *timer)
{
    (void)timer;
    ui_home_reset_hint();
    lv_timer_pause(s_ds_timer);
}

/* Button height shared by every entry button (landscape 320x240 layout) */
#define ENTRY_H 48

/* Create one page-entry button at (x, y) with the given width. */
static void add_entry_btn(lv_obj_t *scr, int idx, lv_coord_t x,
                          lv_coord_t y, lv_coord_t w)
{
    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, w, ENTRY_H);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1565C0), 0);
    lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(btn, 8, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, s_entries[idx]);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, entry_click_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)idx);
}

/* Deep sleep fires after 3 presses inside the window; each press shows
 * the remaining count in the clock label and (re)starts the timeout. */
static void deepsleep_click_cb(lv_event_t *e)
{
    (void)e;
    s_ds_count++;

    if (s_ds_count < DS_PRESS_REQUIRED) {
        /* Show the feedback in the top clock label (the 1 s clock timer
         * skips refreshing while the counter is non-zero). */
        if (s_clock_label != NULL) {
            lv_label_set_text_fmt(s_clock_label, "Press %d more to sleep",
                                  DS_PRESS_REQUIRED - s_ds_count);
            lv_obj_set_style_text_color(s_clock_label,
                                        lv_color_hex(0xFFB300), 0);
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

/* Wall-clock label content: the SNTP-synced local time (TZ from
 * Kconfig) in white, or a gray placeholder until the first sync (needs
 * WiFi + NTP reachable). */
static void clock_update_label(void)
{
    if (s_clock_label == NULL) {
        return;
    }

    if (time_manager_is_synced()) {
        time_t now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);
        lv_label_set_text_fmt(s_clock_label, "%04d-%02d-%02d %02d:%02d",
                              t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                              t.tm_hour, t.tm_min);
        lv_obj_set_style_text_color(s_clock_label, lv_color_hex(0xFFFFFF), 0);
    } else {
        lv_label_set_text(s_clock_label, "Syncing time...");
        lv_obj_set_style_text_color(s_clock_label, lv_color_hex(0x9E9E9E), 0);
    }
}

/* 1 s timer: refreshes the clock label, but only while Home is on screen
 * and no deep-sleep press feedback is occupying the label. */
static void clock_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (s_home_scr == NULL || lv_disp_get_scr_act(NULL) != s_home_scr) {
        return; /* Home not visible */
    }

    if (s_ds_count > 0) {
        return; /* "Press N more to sleep" is showing; wait for reset */
    }

    clock_update_label();
}

lv_obj_t *ui_home_create(void)
{
    /* Landscape 320x240, like the PC-Perf page: main.c rotates the whole
     * display to landscape before this screen is loaded. */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 240);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "WhiteLee");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 4);

    /* Wall-clock time (SNTP-synced, shown once available); the label is
     * also reused to show the deep-sleep press feedback */
    s_home_scr = scr;
    s_clock_label = lv_label_create(scr);
    lv_label_set_text(s_clock_label, "Syncing time...");
    lv_obj_set_style_text_color(s_clock_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(s_clock_label, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_timer_create(clock_timer_cb, 1000, NULL);

    /* Three rows in the 320x240 space:
     *   row 0 (y=46):  Sensor, WiFi, MQTT    (3 buttons, 93 px wide)
     *   row 1 (y=104): LED, SD Card, System  (3 buttons, 93 px wide)
     *   row 2 (y=162): PC Perf, Deep Sleep   (2 buttons, 144 px wide)
     * Buttons are ENTRY_H (48 px) tall; row pitch 58 px (10 px gap).
     * Short labels (Sensor/WiFi/MQTT/LED) get three per row; the longer
     * ones (SD Card/System/PC Perf/Deep Sleep) sit two per row. */
    const lv_coord_t y_row0 = 46, y_row1 = 104, y_row2 = 162;

    /* Row 0: short labels, three per row */
    add_entry_btn(scr, 0, 12, y_row0, 93);  /* Sensor */
    add_entry_btn(scr, 1, 113, y_row0, 93); /* WiFi */
    add_entry_btn(scr, 2, 214, y_row0, 93); /* MQTT */

    /* Row 1: short + medium labels, three per row */
    add_entry_btn(scr, 3, 12, y_row1, 93);   /* LED */
    add_entry_btn(scr, 4, 113, y_row1, 93);  /* SD Card */
    add_entry_btn(scr, 5, 214, y_row1, 93);  /* System */

    /* Row 2: longer labels, two per row (wide buttons) */
    add_entry_btn(scr, 6, 12, y_row2, 144);  /* PC Perf */
    lv_obj_t *ds_btn = lv_btn_create(scr);
    lv_obj_set_size(ds_btn, 144, ENTRY_H);
    lv_obj_align(ds_btn, LV_ALIGN_TOP_LEFT, 164, y_row2);
    lv_obj_set_style_bg_color(ds_btn, lv_color_hex(0x1565C0), 0);
    lv_obj_set_style_text_color(ds_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(ds_btn, 8, 0);
    lv_obj_t *ds_label = lv_label_create(ds_btn);
    lv_label_set_text(ds_label, "Deep Sleep");
    lv_obj_center(ds_label);
    lv_obj_add_event_cb(ds_btn, deepsleep_click_cb, LV_EVENT_CLICKED, NULL);

    /* One-shot timer for the deep-sleep 3-press window (starts paused) */
    s_ds_timer = lv_timer_create(ds_timeout_cb, DS_PRESS_WINDOW_MS, NULL);
    lv_timer_pause(s_ds_timer);

    return scr;
}
