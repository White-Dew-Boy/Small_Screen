#pragma once
#include "lvgl.h"

/**
 * @brief Create the main System Info page. Shows runtime memory usage
 *        (free heap / minimum free heap / internal / PSRAM), uptime and the
 *        total CPU load of both cores (color-coded), plus two buttons that
 *        open the CPU load and stack high-water mark detail pages.
 *
 *        The values are refreshed by an internal LVGL timer while the page
 *        is on screen. Requires the FreeRTOS run-time stats feature
 *        (CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) for the CPU load column.
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_sysinfo_create(void);

/**
 * @brief Create the CPU load detail page: every task with its 2 s average
 *        CPU usage, busiest first, plus the combined IDLE row.
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_sysinfo_cpu_create(void);

/**
 * @brief Create the stack high-water mark detail page: every task with the
 *        least stack space it ever had left vs. its allocated stack size
 *        ("free/total" bytes), most at-risk first. Tasks that use more than
 *        85% of their stack are highlighted in red.
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_sysinfo_stack_create(void);

/**
 * @brief Create the About page: fixed build/board information (IDF version,
 *        chip model, cores, CPU frequency, flash/PSRAM size, FreeRTOS and
 *        LVGL versions, build date, MAC address). Static — no refresh.
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_sysinfo_about_create(void);

/**
 * @brief Register callbacks invoked when the "CPU Load" / "Stack HWM" /
 *        "About" buttons on the main page are tapped (LVGL thread).
 */
void ui_sysinfo_set_cpu_cb(void (*cb)(void));
void ui_sysinfo_set_stack_cb(void (*cb)(void));
void ui_sysinfo_set_about_cb(void (*cb)(void));

/**
 * @brief Register callbacks invoked when the detail pages' "Back" button
 *        is tapped (LVGL thread).
 */
void ui_sysinfo_cpu_set_back_cb(void (*cb)(void));
void ui_sysinfo_stack_set_back_cb(void (*cb)(void));
void ui_sysinfo_about_set_back_cb(void (*cb)(void));
