#include "ui_home.h"
#include "lvgl.h"

/* Menu entries: name and accent color */
static const struct {
    const char *name;
    uint32_t color;
} s_entries[] = {
    { "Sensor",     0x1565C0 }, /* blue */
    { "WiFi",       0x1565C0 },
    { "MQTT",       0x2E7D32 }, /* green */
    { "LED",        0xE65100 }, /* orange */
};

/* Callback to open a page (set by main.c, invoked from the LVGL thread) */
static void (*s_page_cb)(int page) = NULL;

void ui_home_set_page_cb(void (*cb)(int page))
{
    s_page_cb = cb;
}

static void entry_click_cb(lv_event_t *e)
{
    int page = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_page_cb != NULL) {
        s_page_cb(page);
    }
}

lv_obj_t *ui_home_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Home");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    /* 2x2 grid of page entries */
    for (int i = 0; i < 4; i++) {
        int col = i % 2;
        int row = i / 2;

        lv_obj_t *btn = lv_btn_create(scr);
        lv_obj_set_size(btn, 104, 74);
        lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 12 + col * 112, 56 + row * 82);
        lv_obj_set_style_bg_color(btn, lv_color_hex(s_entries[i].color), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_radius(btn, 8, 0);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, s_entries[i].name);
        lv_obj_center(lbl);

        lv_obj_add_event_cb(btn, entry_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }

    /* Hint at the bottom */
    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "KEY3: back to Home");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    return scr;
}
