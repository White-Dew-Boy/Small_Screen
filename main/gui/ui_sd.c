#include "ui_sd.h"
#include "lvgl.h"
#include "sd_card.h"
#include "upload_server.h"
#include "wifi_manager.h"

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
static lv_obj_t *s_server_label;
static lv_obj_t *s_upload_btn;
static lv_obj_t *s_upload_btn_label;

/* Callbacks to open the file browser / slideshow (set by main.c) */
static void (*s_browse_cb)(void) = NULL;
static void (*s_gallery_cb)(void) = NULL;

void ui_sd_set_browse_cb(void (*cb)(void))
{
    s_browse_cb = cb;
}

void ui_sd_set_gallery_cb(void (*cb)(void))
{
    s_gallery_cb = cb;
}

static void browse_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_browse_cb != NULL) {
        s_browse_cb();
    }
}

static void gallery_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_gallery_cb != NULL) {
        s_gallery_cb();
    }
}

/* Toggle the HTTP upload/download server. Requires WiFi + mounted card;
 * reports the failure reason in the server status label. */
static void upload_click_cb(lv_event_t *e)
{
    (void)e;

    if (upload_server_is_running()) {
        upload_server_stop();
        lv_label_set_text(s_upload_btn_label, "Start Upload");
        lv_label_set_text(s_server_label, "Upload server: off");
        return;
    }

    if (!sd_card_is_mounted()) {
        lv_label_set_text(s_server_label, "No SD card");
        return;
    }
    if (!wifi_manager_is_connected()) {
        lv_label_set_text(s_server_label, "WiFi not connected");
        return;
    }
    if (upload_server_start() == ESP_OK) {
        lv_label_set_text(s_upload_btn_label, "Stop Upload");
    } else {
        lv_label_set_text(s_server_label, "Server start failed");
    }
}

/* Refresh the upload-server status line (IP + port, upload progress).
 * Cheap: reads the driver's in-memory struct, no SPI traffic. */
static void refresh_server_label(void)
{
    upload_server_status_t st;
    upload_server_get_status(&st);

    if (!st.running) {
        lv_obj_set_style_text_color(s_server_label, lv_color_hex(0x8A94A0), 0);
        lv_label_set_text(s_server_label, "Upload server: off");
        return;
    }

    wifi_info_t wifi;
    wifi_manager_get_info(&wifi);
    const char *ip = (wifi.state == WIFI_STATE_CONNECTED) ? wifi.ip : "?.?.?.?";

    if (st.phase == UPLOAD_SERVER_UPLOADING) {
        lv_obj_set_style_text_color(s_server_label, lv_color_hex(0x66BB6A), 0);
        if (st.total > 0) {
            lv_label_set_text_fmt(s_server_label, "UP %s %lu/%lu KB",
                                  st.filename,
                                  (unsigned long)(st.written / 1024),
                                  (unsigned long)(st.total / 1024));
        } else {
            lv_label_set_text_fmt(s_server_label, "UP %s %lu KB",
                                  st.filename,
                                  (unsigned long)(st.written / 1024));
        }
    } else if (st.phase == UPLOAD_SERVER_DOWNLOADING) {
        lv_obj_set_style_text_color(s_server_label, lv_color_hex(0x66BB6A), 0);
        lv_label_set_text_fmt(s_server_label, "DL %s %lu/%lu KB",
                              st.filename,
                              (unsigned long)(st.written / 1024),
                              (unsigned long)(st.total / 1024));
    } else {
        lv_obj_set_style_text_color(s_server_label, lv_color_hex(0x66BB6A), 0);
        lv_label_set_text_fmt(s_server_label, "http://%s:%u/",
                              ip, (unsigned)st.port);
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

    /* Upload server status (in-memory, no SPI traffic) */
    refresh_server_label();
}

/* Fast LVGL timer: executes pending SD file I/O for the upload/download
 * server. Deliberately NOT gated on page visibility — an in-flight upload
 * keeps progressing even if the user switches pages. Runs on the LVGL
 * task, which is the only task allowed to touch the shared SPI bus. */
static void upload_poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    upload_server_poll();
}

lv_obj_t *ui_sd_create(void)
{
    /* Landscape 320x240, like the Home/PC-Perf pages: main.c rotates the
     * whole display to landscape before this screen is loaded. */
    s_scr = lv_obj_create(NULL);
    lv_obj_set_size(s_scr, 320, 240);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x101418), 0);

    /* The default LVGL theme renders text in gray — set white explicitly
     * so the labels are clearly readable on the dark background. */
    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "SD Card");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    status_label = lv_label_create(s_scr);
    lv_label_set_text(status_label, "Checking...");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 30);

    info_label = lv_label_create(s_scr);
    lv_label_set_text(info_label, "");
    lv_obj_set_style_text_color(info_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(info_label, LV_ALIGN_TOP_MID, 0, 54);

    /* Upload server status line (IP/port or transfer progress) */
    s_server_label = lv_label_create(s_scr);
    lv_label_set_text(s_server_label, "Upload server: off");
    lv_obj_set_style_text_color(s_server_label, lv_color_hex(0x8A94A0), 0);
    lv_obj_set_style_text_font(s_server_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_server_label, LV_ALIGN_TOP_MID, 0, 76);

    /* Open the SD picture slideshow (the gallery itself stays portrait;
     * main.c switches back before loading it). */
    lv_obj_t *gallery_btn = lv_btn_create(s_scr);
    lv_obj_set_size(gallery_btn, 296, 42);
    lv_obj_align(gallery_btn, LV_ALIGN_TOP_MID, 0, 104);
    lv_obj_set_style_bg_color(gallery_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(gallery_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *gallery_label = lv_label_create(gallery_btn);
    lv_label_set_text(gallery_label, "Slide Show");
    lv_obj_center(gallery_label);
    lv_obj_add_event_cb(gallery_btn, gallery_click_cb, LV_EVENT_CLICKED, NULL);

    /* Open the file browser (shows "No SD card" inside if none mounted) */
    lv_obj_t *browse_btn = lv_btn_create(s_scr);
    lv_obj_set_size(browse_btn, 144, 44);
    lv_obj_align(browse_btn, LV_ALIGN_TOP_LEFT, 12, 164);
    lv_obj_set_style_bg_color(browse_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(browse_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *browse_label = lv_label_create(browse_btn);
    lv_label_set_text(browse_label, "Browse Files");
    lv_obj_center(browse_label);
    lv_obj_add_event_cb(browse_btn, browse_click_cb, LV_EVENT_CLICKED, NULL);

    /* Toggle the HTTP upload/download server */
    s_upload_btn = lv_btn_create(s_scr);
    lv_obj_set_size(s_upload_btn, 144, 44);
    lv_obj_align(s_upload_btn, LV_ALIGN_TOP_RIGHT, -12, 164);
    lv_obj_set_style_bg_color(s_upload_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(s_upload_btn, lv_color_hex(0xFFFFFF), 0);
    s_upload_btn_label = lv_label_create(s_upload_btn);
    lv_label_set_text(s_upload_btn_label, "Start Upload");
    lv_obj_center(s_upload_btn_label);
    lv_obj_add_event_cb(s_upload_btn, upload_click_cb, LV_EVENT_CLICKED, NULL);

    /* Poll/probe + refresh once per second (LVGL task) */
    lv_timer_create(sd_poll_timer_cb, SD_POLL_PERIOD_MS, NULL);

    /* Fast timer that executes the upload server's SD file I/O on the LVGL
     * task (see upload_server.h). Runs regardless of page visibility. */
    lv_timer_create(upload_poll_timer_cb, 10, NULL);

    return s_scr;
}
