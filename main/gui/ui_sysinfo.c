#include "ui_sysinfo.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_partition.h"
#include "esp_image_format.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

/* Refresh period; also the CPU% averaging window */
#define SYSINFO_REFRESH_MS 2000

#define MAX_TASKS     24
#define DETAIL_ROWS   (MAX_TASKS + 1) /* every task + the IDLE summary row */
#define ROW_PITCH     18              /* vertical spacing of detail rows */

/*==========================================================================
 * Shared per-task stats collection (used by all three pages)
 *==========================================================================*/

/* Previous run-time counter of each task, matched by handle: the task order
 * returned by uxTaskGetSystemState() is not guaranteed to be stable between
 * calls, so index-based matching would corrupt the CPU% deltas. */
typedef struct {
    TaskHandle_t handle;
    uint32_t run_time;
} prev_stat_t;

static prev_stat_t s_prev[MAX_TASKS];
static int s_prev_count = 0;

typedef struct {
    const char *name;
    uint32_t pct_mil;   /* CPU share of the last window, in per-mille (0..1000) */
    uint32_t stack_hwm;    /* least free stack space since boot, in WORDS
                            * (the kernel divides the fill-byte count by
                            * sizeof(StackType_t); ×4 = bytes on xtensa) */
    uint32_t stack_total;  /* allocated stack size in bytes; 0 = static/unknown */
    bool is_idle;
} sysinfo_task_t;

/* Fill out[] with one entry per task (state-list order) and advance the
 * previous-counter snapshot. Returns the number of entries written.
 * Call once per refresh window — the snapshot only advances when this is
 * called, and only the visible page's timer calls it. */
static int sysinfo_collect(sysinfo_task_t *out, int max)
{
    static TaskStatus_t status[MAX_TASKS];
    UBaseType_t count = uxTaskGetNumberOfTasks();
    if (count > MAX_TASKS) {
        count = MAX_TASKS;
    }

    /* uxTaskGetSystemState() only fills the status array; the total run time
     * it reports is the raw timer value (one core's worth of wall-clock
     * time), which is the wrong denominator on a dual-core chip. */
    UBaseType_t got = uxTaskGetSystemState(status, count, NULL);

    /* Per-task counter deltas over this window. The counters are 32-bit and
     * the ESP timer (1 MHz) wraps every ~4290 s; unsigned subtraction yields
     * the correct delta across a wrap. */
    static uint32_t delta[MAX_TASKS];
    uint32_t delta_total = 0;

    for (UBaseType_t i = 0; i < got; i++) {
        uint32_t prev = 0;
        for (int j = 0; j < s_prev_count; j++) {
            if (s_prev[j].handle == status[i].xHandle) {
                prev = s_prev[j].run_time;
                break;
            }
        }
        delta[i] = status[i].ulRunTimeCounter - prev;
        delta_total += delta[i];
    }

    /* Replace the previous snapshot with the current one */
    s_prev_count = 0;
    for (UBaseType_t i = 0; i < got && s_prev_count < MAX_TASKS; i++) {
        s_prev[s_prev_count].handle = status[i].xHandle;
        s_prev[s_prev_count].run_time = status[i].ulRunTimeCounter;
        s_prev_count++;
    }

    /* CPU share = task delta / sum of all task deltas, scaled to per-mille
     * (×1000) so values below 1% are still visible as one decimal place.
     * The sum (not the raw timer delta) is the denominator: on a dual-core
     * chip two cores of time elapse per wall-clock interval, so using the raw
     * timer value would count every task twice as busy (the two IDLE rows
     * summed to ~190%). With this denominator all rows add up to ~1000 and
     * IDLE shows the true fraction of total CPU capacity that was idle. */
    int n = 0;
    for (UBaseType_t i = 0; i < got && n < max; i++) {
        out[n].name = status[i].pcTaskName;
        out[n].pct_mil = (delta_total > 0)
                             ? (uint32_t)((uint64_t)delta[i] * 1000ULL / delta_total)
                             : 0;
        out[n].stack_hwm = status[i].usStackHighWaterMark;

        /* NOTE: is_idle must be set before the heap lookup below uses it —
         * the status array order is not stable between calls, so a stale
         * is_idle from the previous occupant of this slot would randomly
         * skip the lookup (task showed "-" on some refreshes). */
        out[n].is_idle = (strncmp(status[i].pcTaskName, "IDLE", 4) == 0);

        /* Allocated stack size: available from the heap for dynamically
         * created tasks (their pxStackBase is the start of a heap block).
         * IDLE0/IDLE1 and "Tmr Svc" use static stacks (IDF sets
         * configSUPPORT_STATIC_ALLOCATION=1), so querying the heap with their
         * pxStackBase would trip an assert — leave 0 (= unknown) for them.
         * The plausibility check guards against any other static task. */
        out[n].stack_total = 0;
        if (!out[n].is_idle && strcmp(status[i].pcTaskName, "Tmr Svc") != 0) {
            uint32_t total = (uint32_t)heap_caps_get_allocated_size(
                (void *)status[i].pxStackBase);
            if (total >= out[n].stack_hwm * sizeof(StackType_t)) {
                out[n].stack_total = total;
            }
        }

        n++;
    }
    return n;
}

/* Selection sorts (small arrays, called at most every 2 s) */
static void sort_by_pct_desc(sysinfo_task_t *arr, int n)
{
    for (int i = 0; i < n; i++) {
        int best = i;
        for (int j = i + 1; j < n; j++) {
            if (arr[j].pct_mil > arr[best].pct_mil) {
                best = j;
            }
        }
        sysinfo_task_t tmp = arr[i];
        arr[i] = arr[best];
        arr[best] = tmp;
    }
}

static void sort_by_hwm_asc(sysinfo_task_t *arr, int n)
{
    for (int i = 0; i < n; i++) {
        int best = i;
        for (int j = i + 1; j < n; j++) {
            if (arr[j].stack_hwm < arr[best].stack_hwm) {
                best = j;
            }
        }
        sysinfo_task_t tmp = arr[i];
        arr[i] = arr[best];
        arr[best] = tmp;
    }
}

/*==========================================================================
 * Widget helpers
 *==========================================================================*/

/* Left-aligned 12px text row at (x, y) */
static lv_obj_t *make_line(lv_obj_t *parent, const char *text, uint32_t color,
                           int x, int y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, x, y);
    return lbl;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, uint32_t color,
                             int x, int y, int w, int h, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

/* Flashed firmware size, read once at page creation (image verify reads the
 * whole image, so it must not run on every refresh). */
static uint32_t s_fw_size = 0;    /* actual binary size, bytes */
static uint32_t s_fw_part = 0;    /* app partition size, bytes */

/* Read the size of the flashed firmware image from the app partition header.
 * Uses esp_image_verify() (authoritative parser) — one-time cost, results are
 * cached by the caller. */
static void firmware_size_init(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    if (part == NULL) {
        return;
    }
    s_fw_part = part->size;

    esp_partition_pos_t pos = {
        .offset = part->address,
        .size = part->size,
    };
    esp_image_metadata_t meta;
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) == ESP_OK) {
        s_fw_size = meta.image_len;
    }
}

/* 14px info row: gray label + colored value, two-tone via LVGL inline colors.
 * lv_label_set_recolor() must be enabled or the "#RRGGBB ...#" markers are
 * shown as literal text. */
static lv_obj_t *make_info_row(lv_obj_t *parent, const char *label, int y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_label_set_recolor(lbl, true);
    lv_label_set_text_fmt(lbl, "#9E9E9E %-10s# #FFFFFF ---#", label);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 16, y);
    return lbl;
}

static void set_info_row(lv_obj_t *lbl, const char *label, uint32_t value_color,
                         const char *value)
{
    lv_label_set_text_fmt(lbl, "#9E9E9E %-10s# #%06X %s#",
                          label, (unsigned)(value_color & 0xFFFFFF), value);
}

/*==========================================================================
 * Main System Info page: uptime + memory + CPU summary + 2 buttons
 *==========================================================================*/

static lv_obj_t *s_scr;
static lv_obj_t *up_label;
static lv_obj_t *heap_label;
static lv_obj_t *min_label;
static lv_obj_t *mem_label;
static lv_obj_t *psram_label;
static lv_obj_t *fw_label;
static lv_obj_t *cpu_load_label; /* total CPU load of both cores */

static void (*s_cpu_cb)(void) = NULL;
static void (*s_stack_cb)(void) = NULL;
static void (*s_about_cb)(void) = NULL;

static void sysinfo_main_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (lv_disp_get_scr_act(NULL) != s_scr) {
        return; /* page not visible: skip */
    }

    /* --- Uptime --- */
    uint64_t us = esp_timer_get_time();
    uint32_t secs = (uint32_t)(us / 1000000ULL);
    char buf[24];
    lv_snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
                (unsigned long)(secs / 3600),
                (unsigned long)((secs / 60) % 60),
                (unsigned long)(secs % 60));
    set_info_row(up_label, "Uptime", 0xFFFFFF, buf);

    /* --- Memory --- */
    size_t total = esp_get_free_heap_size();
    size_t min_free = esp_get_minimum_free_heap_size();
    size_t int_free = esp_get_free_internal_heap_size();
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    lv_snprintf(buf, sizeof(buf), "%luK", (unsigned long)(total / 1024));
    set_info_row(heap_label, "Free heap", 0xFFFFFF, buf);

    lv_snprintf(buf, sizeof(buf), "%luK", (unsigned long)(min_free / 1024));
    set_info_row(min_label, "Min free", 0xFFA726, buf);

    lv_snprintf(buf, sizeof(buf), "%luK", (unsigned long)(int_free / 1024));
    set_info_row(mem_label, "Int RAM", 0xFFFFFF, buf);

    lv_snprintf(buf, sizeof(buf), "%luK", (unsigned long)(psram_free / 1024));
    set_info_row(psram_label, "PSRAM", 0xFFFFFF, buf);

    /* --- Firmware size (cached, read once at page creation) --- */
    if (s_fw_size > 0) {
        lv_snprintf(buf, sizeof(buf), "%lu.%02luMB / %lu.%02luMB",
                    (unsigned long)((uint64_t)s_fw_size * 100 / (1024 * 1024) / 100),
                    (unsigned long)((uint64_t)s_fw_size * 100 / (1024 * 1024) % 100),
                    (unsigned long)((uint64_t)s_fw_part * 100 / (1024 * 1024) / 100),
                    (unsigned long)((uint64_t)s_fw_part * 100 / (1024 * 1024) % 100));
    } else {
        lv_snprintf(buf, sizeof(buf), "unknown");
    }
    set_info_row(fw_label, "Firmware", 0xFFFFFF, buf);

    /* --- Total CPU load of both cores (100% - idle) --- */
    sysinfo_task_t tasks[MAX_TASKS];
    int n = sysinfo_collect(tasks, MAX_TASKS);

    uint32_t idle_mil = 0;
    for (int i = 0; i < n; i++) {
        if (tasks[i].is_idle) {
            idle_mil += tasks[i].pct_mil;
        }
    }
    uint32_t busy_mil = (idle_mil > 1000) ? 0 : 1000 - idle_mil;

    lv_snprintf(buf, sizeof(buf), "%lu%%", (unsigned long)(busy_mil / 10));
    /* color-code: green = idle, orange = busy, red = overloaded */
    if (busy_mil >= 800) {
        set_info_row(cpu_load_label, "CPU load", 0xEF5350, buf);
    } else if (busy_mil >= 500) {
        set_info_row(cpu_load_label, "CPU load", 0xFFA726, buf);
    } else {
        set_info_row(cpu_load_label, "CPU load", 0x66BB6A, buf);
    }
}

static void cpu_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_cpu_cb != NULL) {
        s_cpu_cb();
    }
}

static void stack_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_stack_cb != NULL) {
        s_stack_cb();
    }
}

static void about_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_about_cb != NULL) {
        s_about_cb();
    }
}

lv_obj_t *ui_sysinfo_create(void)
{
    /* Landscape 320x240, like the Home/PC-Perf pages: main.c rotates the
     * whole display to landscape before this screen is loaded. */
    s_scr = lv_obj_create(NULL);
    lv_obj_set_size(s_scr, 320, 240);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x101418), 0);

    /* Title (default 14px font) */
    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "System Info");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    /* Always-visible info rows: gray label + white value, 14px.
     * Rows are full-width single lines, spaced every 22 px. */
    up_label     = make_info_row(s_scr, "Uptime", 30);
    heap_label   = make_info_row(s_scr, "Free heap", 52);
    min_label    = make_info_row(s_scr, "Min free", 74);
    mem_label    = make_info_row(s_scr, "Int RAM", 96);
    psram_label  = make_info_row(s_scr, "PSRAM", 118);
    fw_label     = make_info_row(s_scr, "Firmware", 140);
    cpu_load_label = make_info_row(s_scr, "CPU load", 162);

    /* Flashed firmware size: read once (image verify walks the whole image) */
    firmware_size_init();

    /* Detail-page buttons (one row at the bottom) */
    make_button(s_scr, "CPU Load", 0x1565C0, 12, 194, 93, 36, cpu_btn_cb);
    make_button(s_scr, "Stack HWM", 0x00695C, 113, 194, 93, 36, stack_btn_cb);
    make_button(s_scr, "About", 0x4527A0, 214, 194, 93, 36, about_btn_cb);

    lv_timer_create(sysinfo_main_timer_cb, SYSINFO_REFRESH_MS, NULL);

    return s_scr;
}

void ui_sysinfo_set_cpu_cb(void (*cb)(void))
{
    s_cpu_cb = cb;
}

void ui_sysinfo_set_stack_cb(void (*cb)(void))
{
    s_stack_cb = cb;
}

void ui_sysinfo_set_about_cb(void (*cb)(void))
{
    s_about_cb = cb;
}

/*==========================================================================
 * CPU load detail page: every task, busiest first, + IDLE
 *==========================================================================*/

static lv_obj_t *s_cpu_scr;
static lv_obj_t *cpu_rows[DETAIL_ROWS];
static void (*s_cpu_back_cb)(void) = NULL;

static void sysinfo_cpu_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (lv_disp_get_scr_act(NULL) != s_cpu_scr) {
        return;
    }

    sysinfo_task_t tasks[MAX_TASKS];
    int n = sysinfo_collect(tasks, MAX_TASKS);

    uint32_t idle_mil = 0;
    int busy = 0;
    for (int i = 0; i < n; i++) {
        if (tasks[i].is_idle) {
            idle_mil += tasks[i].pct_mil;
        } else {
            tasks[busy++] = tasks[i];
        }
    }
    sort_by_pct_desc(tasks, busy);

    /* Busy tasks in order, then the combined IDLE line right after them,
     * so the visible list is gap-free (each row = one line in the
     * scrollable container). */
    for (int r = 0; r < DETAIL_ROWS; r++) {
        if (r < busy) {
            lv_label_set_text_fmt(cpu_rows[r], "%-10s %2lu.%lu%%",
                                  tasks[r].name,
                                  (unsigned long)(tasks[r].pct_mil / 10),
                                  (unsigned long)(tasks[r].pct_mil % 10));
            lv_obj_clear_flag(cpu_rows[r], LV_OBJ_FLAG_HIDDEN);
        } else if (r == busy) {
            lv_label_set_text_fmt(cpu_rows[r], "%-10s %2lu.%lu%%",
                                  "IDLE",
                                  (unsigned long)(idle_mil / 10),
                                  (unsigned long)(idle_mil % 10));
            lv_obj_clear_flag(cpu_rows[r], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(cpu_rows[r], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void cpu_back_cb(lv_event_t *e)
{
    (void)e;
    if (s_cpu_back_cb != NULL) {
        s_cpu_back_cb();
    }
}

lv_obj_t *ui_sysinfo_cpu_create(void)
{
    /* Landscape 320x240: a scrollable single column holds up to
     * DETAIL_ROWS (25) task rows; content taller than the viewport swipes
     * vertically so every task stays reachable. */
    s_cpu_scr = lv_obj_create(NULL);
    lv_obj_set_size(s_cpu_scr, 320, 240);
    lv_obj_set_style_bg_color(s_cpu_scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(s_cpu_scr);
    lv_label_set_text(title, "CPU Load");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    make_line(s_cpu_scr, "2s avg, all tasks", 0x9E9E9E, 16, 26);

    /* Scrollable row container between the subtitle and the Back button */
    lv_obj_t *cont = lv_obj_create(s_cpu_scr);
    lv_obj_set_pos(cont, 12, 44);
    lv_obj_set_size(cont, 296, 144);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);

    for (int i = 0; i < DETAIL_ROWS; i++) {
        cpu_rows[i] = make_line(cont, "", 0xCFCFCF, 8, i * ROW_PITCH);
        lv_obj_add_flag(cpu_rows[i], LV_OBJ_FLAG_HIDDEN);
    }

    make_button(s_cpu_scr, "Back", 0x455A64, 100, 196, 120, 36, cpu_back_cb);

    lv_timer_create(sysinfo_cpu_timer_cb, SYSINFO_REFRESH_MS, NULL);

    return s_cpu_scr;
}

void ui_sysinfo_cpu_set_back_cb(void (*cb)(void))
{
    s_cpu_back_cb = cb;
}

/*==========================================================================
 * Stack HWM detail page: every task, most at-risk first
 *==========================================================================*/

static lv_obj_t *s_stack_scr;
static lv_obj_t *stack_rows[DETAIL_ROWS];
static void (*s_stack_back_cb)(void) = NULL;

static void sysinfo_stack_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (lv_disp_get_scr_act(NULL) != s_stack_scr) {
        return;
    }

    sysinfo_task_t tasks[MAX_TASKS];
    int n = sysinfo_collect(tasks, MAX_TASKS);
    sort_by_hwm_asc(tasks, n); /* smallest remaining stack first */

    /* Show every collected task (n <= MAX_TASKS); extra pre-created rows
     * stay hidden so the visible list has no gaps. */
    int shown = n;
    for (int r = 0; r < DETAIL_ROWS; r++) {
        if (r >= shown) {
            lv_obj_add_flag(stack_rows[r], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        /* HWM is in words; convert to bytes for a direct comparison with the
         * allocated stack size. */
        uint32_t hwm_bytes = tasks[r].stack_hwm * sizeof(StackType_t);

        if (tasks[r].stack_total > 0) {
            lv_label_set_text_fmt(stack_rows[r], "%-10s %5lu/%lu",
                                  tasks[r].name,
                                  (unsigned long)hwm_bytes,
                                  (unsigned long)tasks[r].stack_total);
            /* highlight tasks that have used more than 85% of their stack */
            if (hwm_bytes < (tasks[r].stack_total * 15) / 100) {
                lv_obj_set_style_text_color(stack_rows[r],
                                            lv_color_hex(0xEF5350), 0);
            } else {
                lv_obj_set_style_text_color(stack_rows[r],
                                            lv_color_hex(0xCFCFCF), 0);
            }
        } else {
            lv_label_set_text_fmt(stack_rows[r], "%-10s %5lu/-",
                                  tasks[r].name, (unsigned long)hwm_bytes);
            lv_obj_set_style_text_color(stack_rows[r],
                                        lv_color_hex(0xCFCFCF), 0);
        }
        lv_obj_clear_flag(stack_rows[r], LV_OBJ_FLAG_HIDDEN);
    }
}

static void stack_back_cb(lv_event_t *e)
{
    (void)e;
    if (s_stack_back_cb != NULL) {
        s_stack_back_cb();
    }
}

lv_obj_t *ui_sysinfo_stack_create(void)
{
    /* Landscape 320x240: a scrollable single column holds up to
     * DETAIL_ROWS (25) task rows; content taller than the viewport swipes
     * vertically so every task stays reachable. */
    s_stack_scr = lv_obj_create(NULL);
    lv_obj_set_size(s_stack_scr, 320, 240);
    lv_obj_set_style_bg_color(s_stack_scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(s_stack_scr);
    lv_label_set_text(title, "Stack HWM");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    make_line(s_stack_scr, "free/total bytes, since boot", 0x9E9E9E, 16, 26);

    /* Scrollable row container between the subtitle and the Back button */
    lv_obj_t *cont = lv_obj_create(s_stack_scr);
    lv_obj_set_pos(cont, 12, 44);
    lv_obj_set_size(cont, 296, 144);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);

    for (int i = 0; i < DETAIL_ROWS; i++) {
        stack_rows[i] = make_line(cont, "", 0xCFCFCF, 8, i * ROW_PITCH);
        lv_obj_add_flag(stack_rows[i], LV_OBJ_FLAG_HIDDEN);
    }

    make_button(s_stack_scr, "Back", 0x455A64, 100, 196, 120, 36, stack_back_cb);

    lv_timer_create(sysinfo_stack_timer_cb, SYSINFO_REFRESH_MS, NULL);

    return s_stack_scr;
}

void ui_sysinfo_stack_set_back_cb(void (*cb)(void))
{
    s_stack_back_cb = cb;
}

/*==========================================================================
 * About page: fixed build/board information (static, no refresh timer)
 *==========================================================================*/

static lv_obj_t *s_about_scr;
static void (*s_about_back_cb)(void) = NULL;

static void about_back_cb(lv_event_t *e)
{
    (void)e;
    if (s_about_back_cb != NULL) {
        s_about_back_cb();
    }
}

lv_obj_t *ui_sysinfo_about_create(void)
{
    /* Landscape 320x240. */
    s_about_scr = lv_obj_create(NULL);
    lv_obj_set_size(s_about_scr, 320, 240);
    lv_obj_set_style_bg_color(s_about_scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(s_about_scr);
    lv_label_set_text(title, "About");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    char buf[48];
    int y = 28;

    /* IDF version (runtime, e.g. "v6.0.1") */
    const char *idf = esp_get_idf_version();
    lv_snprintf(buf, sizeof(buf), "%-10s %s", "IDF", idf ? idf : "?");
    make_line(s_about_scr, buf, 0xCFCFCF, 16, y);
    y += 18;

    /* Chip model + revision */
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const char *model = "?";
    switch (chip.model) {
    case CHIP_ESP32:   model = "ESP32";    break;
    case CHIP_ESP32S2: model = "ESP32-S2"; break;
    case CHIP_ESP32S3: model = "ESP32-S3"; break;
    case CHIP_ESP32C3: model = "ESP32-C3"; break;
    case CHIP_ESP32C6: model = "ESP32-C6"; break;
    default:                               break;
    }
    lv_snprintf(buf, sizeof(buf), "%-10s %s rev %d", "Chip", model, chip.revision);
    make_line(s_about_scr, buf, 0xCFCFCF, 16, y);
    y += 18;

    /* Cores + configured CPU frequency */
    lv_snprintf(buf, sizeof(buf), "%-10s %d x %d MHz", "Cores",
                chip.cores, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    make_line(s_about_scr, buf, 0xCFCFCF, 16, y);
    y += 18;

    /* Flash size (from the flash-size sdkconfig option) */
    lv_snprintf(buf, sizeof(buf), "%-10s %s", "Flash", CONFIG_ESPTOOLPY_FLASHSIZE);
    make_line(s_about_scr, buf, 0xCFCFCF, 16, y);
    y += 18;

    /* PSRAM: physical/available size (esp_psram_get_size), shown with one
     * decimal so integer truncation can't hide a few hundred KB (e.g. an
     * 8 MB chip always reads "8.0 MB", never "7"). */
    size_t psram = esp_psram_get_size();
    if (psram > 0) {
        uint32_t mb_x10 = (uint32_t)((uint64_t)psram * 10 / (1024 * 1024));
        lv_snprintf(buf, sizeof(buf), "%-10s %lu.%lu MB", "PSRAM",
                    (unsigned long)(mb_x10 / 10),
                    (unsigned long)(mb_x10 % 10));
    } else {
        lv_snprintf(buf, sizeof(buf), "%-10s none", "PSRAM");
    }
    make_line(s_about_scr, buf, 0xCFCFCF, 16, y);
    y += 18;

    /* FreeRTOS kernel version, e.g. "V10.5.1" */
    lv_snprintf(buf, sizeof(buf), "%-10s %s", "FreeRTOS", tskKERNEL_VERSION_NUMBER);
    make_line(s_about_scr, buf, 0xCFCFCF, 16, y);
    y += 18;

    /* LVGL version */
    lv_snprintf(buf, sizeof(buf), "%-10s %d.%d.%d", "LVGL",
                LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    make_line(s_about_scr, buf, 0xCFCFCF, 16, y);
    y += 18;

    /* Build date/time of this firmware */
    lv_snprintf(buf, sizeof(buf), "%-10s %s %s", "Built", __DATE__, __TIME__);
    make_line(s_about_scr, buf, 0xCFCFCF, 16, y);
    y += 18;

    /* Station MAC address */
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        lv_snprintf(buf, sizeof(buf), "%-10s %02X:%02X:%02X:%02X:%02X:%02X",
                    "MAC", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        lv_snprintf(buf, sizeof(buf), "%-10s unavailable", "MAC");
    }
    make_line(s_about_scr, buf, 0xCFCFCF, 16, y);
    y += 18;

    make_button(s_about_scr, "Back", 0x455A64, 100, 196, 120, 36, about_back_cb);

    return s_about_scr;
}

void ui_sysinfo_about_set_back_cb(void (*cb)(void))
{
    s_about_back_cb = cb;
}
