#include "ui_pc_perf.h"
#include "lvgl.h"
#include "ble_perf.h"
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

/* Status line (connection + data age) */
static lv_obj_t *s_status_lbl;

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

/* Lazy BLE start flag (set on first visible timer tick) */
static bool s_ble_started = false;

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
 * Row layout (150 x 46): metric name top-left, value top-right on the same
 * line, and (for bar rows) a full-width bar below. Name and value never
 * overlap (both on the top line, left vs right); the bar starts below the
 * text line, so nothing can collide.
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

/* One plain value row: name left, value right (no bar). */
static lv_obj_t *make_text_row(lv_obj_t *body, const char *name,
                               lv_obj_t **out_val)
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

static void pc_perf_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (s_scr == NULL || lv_disp_get_scr_act(NULL) != s_scr) {
        return; /* page not visible */
    }

    /* Lazy BLE start: internal RAM is too tight to run NimBLE at boot, so
     * the peripheral (re)starts the first time this page is shown. One-time
     * (~a few hundred ms of LVGL-task blocking), then advertising runs. */
    if (!s_ble_started) {
        s_ble_started = true;
        esp_err_t r = pc_perf_init();
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "pc_perf_init failed: %s", esp_err_to_name(r));
        }
    }

    pc_perf_data_t d;
    pc_perf_get_data(&d);

    const bool fresh = d.valid &&
                       ((uint32_t)(esp_timer_get_time() / 1000) - d.last_update_ms) < PC_PERF_STALE_MS;

    /* Status line */
    if (!d.connected) {
        lv_label_set_text(s_status_lbl, "BLE: waiting for PC...");
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0x9E9E9E), 0);
    } else if (!fresh) {
        lv_label_set_text(s_status_lbl, "BLE: connected, no fresh data");
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0xFFB300), 0);
    } else {
        uint32_t age = (uint32_t)(esp_timer_get_time() / 1000) - d.last_update_ms;
        lv_label_set_text_fmt(s_status_lbl, "BLE: connected (%lu s ago)",
                              (unsigned long)(age / 1000));
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0x66BB6A), 0);
    }

    if (!d.valid || !fresh) {
        /* No usable data yet: zero the bars, dash the values */
        lv_bar_set_value(s_cpu_bar, 0, LV_ANIM_OFF);
        lv_bar_set_value(s_mem_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_cpu_bar, lv_color_hex(0x43A047), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_mem_bar, lv_color_hex(0x43A047), LV_PART_INDICATOR);
        lv_label_set_text(s_cpu_val, "--");
        lv_label_set_text(s_mem_val, "--");
        lv_label_set_text(s_up_val, "--");
        lv_label_set_text(s_down_val, "--");
        lv_obj_set_style_text_color(s_cpu_val, lv_color_hex(0x757575), 0);
        lv_obj_set_style_text_color(s_mem_val, lv_color_hex(0x757575), 0);
        lv_obj_set_style_text_color(s_up_val, lv_color_hex(0x757575), 0);
        lv_obj_set_style_text_color(s_down_val, lv_color_hex(0x757575), 0);
        set_optional(s_gpu_row, s_gpu_val, d.gpu_pct > 0.1f, "--");
        set_optional(s_disk_row, s_disk_val, d.disk_pct > 0.1f, "--");
        set_optional(s_temp_row, s_temp_val, d.temp_c > 0.1f, "--");
        set_optional(s_fps_row, s_fps_val, d.fps > 0.1f, "--");
        return;
    }

    /* Usage bars */
    int vi, fr;
    char txt[VAL_BUF];

    split1(d.cpu_pct, &vi, &fr);
    snprintf(txt, sizeof(txt), "%d.%d%%", vi, fr);
    set_bar(s_cpu_bar, s_cpu_val, d.cpu_pct, txt, fresh);

    split1(d.mem_pct, &vi, &fr);
    snprintf(txt, sizeof(txt), "%d.%d%%", vi, fr);
    set_bar(s_mem_bar, s_mem_val, d.mem_pct, txt, fresh);

    /* GPU / disk: only if the sender reports them */
    if (d.gpu_pct > 0.1f) {
        split1(d.gpu_pct, &vi, &fr);
        snprintf(txt, sizeof(txt), "%d.%d%%", vi, fr);
        set_bar(s_gpu_bar, s_gpu_val, d.gpu_pct, txt, fresh);
    }
    set_optional(s_gpu_row, s_gpu_val, d.gpu_pct > 0.1f, txt);

    if (d.disk_pct > 0.1f) {
        split1(d.disk_pct, &vi, &fr);
        snprintf(txt, sizeof(txt), "%d.%d%%", vi, fr);
        set_bar(s_disk_bar, s_disk_val, d.disk_pct, txt, fresh);
    }
    set_optional(s_disk_row, s_disk_val, d.disk_pct > 0.1f, txt);

    /* Upload / download speeds (KB/s, two decimals to keep slow links
     * readable, e.g. 0.44 KB/s) */
    split2(d.up_kbs, &vi, &fr);
    snprintf(txt, sizeof(txt), "%d.%02d KB/s", vi, fr);
    lv_label_set_text(s_up_val, txt);
    lv_obj_set_style_text_color(s_up_val, fresh ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x757575), 0);

    split2(d.down_kbs, &vi, &fr);
    snprintf(txt, sizeof(txt), "%d.%02d KB/s", vi, fr);
    lv_label_set_text(s_down_val, txt);
    lv_obj_set_style_text_color(s_down_val, fresh ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x757575), 0);

    /* Optional: CPU temperature / FPS */
    if (d.temp_c > 0.1f) {
        split1(d.temp_c, &vi, &fr);
        snprintf(txt, sizeof(txt), "%d.%d C", vi, fr);
    }
    set_optional(s_temp_row, s_temp_val, d.temp_c > 0.1f, txt);

    if (d.fps > 0.1f) {
        snprintf(txt, sizeof(txt), "%d", (int)d.fps);
    }
    set_optional(s_fps_row, s_fps_val, d.fps > 0.1f, txt);
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
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    s_status_lbl = lv_label_create(scr);
    lv_label_set_text(s_status_lbl, "BLE: waiting for PC...");
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, 22);

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
    make_text_row(right, "Upload", &s_up_val);
    make_text_row(right, "Download", &s_down_val);
    s_temp_row = make_text_row(right, "CPU Temp", &s_temp_val);
    s_fps_row = make_text_row(right, "FPS", &s_fps_val);

    lv_timer_create(pc_perf_timer_cb, PC_PERF_REFRESH_MS, NULL);

    return scr;
}
