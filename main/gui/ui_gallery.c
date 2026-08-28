#include "ui_gallery.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "lvgl.h"

#include "sd_card.h"
#include "sd_file.h"

#ifndef CONFIG_UPLOAD_DIR
#define CONFIG_UPLOAD_DIR "/sdcard/esp32_files"
#endif

#define GALLERY_MAX_PICS     64
#define PIC_W                240
#define PIC_H                320
#define PIC_BYTES            (PIC_W * PIC_H * 2) /* 153,600 */
#define SLIDESHOW_PERIOD_MS  10000

/* Widgets */
static lv_obj_t *s_scr;
static lv_obj_t *s_img; /* full-screen lv_img showing the current frame */

/* Gallery state */
static char s_pics[GALLERY_MAX_PICS][SD_FILE_NAME_MAX + 1];
static int s_pic_count = 0;
static int s_cur = 0;

static uint8_t *s_buf = NULL;   /* 150 KB frame buffer (PSRAM) */
static lv_img_dsc_t s_dsc;      /* descriptor pointing at s_buf */

/* A gallery picture is a 240x320 raw RGB565 dump (.bin, exact size). */
static bool is_picture(const sd_file_entry_t *e)
{
    size_t n = strlen(e->name);
    if (n < 4) {
        return false;
    }
    return strcasecmp(e->name + n - 4, ".bin") == 0 && e->size == PIC_BYTES;
}

/* Load one frame from SD and show it full-screen. The blocking read
 * (~50-100 ms) runs on the LVGL task as required by the shared-SPI
 * constraint; the screen just holds the previous frame during the read. */
static esp_err_t load_pic(int idx)
{
    char path[SD_FILE_NAME_MAX + 64];
    snprintf(path, sizeof(path), "%s/%s", CONFIG_UPLOAD_DIR, s_pics[idx]);

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t got = fread(s_buf, 1, PIC_BYTES, f);
    fclose(f);
    if (got != PIC_BYTES) {
        return ESP_ERR_INVALID_SIZE; /* truncated / corrupt frame */
    }

    s_dsc.header.always_zero = 0;
    s_dsc.header.w = PIC_W;
    s_dsc.header.h = PIC_H;
    s_dsc.header.cf = LV_IMG_CF_TRUE_COLOR; /* standard RGB565 */
    s_dsc.data_size = PIC_BYTES;
    s_dsc.data = s_buf;
    lv_img_set_src(s_img, &s_dsc);
    return ESP_OK;
}

static void show_pic(int idx)
{
    if (s_pic_count == 0) {
        return;
    }
    s_cur = idx;
    load_pic(s_cur); /* on failure keep showing the previous frame */
}

static void next_pic(void)
{
    if (s_pic_count > 0) {
        show_pic((s_cur + 1) % s_pic_count);
    }
}

/* Auto-advance: only while the page is on screen. */
static void slideshow_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (lv_disp_get_scr_act(NULL) != s_scr) {
        return;
    }
    if (s_pic_count < 2) {
        return;
    }
    next_pic();
}

void ui_gallery_refresh(void)
{
    /* Re-scan the upload dir for 240x320 .bin pictures. */
    static sd_file_entry_t entries[GALLERY_MAX_PICS]; /* not on LVGL stack */
    s_pic_count = 0;
    s_cur = 0;

    size_t count = 0;
    if (sd_card_is_mounted() &&
        sd_file_list(CONFIG_UPLOAD_DIR, entries, GALLERY_MAX_PICS, &count) == ESP_OK) {
        for (size_t i = 0; i < count && s_pic_count < GALLERY_MAX_PICS; i++) {
            if (is_picture(&entries[i])) {
                snprintf(s_pics[s_pic_count], sizeof(s_pics[0]), "%s",
                         entries[i].name);
                s_pic_count++;
            }
        }
    }

    if (s_pic_count == 0) {
        lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN); /* blank screen */
        return;
    }

    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    show_pic(0);
}

lv_obj_t *ui_gallery_create(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x000000), 0);

    /* 150 KB frame buffer; PSRAM first (plenty free), internal RAM as a
     * fallback so the page still works on PSRAM-less configs. */
    s_buf = heap_caps_malloc(PIC_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_buf == NULL) {
        s_buf = heap_caps_malloc(PIC_BYTES, MALLOC_CAP_8BIT);
    }

    /* Full-screen image, no decorations. The default theme's card style
     * only targets exact lv_obj_class, but clear bg/border/pad anyway as
     * a safety so the picture is truly edge to edge. */
    s_img = lv_img_create(s_scr);
    lv_obj_center(s_img);
    lv_obj_set_style_bg_opa(s_img, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_img, 0, 0);
    lv_obj_set_style_pad_all(s_img, 0, 0);
    lv_obj_set_style_radius(s_img, 0, 0);
    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_SCROLLABLE);

    lv_timer_create(slideshow_timer_cb, SLIDESHOW_PERIOD_MS, NULL);

    return s_scr;
}
