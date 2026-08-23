#include "ui_led_preset.h"
#include "lvgl.h"
#include "ui_led.h"

/* Preset colors, matching the table in ui_led.c */
static const struct {
    const char *name;
    uint8_t r, g, b;
} s_colors[] = {
    { "Red",     255, 0,   0   },
    { "Green",   0,   255, 0   },
    { "Blue",    0,   0,   255 },
    { "Yellow",  255, 255, 0   },
    { "Cyan",    0,   255, 255 },
    { "Magenta", 255, 0,   255 },
    { "White",   255, 255, 255 },
    { "Off",     0,   0,   0   },
};
#define COLOR_COUNT (sizeof(s_colors) / sizeof(s_colors[0]))

/* Callback to leave the page (set by main.c, invoked from the LVGL thread) */
static void (*s_back_cb)(void) = NULL;

void ui_led_preset_set_back_cb(void (*cb)(void))
{
    s_back_cb = cb;
}

/* A color was picked: apply it to the target on the LED page and leave. */
static void color_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= (int)COLOR_COUNT) {
        return;
    }
    ui_led_apply_preset(idx);
    if (s_back_cb != NULL) {
        s_back_cb();
    }
}

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_back_cb != NULL) {
        s_back_cb();
    }
}

lv_obj_t *ui_led_preset_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Preset Colors");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    /* Color buttons: 4 columns x 2 rows */
    for (int i = 0; i < (int)COLOR_COUNT; i++) {
        int col = i % 4;
        int row = i / 4;
        lv_obj_t *btn = lv_btn_create(scr);
        lv_obj_set_size(btn, 52, 52);
        lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 10 + col * 58, 50 + row * 60);
        lv_obj_set_style_bg_color(btn, lv_color_make(s_colors[i].r,
                                                     s_colors[i].g,
                                                     s_colors[i].b), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), 0);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, s_colors[i].name);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn, color_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }

    /* Back button */
    lv_obj_t *back_btn = lv_btn_create(scr);
    lv_obj_set_size(back_btn, 100, 36);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x2A323A), 0);
    lv_obj_set_style_text_color(back_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, back_click_cb, LV_EVENT_CLICKED, NULL);

    return scr;
}
