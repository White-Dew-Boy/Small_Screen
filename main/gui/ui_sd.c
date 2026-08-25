#include "ui_sd.h"
#include "lvgl.h"
#include "sd_card.h"

/* SD card state machine. Everything runs inside an LVGL timer (i.e. on the
 * LVGL task, same task that performs the LCD flush). This is deliberate:
 * the SD card shares the SPI bus with the LCD, and probing the card from a
 * second task while the LCD is flushing trips an ESP-IDF SPI assert. Running
 * both from the LVGL task (with the synchronous flush in lv_port_disp.c)
 * serializes all SPI traffic. The probes may block for ~100 ms (empty slot)
 * or ~1 s (card removed while mounted) — acceptable on this status page. */

#define SD_POLL_PERIOD_MS 1000

static lv_obj_t *s_scr;
static lv_obj_t *status_label;
static lv_obj_t *info_label;

/* Callback to open the file browser (set by main.c, LVGL thread) */
static void (*s_browse_cb)(void) = NULL;

void ui_sd_set_browse_cb(void (*cb)(void))
{
    s_browse_cb = cb;
}

static void browse_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_browse_cb != NULL) {
        s_browse_cb();
    }
}

/* LVGL timer callback: probe/mount/unmount the card and refresh the labels.
 * Only runs while the SD page is on screen. */
static void sd_poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (lv_disp_get_scr_act(NULL) != s_scr) {
        return; /* page not visible: no needless SPI traffic */
    }

    if (sd_card_is_mounted()) {
        /* Card mounted: verify it still responds; unmount as soon as it is
         * removed so the "Not Connected" state sticks (no more probing of
         * a half-present card). A later re-insertion is caught by the
         * probe branch below. */
        if (!sd_card_is_present()) {
            sd_card_deinit();
        }
    } else if (sd_card_probe()) {
        /* A card appeared: try to mount it. */
        sd_card_init();
    }

    if (sd_card_is_mounted()) {
        lv_obj_set_style_text_color(status_label, lv_color_hex(0x66BB6A), 0);
        lv_label_set_text(status_label, "Connected");

        /* Card info comes from the driver's in-memory card struct — no SPI
         * traffic here. */
        sdmmc_card_t *card = NULL;
        if (sd_card_get_info(&card) == ESP_OK && card != NULL) {
            uint64_t size_mb = ((uint64_t)card->csd.capacity *
                                card->csd.sector_size) / (1024 * 1024);
            lv_label_set_text_fmt(info_label, "%s | %llu MB",
                                  card->cid.name, (unsigned long long)size_mb);
        } else {
            lv_label_set_text(info_label, "card info unavailable");
        }
    } else {
        lv_obj_set_style_text_color(status_label, lv_color_hex(0xEF5350), 0);
        lv_label_set_text(status_label, "Not Connected");
        lv_label_set_text(info_label, "Insert a microSD card");
    }
}

lv_obj_t *ui_sd_create(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x101418), 0);

    /* The default LVGL theme renders text in gray — set white explicitly
     * so the labels are clearly readable on the dark background. */
    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "SD Card");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    status_label = lv_label_create(s_scr);
    lv_label_set_text(status_label, "Checking...");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, -20);

    info_label = lv_label_create(s_scr);
    lv_label_set_text(info_label, "");
    lv_obj_set_style_text_color(info_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(info_label, LV_ALIGN_CENTER, 0, 20);

    /* Open the file browser (shows "No SD card" inside if none mounted) */
    lv_obj_t *browse_btn = lv_btn_create(s_scr);
    lv_obj_set_size(browse_btn, 120, 36);
    lv_obj_align(browse_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(browse_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(browse_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *browse_label = lv_label_create(browse_btn);
    lv_label_set_text(browse_label, "Browse Files");
    lv_obj_center(browse_label);
    lv_obj_add_event_cb(browse_btn, browse_click_cb, LV_EVENT_CLICKED, NULL);

    /* Poll/probe + refresh once per second (LVGL task) */
    lv_timer_create(sd_poll_timer_cb, SD_POLL_PERIOD_MS, NULL);

    return s_scr;
}
