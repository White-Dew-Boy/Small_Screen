#include "ui_pc_perf.h"
#include "lvgl.h"
#include "ble_perf.h"
#include "pc_perf_mqtt.h"
#include "pc_perf_src.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_pc_perf";

/* Refresh period (ms) */
#define PC_PERF_REFRESH_MS 500

/* Data older than this is considered stale (PC sender stopped) */
#define PC_PERF_STALE_MS 8000

/* Bar range 0..1000 = 0.0% .. 100.0% (one decimal without %f formatting) */
#define BAR_MAX 1000

/* Value label buffer: must hold the longest label. With int-typed args GCC
 * assumes the full int range for %d (11 chars each), so "%d.%d/%d.%d GB"
 * needs 51 bytes worst-case — 56 leaves margin. */
#define VAL_BUF 56

static lv_obj_t *s_scr = NULL;

/* Bar rows: name + bar + value label */
static lv_obj_t *s_cpu_bar, *s_cpu_val;
static lv_obj_t *s_mem_bar, *s_mem_val;
static lv_obj_t *s_gpu_bar, *s_gpu_val;   /* optional: hidden when no data */
static lv_obj_t *s_disk_bar, *s_disk_val; /* optional */

/* Plain text rows */
static lv_obj_t *s_up_val;
static lv_obj_t *s_down_val;
static lv_obj_t *s_temp_val;              /* optional */
static lv_obj_t *s_fps_val;               /* optional */

/* Optional rows (auto-hidden when the sender never reports the value) */
static lv_obj_t *s_gpu_row;
static lv_obj_t *s_disk_row;
static lv_obj_t *s_temp_row;
static lv_obj_t *s_fps_row;

/* BLE started flag. NimBLE needs a lot of RAM, so it is never started at
 * boot; in BLE data-source mode it starts once, the first time this page is
 * shown (a few hundred ms of LVGL-task blocking, see pc_perf_timer_cb). */
static bool s_ble_started = false;

/* Top-right "Config" button -> Config page (wired by main.c) */
static void (*s_cfg_cb)(void) = NULL;

/*---------------------------------------------------------------------------
 * Helpers
 *---------------------------------------------------------------------------*/

/* Split a float into an integer part and a single decimal digit
 * (LV_SPRINTF_USE_FLOAT is off in this project, so %f is avoided). Each
 * label is then formatted with ONE snprintf straight into the final buffer
 * — GCC's -Wformat-truncation can prove the result fits and won't fire. */
static void split1(float v, int *vi, int *frac)
{
    *vi = (int)v;
    *frac = (int)((v - (float)*vi) * 10.0f);
    if (*frac < 0) {
        *frac = -*frac;
    }
    if (*frac > 9) {
        *frac = 9;
    }
}

/* Same as split1 but with two decimal digits (used for network speeds,
 * whose wire precision is KB/s x 1000). Format with "%d.%02d". */
static void split2(float v, int *vi, int *frac)
{
    *vi = (int)v;
    *frac = (int)((v - (float)*vi) * 100.0f);
    if (*frac < 0) {
        *frac = -*frac;
    }
    if (*frac > 99) {
        *frac = 99;
    }
}

/* Bar color by usage: green < 60 %, orange 60..85 %, red > 85 % */
static uint32_t bar_color(float pct)
{
    if (pct >= 85.0f) {
        return 0xE53935; /* red   */
    }
    if (pct >= 60.0f) {
        return 0xFB8C00; /* orange */
    }
    return 0x43A047; /* green */
}

/*---------------------------------------------------------------------------
 * Widget building (landscape 320x240 layout)
 *
 * Bar rows (150 x 46): metric name top-left, value top-right on the same
 * line, and a full-width bar below. Name and value never overlap (both on
 * the top line, left vs right); the bar starts below the text line, so
 * nothing can collide.
 *
 * Plain text rows (150 x 22): only a name + value line — deliberately half
 * the height of a bar row so stacked rows like Upload/Download sit close
 * together without a blank line between them.
 *---------------------------------------------------------------------------*/

/* One usage row: name + value on top, full-width bar underneath. */
static lv_obj_t *make_bar_row(lv_obj_t *body, const char *name,
                              lv_obj_t **out_bar, lv_obj_t **out_val)
{
    lv_obj_t *row = lv_obj_create(body);
    lv_obj_set_size(row, 150, 46);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name_lbl = lv_label_create(row);
    lv_label_set_text(name_lbl, name);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(0x8AB4F8), 0);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, 0, 2);

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_color(val, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(val, LV_ALIGN_TOP_RIGHT, 0, 2);

    lv_obj_t *bar = lv_bar_create(row);
    lv_obj_set_size(bar, 146, 12);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 24);
    lv_bar_set_range(bar, 0, BAR_MAX);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, 6, 0);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);

    *out_bar = bar;
    *out_val = val;
    return row;
}

/* One plain value row: name left, value right (no bar). Half-height so
 * stacked rows (Upload/Download/…) sit close together. */
static lv_obj_t *make_text_row(lv_obj_t *body, const char *name,
                               lv_obj_t **out_val)
{
    lv_obj_t *row = lv_obj_create(body);
    lv_obj_set_size(row, 150, 22);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name_lbl = lv_label_create(row);
    lv_label_set_text(name_lbl, name);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(0x8AB4F8), 0);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, 0, 2);

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_color(val, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(val, LV_ALIGN_TOP_RIGHT, 0, 2);

    *out_val = val;
    return row;
}

/*---------------------------------------------------------------------------
 * Refresh
 *---------------------------------------------------------------------------*/

static void set_bar(lv_obj_t *bar, lv_obj_t *val_lbl, float pct,
                    const char *text, bool fresh)
{
    int permil = (int)(pct * 10.0f); /* pct*1000/BAR_MAX*... = pct*10 */
    if (permil < 0) {
        permil = 0;
    }
    if (permil > BAR_MAX) {
        permil = BAR_MAX;
    }
    lv_bar_set_value(bar, permil, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(bar_color(pct)), LV_PART_INDICATOR);

    lv_label_set_text(val_lbl, text);
    lv_obj_set_style_text_color(val_lbl,
                                fresh ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x757575), 0);
}

/* Show/hide an optional row depending on whether the sender reports it. */
static void set_optional(lv_obj_t *row, lv_obj_t *val_lbl, bool present,
                         const char *text)
{
    if (present) {
        lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(val_lbl, text);
        lv_obj_set_style_text_color(val_lbl, lv_color_hex(0xFFFFFF), 0);
    } else {
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }
}

/*---------------------------------------------------------------------------
 * Rendering + refresh
 *---------------------------------------------------------------------------*/

/* True when the last frame carried valid data for `bit` (PC_PERF_P_*). */
static bool field_present(const pc_perf_data_t *d, uint8_t bit)
{
    return d != NULL && (d->present & bit) != 0;
}

/* Render the metric rows from snapshot `d`. Display is presence-driven
 * (v2 BLE protocol): a field whose flag bit is 0 has no data and must not
 * be shown — 0 itself is a legal measured value. With fresh data the real
 * values are shown; otherwise (NULL, stale or no link) bars are zeroed and
 * values dashed. Rows for fields that were present before stay visible
 * with "--"; rows for absent fields stay hidden. */
static void render_rows(pc_perf_data_t *d, bool fresh)
{
    if (d == NULL || !fresh) {
        /* No usable data: zero ALL bars (incl. the optional GPU/disk ones,
         * which keep their last value otherwise) and dash the values */
        lv_bar_set_value(s_cpu_bar, 0, LV_ANIM_OFF);
        lv_bar_set_value(s_mem_bar, 0, LV_ANIM_OFF);
        lv_bar_set_value(s_gpu_bar, 0, LV_ANIM_OFF);
        lv_bar_set_value(s_disk_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_cpu_bar, lv_color_hex(0x43A047), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_mem_bar, lv_color_hex(0x43A047), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_gpu_bar, lv_color_hex(0x43A047), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_disk_bar, lv_color_hex(0x43A047), LV_PART_INDICATOR);
        lv_label_set_text(s_cpu_val, "--");
        lv_label_set_text(s_mem_val, "--");
        lv_label_set_text(s_up_val, "--");
        lv_label_set_text(s_down_val, "--");
        lv_obj_set_style_text_color(s_cpu_val, lv_color_hex(0x757575), 0);
        lv_obj_set_style_text_color(s_mem_val, lv_color_hex(0x757575), 0);
        lv_obj_set_style_text_color(s_up_val, lv_color_hex(0x757575), 0);
        lv_obj_set_style_text_color(s_down_val, lv_color_hex(0x757575), 0);
        set_optional(s_gpu_row, s_gpu_val, field_present(d, PC_PERF_P_GPU), "--");
        set_optional(s_disk_row, s_disk_val, field_present(d, PC_PERF_P_DISK), "--");
        set_optional(s_temp_row, s_temp_val, field_present(d, PC_PERF_P_TEMP), "--");
        set_optional(s_fps_row, s_fps_val, field_present(d, PC_PERF_P_FPS), "--");
        return;
    }

    /* Fresh data from snapshot `d` */
    int vi, fr;
    char txt[VAL_BUF];

    /* CPU / memory: shown whenever reported; 0 is a legal value. */
    if (field_present(d, PC_PERF_P_CPU)) {
        split1(d->cpu_pct, &vi, &fr);
        snprintf(txt, sizeof(txt), "%d.%d%%", vi, fr);
        set_bar(s_cpu_bar, s_cpu_val, d->cpu_pct, txt, fresh);
    } else {
        lv_bar_set_value(s_cpu_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(s_cpu_val, "--");
        lv_obj_set_style_text_color(s_cpu_val, lv_color_hex(0x757575), 0);
    }

    if (field_present(d, PC_PERF_P_MEM)) {
        split1(d->mem_pct, &vi, &fr);
        snprintf(txt, sizeof(txt), "%d.%d%%", vi, fr);
        set_bar(s_mem_bar, s_mem_val, d->mem_pct, txt, fresh);
    } else {
        lv_bar_set_value(s_mem_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(s_mem_val, "--");
        lv_obj_set_style_text_color(s_mem_val, lv_color_hex(0x757575), 0);
    }

    /* GPU / disk rows: presence-driven. When the flag bit is 0 the raw
     * value is meaningless: hide the row and clear its bar. */
    if (field_present(d, PC_PERF_P_GPU)) {
        split1(d->gpu_pct, &vi, &fr);
        snprintf(txt, sizeof(txt), "%d.%d%%", vi, fr);
        set_bar(s_gpu_bar, s_gpu_val, d->gpu_pct, txt, fresh);
        lv_obj_clear_flag(s_gpu_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_bar_set_value(s_gpu_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_gpu_bar, lv_color_hex(0x43A047), LV_PART_INDICATOR);
        lv_obj_add_flag(s_gpu_row, LV_OBJ_FLAG_HIDDEN);
    }

    if (field_present(d, PC_PERF_P_DISK)) {
        split1(d->disk_pct, &vi, &fr);
        snprintf(txt, sizeof(txt), "%d.%d%%", vi, fr);
        set_bar(s_disk_bar, s_disk_val, d->disk_pct, txt, fresh);
        lv_obj_clear_flag(s_disk_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_bar_set_value(s_disk_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_disk_bar, lv_color_hex(0x43A047), LV_PART_INDICATOR);
        lv_obj_add_flag(s_disk_row, LV_OBJ_FLAG_HIDDEN);
    }

    /* Upload / download speeds (KB/s, two decimals to keep slow links
     * readable, e.g. 0.44 KB/s) */
    if (field_present(d, PC_PERF_P_UP)) {
        split2(d->up_kbs, &vi, &fr);
        snprintf(txt, sizeof(txt), "%d.%02d KB/s", vi, fr);
        lv_label_set_text(s_up_val, txt);
        lv_obj_set_style_text_color(s_up_val, lv_color_hex(0xFFFFFF), 0);
    } else {
        lv_label_set_text(s_up_val, "--");
        lv_obj_set_style_text_color(s_up_val, lv_color_hex(0x757575), 0);
    }

    if (field_present(d, PC_PERF_P_DOWN)) {
        split2(d->down_kbs, &vi, &fr);
        snprintf(txt, sizeof(txt), "%d.%02d KB/s", vi, fr);
        lv_label_set_text(s_down_val, txt);
        lv_obj_set_style_text_color(s_down_val, lv_color_hex(0xFFFFFF), 0);
    } else {
        lv_label_set_text(s_down_val, "--");
        lv_obj_set_style_text_color(s_down_val, lv_color_hex(0x757575), 0);
    }

    /* Optional text rows: CPU temperature (may be negative) / FPS */
    if (field_present(d, PC_PERF_P_TEMP)) {
        split1(d->temp_c, &vi, &fr);
        snprintf(txt, sizeof(txt), "%d.%d C", vi, fr);
        lv_label_set_text(s_temp_val, txt);
        lv_obj_set_style_text_color(s_temp_val, lv_color_hex(0xFFFFFF), 0);
        lv_obj_clear_flag(s_temp_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_temp_row, LV_OBJ_FLAG_HIDDEN);
    }

    if (field_present(d, PC_PERF_P_FPS)) {
        snprintf(txt, sizeof(txt), "%d", (int)d->fps);
        lv_label_set_text(s_fps_val, txt);
        lv_obj_set_style_text_color(s_fps_val, lv_color_hex(0xFFFFFF), 0);
        lv_obj_clear_flag(s_fps_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_fps_row, LV_OBJ_FLAG_HIDDEN);
    }
}

static void pc_perf_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (s_scr == NULL || lv_disp_get_scr_act(NULL) != s_scr) {
        /* Page not visible. Keep the link while the source is BLE, but
         * stop the connectable advertising so the radio does not beacon
         * while nobody is watching (advertising resumes when this page is
         * shown again in BLE mode). When the source is NOT BLE, also
         * terminate an established BLE link so the PC sees the disconnect.
         */
        if (s_ble_started) {
            pc_perf_advertise_stop();
            if (pc_perf_src_get() != PC_PERF_SRC_BLE) {
                pc_perf_disconnect();
            }
        }
        return;
    }

    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    switch (pc_perf_src_get()) {
    case PC_PERF_SRC_OFF:
        /* Never beacon when the source is off, and drop an established
         * BLE link so the PC sees the disconnect. */
        pc_perf_disconnect();
        pc_perf_advertise_stop();
        render_rows(NULL, false);
        return;

    case PC_PERF_SRC_BLE: {
        /* BLE transport only. NimBLE needs a lot of RAM, so it is NOT
         * started at boot: it starts once, the first time this page is
         * shown (a few hundred ms of LVGL-task blocking), then advertises
         * until a PC connects. Link state is shown on the Config page. */
        if (!s_ble_started) {
            s_ble_started = true;
            esp_err_t r = pc_perf_init();
            if (r != ESP_OK) {
                ESP_LOGE(TAG, "pc_perf_init failed: %s", esp_err_to_name(r));
            }
        }
        /* Beacon while this page is visible so the PC can find/connect.
         * pc_perf_advertise_start() is a no-op when the host is not synced
         * yet, a PC is already connected, or advertising already runs. */
        pc_perf_advertise_start();

        pc_perf_data_t ble;
        pc_perf_get_data(&ble);
        const bool fresh = ble.connected && ble.valid &&
                           (now_ms - ble.last_update_ms) < PC_PERF_STALE_MS;
        render_rows(&ble, fresh);
        return;
    }

    case PC_PERF_SRC_MQTT:
    default: {
        /* MQTT transport only (the default). BLE is never started here;
         * if it was started earlier (source changed), stop beaconing and
         * disconnect an established link. Link state is on the Config page. */
        pc_perf_disconnect();
        pc_perf_advertise_stop();
        pc_perf_data_t mq;
        pc_perf_mqtt_get_data(&mq); /* connected = broker link up */
        const bool fresh = mq.connected && mq.valid &&
                           (now_ms - mq.last_update_ms) < PC_PERF_STALE_MS;
        render_rows(&mq, fresh);
        return;
    }
    }
}

/*---------------------------------------------------------------------------
 * Public API
 *---------------------------------------------------------------------------*/

void ui_pc_perf_set_cfg_cb(void (*cb)(void))
{
    s_cfg_cb = cb;
}

/* Top-right "Config" button pressed -> Config page (LVGL thread) */
static void cfg_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_cfg_cb != NULL) {
        s_cfg_cb();
    }
}

/*---------------------------------------------------------------------------
 * Create
 *---------------------------------------------------------------------------*/

lv_obj_t *ui_pc_perf_create(void)
{
    /* The page is shown in LANDSCAPE (320x240); main.c calls
     * lv_port_set_landscape(true) and resizes this screen before loading. */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 240);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
    s_scr = scr;

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "PC Performance");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    /* Left-aligned: a 20 px centered title would run into the top-right
     * Config button. */
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 6);

    /* Top-right button: open the Config page (data source + link state).
     * Sized for comfortable touch (92x36). */
    lv_obj_t *cfg_btn = lv_btn_create(scr);
    lv_obj_set_size(cfg_btn, 92, 32);
    lv_obj_align(cfg_btn, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_set_style_bg_color(cfg_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(cfg_btn, lv_color_hex(0x8AB4F8), 0);
    lv_obj_t *cfg_label = lv_label_create(cfg_btn);
    lv_label_set_text(cfg_label, "Config");
    lv_obj_center(cfg_label);
    lv_obj_add_event_cb(cfg_btn, cfg_btn_cb, LV_EVENT_CLICKED, NULL);

    /* Two side-by-side flex columns; hidden optional rows collapse.
     * 4 rows x 46 px + 3 x 2 px gaps = 190 px, starting at y=42. */
    lv_obj_t *left = lv_obj_create(scr);
    lv_obj_set_size(left, 150, 190);
    lv_obj_align(left, LV_ALIGN_TOP_LEFT, 8, 42);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(left, 2, 0);

    lv_obj_t *right = lv_obj_create(scr);
    lv_obj_set_size(right, 150, 190);
    lv_obj_align(right, LV_ALIGN_TOP_RIGHT, -8, 42);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 0, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(right, 2, 0);

    /* Left column: usage bars */
    make_bar_row(left, "CPU", &s_cpu_bar, &s_cpu_val);
    make_bar_row(left, "Memory", &s_mem_bar, &s_mem_val);
    s_gpu_row = make_bar_row(left, "GPU", &s_gpu_bar, &s_gpu_val);
    s_disk_row = make_bar_row(left, "Disk", &s_disk_bar, &s_disk_val);

    /* Right column: speeds + optional values */
    make_text_row(right, "Up", &s_up_val);
    make_text_row(right, "Down", &s_down_val);
    s_temp_row = make_text_row(right, "CPU Temp", &s_temp_val);
    s_fps_row = make_text_row(right, "FPS", &s_fps_val);

    lv_timer_create(pc_perf_timer_cb, PC_PERF_REFRESH_MS, NULL);

    return scr;
}
