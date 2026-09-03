#include "lvgl.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_check.h"
#include "lcd_driver.h"

static const char *TAG = "lv_port_disp";

static lv_disp_draw_buf_t disp_buf;
static lv_disp_drv_t disp_drv;
static lv_color_t *buf1 = NULL;
static lv_color_t *buf2 = NULL;

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(1);
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    gb_swap_buf((uint16_t *)color_p, w * h);

    /* Synchronous flush: wait until the panel io has really finished.
     * The LCD and the SD card share one SPI bus; if the transfer were left
     * running in the background while an SD probe starts, the shared host
     * would trip an ESP-IDF SPI assert. Waiting here keeps every SPI
     * transfer (LCD flush + SD probe) serialized in the LVGL task. */
    lcd_flush_begin();
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_p);
    lcd_flush_wait();

    lv_disp_flush_ready(drv);
}

esp_err_t lv_port_tick_init(void)
{
    const esp_timer_create_args_t tick_timer_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_timer_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, 1000));
    return ESP_OK;
}

/* Landscape (320x240): used by the PC-Perf page.
 *
 * Two complementary halves:
 *  1. lv_disp_set_rotation(LV_DISP_ROT_90) keeps LVGL's state coherent:
 *     it swaps the logical resolution, resizes every screen and resets the
 *     invalidation areas via lv_disp_drv_update(). (Poking driver->hor_res
 *     directly instead left the refresh geometry stale - flush areas came
 *     out ~245 px wide and the right side of the page never rendered.)
 *  2. The ILI9341 panel is rotated in hardware via MADCTL (swap_xy +
 *     mirror), zero CPU cost.
 *  sw_rotate stays 0 on purpose: LVGL 8's software pixel rotation only runs
 *  when rotated != NONE && sw_rotate, and with our partial draw buffer it
 *  garbles the image. With sw_rotate = 0 the flush callback receives the
 *  UNROTATED logical pixels + coordinates, which the hardware-rotated panel
 *  maps correctly.
 *
 * Mirror combo for landscape (VERIFIED on this panel): portrait uses
 * mirror(true,false); with swap_xy(true) this panel needs:
 *   LV_DISP_ROT_90  -> mirror(true,true)   (one landscape direction)
 *   LV_DISP_ROT_270 -> mirror(false,false) (the other direction, 180 deg)
 * The two must stay consistent: LVGL rotates the touch coordinates
 * according to LANDSCAPE_ROTATION, and the panel mirror has to show the
 * image the way LVGL assumes. */
#define LANDSCAPE_ROTATION LV_DISP_ROT_270
#define LANDSCAPE_MIRROR_X false
#define LANDSCAPE_MIRROR_Y false

esp_err_t lv_port_set_landscape(bool landscape)
{
    lv_disp_t *disp = lv_disp_get_default();
    if (disp == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* No-op if the orientation did not actually change (e.g. switching
     * between two portrait pages) - keeps the log free of noise and makes
     * a real orientation switch obvious. */
    lv_disp_rot_t target = landscape ? LANDSCAPE_ROTATION : LV_DISP_ROT_NONE;
    if (lv_disp_get_rotation(disp) == target) {
        return ESP_OK;
    }

    /* Coherent LVGL state (resolution, screen sizes, invalidation areas) */
    lv_disp_set_rotation(disp, target);

    /* Hardware rotation of the panel (MADCTL MV + mirror) */
    esp_err_t r1, r2;
    if (landscape) {
        r1 = esp_lcd_panel_swap_xy(panel_handle, true);
        r2 = esp_lcd_panel_mirror(panel_handle, LANDSCAPE_MIRROR_X,
                                  LANDSCAPE_MIRROR_Y);
    } else {
        r1 = esp_lcd_panel_swap_xy(panel_handle, false);
        r2 = esp_lcd_panel_mirror(panel_handle, true, false);
    }

    if (r1 != ESP_OK || r2 != ESP_OK) {
        ESP_LOGE(TAG, "panel rotate failed: swap=%s mirror=%s",
                 esp_err_to_name(r1), esp_err_to_name(r2));
        return (r1 != ESP_OK) ? r1 : r2;
    }

    ESP_LOGI(TAG, "display switched to %dx%d (%s)",
             lv_disp_get_hor_res(disp), lv_disp_get_ver_res(disp),
             landscape ? "landscape" : "portrait");

    lv_obj_invalidate(lv_disp_get_scr_act(disp));
    return ESP_OK;
}

esp_err_t lv_port_disp_init(void)
{
    buf1 = heap_caps_malloc(LCD_H_RES * 80 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    buf2 = heap_caps_malloc(LCD_H_RES * 80 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf1 == NULL || buf2 == NULL) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffers");
        return ESP_ERR_NO_MEM;
    }

    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, LCD_H_RES * 80);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
    lv_disp_drv_register(&disp_drv);

    /* Landscape (320x240) is the default orientation of this UI. Rotating
     * right after registration makes it the startup state, so screens are
     * created at the wide size from the beginning (no post-boot switch
     * needed in main.c). lv_port_set_landscape() is intentionally kept
     * intact below so portrait (240x320) remains available on demand. */
    esp_err_t rot = lv_port_set_landscape(true);
    if (rot != ESP_OK) {
        ESP_LOGE(TAG, "default landscape rotation failed: %s",
                 esp_err_to_name(rot));
        return rot;
    }

    return ESP_OK;
}
