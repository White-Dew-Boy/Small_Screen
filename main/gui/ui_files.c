#include "ui_files.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lvgl.h"

#include "sd_card.h"
#include "sd_file.h"

/* File-manager style SD browser. Shows one directory level at a time; the
 * whole tree is never loaded into RAM (SPI reads are slow and would freeze
 * the UI). Navigation is on-demand: tap a folder to enter it, ".." or the
 * Back button to go up. All SD I/O runs on the LVGL task (the SD card
 * shares the SPI bus with the LCD), so refresh() blocks briefly while it
 * reads one directory — acceptable for a single level. */

#define FILE_MAX_ENTRIES 96 /* dir entries cached per level (~7 KB BSS) */

/* Widgets */
static lv_obj_t *s_scr;
static lv_obj_t *s_path_label;
static lv_obj_t *s_list;
static lv_obj_t *s_info_label;
static lv_obj_t *s_delete_btn;

/* Callback to leave the page (set by main.c, invoked from the LVGL thread) */
static void (*s_back_cb)(void) = NULL;

/* Browser state */
static char s_cur_path[256] = SD_MOUNT_PATH; /* current directory */
static sd_file_entry_t s_entries[FILE_MAX_ENTRIES];
static size_t s_entry_count = 0;
static int s_sel = -1; /* selected file index in s_entries, -1 = none */

void ui_files_set_back_cb(void (*cb)(void))
{
    s_back_cb = cb;
}

/* Move one level up in s_cur_path; false when already at the SD root. */
static bool path_go_up(char *path)
{
    if (strcmp(path, SD_MOUNT_PATH) == 0) {
        return false;
    }
    char *slash = strrchr(path, '/');
    if (slash == NULL || slash == path) {
        return false;
    }
    *slash = '\0';
    return true;
}

static void format_size(uint32_t bytes, char *out, size_t cap)
{
    if (bytes < 1024) {
        snprintf(out, cap, "%u B", (unsigned)bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(out, cap, "%.1f KB", bytes / 1024.0);
    } else {
        snprintf(out, cap, "%.1f MB", bytes / (1024.0 * 1024.0));
    }
}

static void item_click_cb(lv_event_t *e);

/* Build one list row. idx is the index into s_entries; -1 is the virtual
 * ".." (up one level) row. */
static void add_item(const char *icon, const char *name, intptr_t idx)
{
    lv_obj_t *btn = lv_list_add_btn(s_list, icon, name);
    lv_obj_set_user_data(btn, (void *)idx);
    lv_obj_add_event_cb(btn, item_click_cb, LV_EVENT_CLICKED, NULL);

    /* Dark theme styling */
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1C232B), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A323A), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(btn, lv_color_hex(0xE8ECF0), 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_set_style_pad_ver(btn, 8, 0);
    lv_obj_set_height(btn, 36);
}

static void item_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    intptr_t idx = (intptr_t)lv_obj_get_user_data(btn);

    if (idx < 0) {
        /* Virtual ".." row: one level up */
        if (path_go_up(s_cur_path)) {
            ui_files_refresh();
        }
        return;
    }
    if ((size_t)idx >= s_entry_count) {
        return;
    }

    const sd_file_entry_t *ent = &s_entries[idx];

    if (ent->is_dir) {
        sd_file_join(s_cur_path, ent->name, s_cur_path, sizeof(s_cur_path));
        ui_files_refresh(); /* resets the selection */
        return;
    }

    /* Regular file: select it (enables Delete) and show name + size */
    s_sel = (int)idx;
    lv_obj_clear_state(s_delete_btn, LV_STATE_DISABLED);
    char size_str[16];
    format_size(ent->size, size_str, sizeof(size_str));
    lv_label_set_text_fmt(s_info_label, "%s  (%s)", ent->name, size_str);
}

/* Delete the selected file (called on LVGL task — unlink is SD I/O). */
static void do_delete_selected(void)
{
    if (s_sel < 0 || (size_t)s_sel >= s_entry_count) {
        return;
    }
    const char *name = s_entries[s_sel].name;

    char path[256 + SD_FILE_NAME_MAX + 2];
    snprintf(path, sizeof(path), "%s/%s", s_cur_path, name);

    bool ok = (unlink(path) == 0);
    ui_files_refresh(); /* rebuild the list (also clears the selection) */
    lv_label_set_text_fmt(s_info_label, ok ? "Deleted %s" : "Delete failed: %s",
                          name);
}

/* Message-box event: button 0 = Delete, 1 = Cancel. */
static void delete_mbox_cb(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_current_target(e);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        if (lv_msgbox_get_active_btn(mbox) == 0) {
            do_delete_selected();
        }
        lv_msgbox_close(mbox);
    }
}

static void delete_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_sel < 0 || (size_t)s_sel >= s_entry_count) {
        return;
    }

    static const char *btns[] = { "Delete", "Cancel", "" };
    lv_obj_t *mbox = lv_msgbox_create(lv_scr_act(),
                                      "Delete file", s_entries[s_sel].name,
                                      btns, false);
    /* Dark styling to match the app */
    lv_obj_set_style_bg_color(mbox, lv_color_hex(0x1C232B), 0);
    lv_obj_set_style_text_color(mbox, lv_color_hex(0xE8ECF0), 0);
    lv_obj_add_event_cb(mbox, delete_mbox_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(mbox);
}

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_back_cb != NULL) {
        s_back_cb();
    }
}

void ui_files_refresh(void)
{
    lv_obj_clean(s_list);
    s_entry_count = 0;
    s_sel = -1; /* the list is rebuilt; the old selection is invalid */
    lv_obj_add_state(s_delete_btn, LV_STATE_DISABLED);

    if (!sd_card_is_mounted()) {
        lv_label_set_text(s_path_label, SD_MOUNT_PATH);
        lv_obj_t *note = lv_list_add_text(s_list, "No SD card mounted");
        lv_obj_set_style_text_color(note, lv_color_hex(0x8A94A0), 0);
        lv_label_set_text(s_info_label, "Insert a microSD card");
        return;
    }

    lv_label_set_text(s_path_label, s_cur_path);

    /* ".." row (go up) — only when not already at the SD root */
    if (strcmp(s_cur_path, SD_MOUNT_PATH) != 0) {
        add_item(LV_SYMBOL_LEFT, "..", -1);
    }

    size_t count = 0;
    if (sd_file_list(s_cur_path, s_entries, FILE_MAX_ENTRIES, &count) != ESP_OK) {
        lv_obj_t *note = lv_list_add_text(s_list, "Cannot read directory");
        lv_obj_set_style_text_color(note, lv_color_hex(0xEF5350), 0);
        lv_label_set_text(s_info_label, "Card may have been removed");
        return;
    }
    s_entry_count = count;

    for (size_t i = 0; i < s_entry_count; i++) {
        const sd_file_entry_t *e = &s_entries[i];
        const char *icon = e->is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE;
        add_item(icon, e->name, (intptr_t)i);
    }

    lv_label_set_text_fmt(s_info_label, "%d item(s)", (int)s_entry_count);
}

lv_obj_t *ui_files_create(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "SD Files");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    /* Current directory, truncated with "..." when too long */
    s_path_label = lv_label_create(s_scr);
    lv_label_set_text(s_path_label, SD_MOUNT_PATH);
    lv_obj_set_style_text_color(s_path_label, lv_color_hex(0x8A94A0), 0);
    lv_obj_set_style_text_font(s_path_label, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(s_path_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_path_label, 216);
    lv_obj_align(s_path_label, LV_ALIGN_TOP_LEFT, 12, 28);

    /* Scrollable file list */
    s_list = lv_list_create(s_scr);
    lv_obj_set_size(s_list, 228, 210);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_radius(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 6, 46);

    /* Info bar (item count / selected file details) */
    s_info_label = lv_label_create(s_scr);
    lv_label_set_text(s_info_label, "");
    lv_obj_set_style_text_color(s_info_label, lv_color_hex(0x8A94A0), 0);
    lv_obj_set_style_text_font(s_info_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_info_label, LV_ALIGN_TOP_MID, 0, 258);

    /* Delete (disabled until a file is selected) + Back buttons */
    s_delete_btn = lv_btn_create(s_scr);
    lv_obj_set_size(s_delete_btn, 90, 32);
    lv_obj_align(s_delete_btn, LV_ALIGN_BOTTOM_LEFT, 10, -8);
    lv_obj_set_style_bg_color(s_delete_btn, lv_color_hex(0x7A3238), 0);
    lv_obj_set_style_bg_color(s_delete_btn, lv_color_hex(0x2A323A),
                              LV_STATE_DISABLED);
    lv_obj_set_style_text_color(s_delete_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *delete_label = lv_label_create(s_delete_btn);
    lv_label_set_text(delete_label, "Delete");
    lv_obj_center(delete_label);
    lv_obj_add_event_cb(s_delete_btn, delete_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(s_delete_btn, LV_STATE_DISABLED);

    lv_obj_t *back_btn = lv_btn_create(s_scr);
    lv_obj_set_size(back_btn, 90, 32);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_RIGHT, -10, -8);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(back_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, back_click_cb, LV_EVENT_CLICKED, NULL);

    return s_scr;
}
