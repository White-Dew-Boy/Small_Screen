#include "ui_player.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "lvgl.h"

#include "audio_player.h"
#include "sd_card.h"
#include "sd_file.h"

#ifndef CONFIG_UPLOAD_DIR
#define CONFIG_UPLOAD_DIR "/sdcard/esp32_files"
#endif

/* Flat list, no navigation: the player only ever looks in one folder. */
#define PLAYER_MAX_FILES 24

#define ROW_BG          0x1C232B
#define ROW_BG_PLAYING  0x2E5D3A

/* Widgets */
static lv_obj_t *s_scr;
static lv_obj_t *s_list;
static lv_obj_t *s_status_label; /* one line, rewritten only on an event */
static lv_obj_t *s_vol_label;
static lv_obj_t *s_rows[PLAYER_MAX_FILES];

/* File list. Lives in PSRAM: internal RAM is the scarcest resource on this
 * board and only the LVGL task ever touches this array (same trick as
 * ui_files.c). */
static EXT_RAM_BSS_ATTR sd_file_entry_t s_entries[PLAYER_MAX_FILES];
static int s_count;

/* What the screen currently shows, so the state-change check below can skip
 * every redraw that is not needed. */
static audio_player_state_t s_drawn_state = AUDIO_PLAYER_IDLE;
static char s_drawn_file[AUDIO_PLAYER_NAME_MAX + 1];

static void (*s_back_cb)(void) = NULL;

void ui_player_set_back_cb(void (*cb)(void))
{
    s_back_cb = cb;
}

/*============================================================================
 * Helpers
 *============================================================================*/
static bool is_wav(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot != NULL && strcasecmp(dot, ".wav") == 0;
}

static void set_status(const char *text)
{
    lv_label_set_text(s_status_label, text);
}

/* Colour the row of the file that is playing (or none, with keep < 0). */
static void highlight_row(int keep)
{
    for (int i = 0; i < s_count; i++) {
        lv_obj_set_style_bg_color(s_rows[i],
                                  lv_color_hex((i == keep) ? ROW_BG_PLAYING : ROW_BG),
                                  0);
    }
}

static void refresh_vol_label(void)
{
    audio_player_status_t st;
    if (audio_player_get_status(&st) == ESP_OK) {
        lv_label_set_text_fmt(s_vol_label, "Vol %u%%", (unsigned)st.volume);
    }
}

/* Bring the status line and the row highlight in sync with the player. The
 * only caller that can run without a tap is state_check_cb(), and it calls
 * this exclusively when the state really changed. */
static void draw_state(void)
{
    audio_player_status_t st;
    if (audio_player_get_status(&st) != ESP_OK) {
        return;
    }

    const bool playing = (st.state == AUDIO_PLAYER_PLAYING);
    int keep = -1;

    for (int i = 0; i < s_count; i++) {
        if (playing && strcmp(st.file, s_entries[i].name) == 0) {
            keep = i;
            break;
        }
    }
    highlight_row(keep);

    if (st.state == AUDIO_PLAYER_ERROR) {
        lv_label_set_text_fmt(s_status_label, "Error: %s",
                              esp_err_to_name(st.last_error));
    } else if (playing) {
        lv_label_set_text_fmt(s_status_label, "Playing  %s", st.file);
    } else {
        set_status("Stopped");
    }

    s_drawn_state = st.state;
    snprintf(s_drawn_file, sizeof(s_drawn_file), "%s", st.file);
}

/*============================================================================
 * Actions (all driven by a tap — never by a timer)
 *============================================================================*/
static void row_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const intptr_t idx = (intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= s_count) {
        return;
    }

    const char *name = s_entries[idx].name;
    audio_player_status_t st;
    audio_player_get_status(&st);

    if (st.state == AUDIO_PLAYER_PLAYING && strcmp(st.file, name) == 0) {
        /* Tapping the playing file again stops it. The teardown finishes a
         * moment later (player_teardown), so only the label is updated here;
         * state_check_cb() clears the highlight when the state really flips. */
        audio_player_stop();
        highlight_row(-1);
        set_status("Stopped");
        return;
    }

    char path[sizeof(CONFIG_UPLOAD_DIR) + SD_FILE_NAME_MAX + 2];
    snprintf(path, sizeof(path), "%s/%s", CONFIG_UPLOAD_DIR, name);

    const esp_err_t ret = audio_player_play(path);
    if (ret != ESP_OK) {
        lv_label_set_text_fmt(s_status_label, "Cannot play: %s",
                              esp_err_to_name(ret));
        return;
    }
    draw_state(); /* immediate feedback for this tap */
}

static void vol_step(int delta)
{
    audio_player_status_t st;
    if (audio_player_get_status(&st) != ESP_OK) {
        return;
    }

    int vol = (int)st.volume + delta;
    if (vol < 0) {
        vol = 0;
    } else if (vol > 100) {
        vol = 100;
    }

    audio_player_set_volume((uint8_t)vol);
    refresh_vol_label();
    lv_label_set_text_fmt(s_status_label, "Volume %d%%", vol);
}

static void vol_down_cb(lv_event_t *e)
{
    (void)e;
    vol_step(-10);
}

static void vol_up_cb(lv_event_t *e)
{
    (void)e;
    vol_step(10);
}

static void stop_click_cb(lv_event_t *e)
{
    (void)e;
    audio_player_stop();
    highlight_row(-1);
    set_status("Stopped");
}

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_back_cb != NULL) {
        s_back_cb();
    }
}

/* Only redraws when the player state or the track name changed, i.e. once
 * per track start / natural end. While a track plays, this callback touches
 * no widget at all, so the screen stays completely still. */
static void state_check_cb(lv_timer_t *timer)
{
    (void)timer;

    if (lv_disp_get_scr_act(NULL) != s_scr) {
        return; /* page not visible: nothing to do (playback continues) */
    }

    audio_player_status_t st;
    if (audio_player_get_status(&st) != ESP_OK) {
        return;
    }
    if (st.state == s_drawn_state && strcmp(st.file, s_drawn_file) == 0) {
        return;
    }
    draw_state();
}

/*============================================================================
 * List
 *============================================================================*/
static void add_row(const char *name, intptr_t idx)
{
    lv_obj_t *btn = lv_list_add_btn(s_list, LV_SYMBOL_AUDIO, name);
    lv_obj_set_user_data(btn, (void *)idx);
    lv_obj_add_event_cb(btn, row_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_set_style_bg_color(btn, lv_color_hex(ROW_BG), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A323A), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(btn, lv_color_hex(0xE8ECF0), 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_set_style_pad_ver(btn, 8, 0);
    lv_obj_set_height(btn, 36);

    /* Keep long names from wrapping/scrolling: an auto-scrolling label would
     * redraw forever, which is exactly what this page must not do. */
    lv_obj_t *label = lv_obj_get_child(btn, lv_obj_get_child_cnt(btn) - 1);
    if (label != NULL) {
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, 228);
    }

    s_rows[s_count] = btn;
}

void ui_player_refresh(void)
{
    lv_obj_clean(s_list);
    s_count = 0;

    if (!sd_card_is_mounted()) {
        set_status("No SD card");
        return;
    }

    size_t count = 0;
    if (sd_file_list(CONFIG_UPLOAD_DIR, s_entries, PLAYER_MAX_FILES,
                     &count) != ESP_OK) {
        set_status("No " CONFIG_UPLOAD_DIR);
        return;
    }

    for (size_t i = 0; i < count; i++) {
        if (s_entries[i].is_dir || !is_wav(s_entries[i].name)) {
            continue;
        }
        /* Compact in place: the row index must match the array index. */
        if ((size_t)s_count != i) {
            s_entries[s_count] = s_entries[i];
        }
        add_row(s_entries[s_count].name, (intptr_t)s_count);
        s_count++;
    }

    if (s_count == 0) {
        set_status("No .wav files");
        return;
    }

    /* Keep showing the current track if one is already playing. */
    audio_player_status_t st;
    const bool have = (audio_player_get_status(&st) == ESP_OK);
    if (have && st.state == AUDIO_PLAYER_PLAYING) {
        draw_state();
    } else {
        lv_label_set_text_fmt(s_status_label, "%d file(s) - tap to play",
                              s_count);
        highlight_row(-1);
        /* Remember what is on screen so state_check_cb() does not redraw
         * one second later. */
        s_drawn_state = have ? st.state : AUDIO_PLAYER_IDLE;
        snprintf(s_drawn_file, sizeof(s_drawn_file), "%s", have ? st.file : "");
    }
}

/*============================================================================
 * Screen
 *============================================================================*/
lv_obj_t *ui_player_create(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_size(s_scr, 320, 240);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "Audio");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    /* Status line: file being played / result of the last tap. Truncated
     * instead of wrapped so a long name cannot push the layout around. */
    s_status_label = lv_label_create(s_scr);
    lv_label_set_text(s_status_label, "");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x8A94A0), 0);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_status_label, 210);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, 12, 28);

    s_vol_label = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_vol_label, lv_color_hex(0x8A94A0), 0);
    lv_obj_set_style_text_font(s_vol_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_vol_label, LV_ALIGN_TOP_RIGHT, -12, 28);

    /* Flat, scrollable list of the .wav files in CONFIG_UPLOAD_DIR. */
    s_list = lv_list_create(s_scr);
    lv_obj_set_size(s_list, 296, 148);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_radius(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 12, 44);

    /* Bottom row: Vol- / Vol+ / Stop / Back */
    lv_obj_t *vol_down = lv_btn_create(s_scr);
    lv_obj_set_size(vol_down, 56, 34);
    lv_obj_align(vol_down, LV_ALIGN_BOTTOM_LEFT, 12, -8);
    lv_obj_set_style_bg_color(vol_down, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(vol_down, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *vol_down_label = lv_label_create(vol_down);
    lv_label_set_text(vol_down_label, "Vol-");
    lv_obj_center(vol_down_label);
    lv_obj_add_event_cb(vol_down, vol_down_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *vol_up = lv_btn_create(s_scr);
    lv_obj_set_size(vol_up, 56, 34);
    lv_obj_align(vol_up, LV_ALIGN_BOTTOM_LEFT, 72, -8);
    lv_obj_set_style_bg_color(vol_up, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(vol_up, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *vol_up_label = lv_label_create(vol_up);
    lv_label_set_text(vol_up_label, "Vol+");
    lv_obj_center(vol_up_label);
    lv_obj_add_event_cb(vol_up, vol_up_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *stop_btn = lv_btn_create(s_scr);
    lv_obj_set_size(stop_btn, 84, 34);
    lv_obj_align(stop_btn, LV_ALIGN_BOTTOM_LEFT, 132, -8);
    lv_obj_set_style_bg_color(stop_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(stop_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *stop_label = lv_label_create(stop_btn);
    lv_label_set_text(stop_label, "Stop");
    lv_obj_center(stop_label);
    lv_obj_add_event_cb(stop_btn, stop_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_btn = lv_btn_create(s_scr);
    lv_obj_set_size(back_btn, 80, 34);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_RIGHT, -12, -8);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(back_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, back_click_cb, LV_EVENT_CLICKED, NULL);

    refresh_vol_label();

    /* Not a refresh timer: it only compares state and redraws when the track
     * starts or ends by itself. */
    lv_timer_create(state_check_cb, 1000, NULL);

    return s_scr;
}
