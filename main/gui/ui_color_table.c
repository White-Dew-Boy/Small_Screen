#include "ui_color_table.h"
#include "lvgl.h"

typedef struct {
    const char *name;
    uint32_t hex;
} color_entry_t;

static const color_entry_t s_colors[] = {
    {"Red",        0xFF0000},
    {"Orange",     0xFFA500},
    {"Gold",       0xFFD700},
    {"Yellow",     0xFFFF00},
    {"Lime",       0x00FF00},
    {"Green",      0x008000},
    {"Cyan",       0x00FFFF},
    {"SkyBlue",    0x87CEEB},
    {"Blue",       0x0000FF},
    {"Navy",       0x000080},
    {"Purple",     0x800080},
    {"Violet",     0xEE82EE},
    {"Magenta",    0xFF00FF},
    {"Pink",       0xFFC0CB},
    {"Brown",      0xA52A2A},
    {"Chocolate",  0xD2691E},
    {"Silver",     0xC0C0C0},
    {"Gray",       0x808080},
    {"White",      0xFFFFFF},
    {"Black",      0x000000},
};

#define COLOR_NUM (sizeof(s_colors) / sizeof(s_colors[0]))

/* Bottom info bar: shows the name + hex code of the last tapped swatch */
static lv_obj_t *s_info_label;

static void swatch_click_cb(lv_event_t *e)
{
    lv_obj_t *swatch = lv_event_get_target(e);
    const color_entry_t *entry = lv_obj_get_user_data(swatch);
    if (entry == NULL) {
        return;
    }

    lv_label_set_text_fmt(s_info_label, "%s  #%06X", entry->name, (unsigned int)entry->hex);
    lv_obj_set_style_bg_color(s_info_label, lv_color_hex(entry->hex), 0);

    /* Pick a readable text color from the perceived luminance */
    uint8_t r = (entry->hex >> 16) & 0xFF;
    uint8_t g = (entry->hex >> 8) & 0xFF;
    uint8_t b = entry->hex & 0xFF;
    uint32_t lum = (299 * r + 587 * g + 114 * b) / 1000;
    lv_obj_set_style_text_color(
        s_info_label,
        lum > 128 ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF), 0);
}

lv_obj_t *ui_color_table_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Color Table");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

    /* 4x5 wrap grid of color swatches */
    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_set_size(cont, 214, LV_SIZE_CONTENT);
    lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(cont, 6, 0);
    lv_obj_set_style_pad_row(cont, 6, 0);

    for (size_t i = 0; i < COLOR_NUM; i++) {
        lv_obj_t *swatch = lv_obj_create(cont);
        lv_obj_set_size(swatch, 44, 44);
        lv_obj_set_style_bg_color(swatch, lv_color_hex(s_colors[i].hex), 0);
        lv_obj_set_style_radius(swatch, 8, 0);
        lv_obj_set_style_border_width(swatch, 1, 0);
        lv_obj_set_style_border_color(swatch, lv_color_hex(0x3A4149), 0);
        lv_obj_set_user_data(swatch, (void *)&s_colors[i]);
        lv_obj_add_event_cb(swatch, swatch_click_cb, LV_EVENT_CLICKED, NULL);
    }

    /* Bottom info bar */
    s_info_label = lv_label_create(scr);
    lv_label_set_text(s_info_label, "Tap a color");
    lv_obj_set_style_bg_color(s_info_label, lv_color_hex(0x1C2228), 0);
    lv_obj_set_style_bg_opa(s_info_label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_info_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_all(s_info_label, 6, 0);
    lv_obj_set_style_text_align(s_info_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_info_label, LV_ALIGN_BOTTOM_MID, 0, -4);

    return scr;
}
