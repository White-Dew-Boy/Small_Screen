#include "ui_led.h"
#include "lvgl.h"
#include "rgb_led.h"

#include <stdio.h>

/* Preset colors (RGB) offered on the preset sub-page */
typedef struct {
    const char *name;
    uint8_t r, g, b;
} led_color_t;

static const led_color_t s_colors[] = {
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

/* Target selection: one LED or all */
#define SEL_ALL 3
static int s_sel = 0; /* 0..2 = LED index, 3 = all */

/* Per-LED color stored at full scale (0..255) and brightness (0..100).
 * Actual output = color * brightness / 100. */
static uint8_t s_led_r[RGB_LED_NUM];
static uint8_t s_led_g[RGB_LED_NUM];
static uint8_t s_led_b[RGB_LED_NUM];
static int s_bri[RGB_LED_NUM] = {100, 100, 100};

static lv_obj_t *s_led_labels[RGB_LED_NUM]; /* per-LED status lines */
static lv_obj_t *s_led_btns[4];             /* LED1..LED3 + ALL */
static lv_obj_t *s_bri_slider;              /* brightness slider (0..100) */

/* Callbacks to open the sub-pages (set by main.c, LVGL thread) */
static void (*s_preset_cb)(void) = NULL;
static void (*s_custom_cb)(void) = NULL;

void ui_led_set_preset_cb(void (*cb)(void))
{
    s_preset_cb = cb;
}

void ui_led_set_custom_cb(void (*cb)(void))
{
    s_custom_cb = cb;
}

/* ============================ LED state ops ============================ */

/* Stage one LED with its stored color scaled by its own brightness. */
static void apply_led(uint32_t idx)
{
    uint8_t r = (uint8_t)((uint32_t)s_led_r[idx] * s_bri[idx] / 100);
    uint8_t g = (uint8_t)((uint32_t)s_led_g[idx] * s_bri[idx] / 100);
    uint8_t b = (uint8_t)((uint32_t)s_led_b[idx] * s_bri[idx] / 100);
    rgb_led_set_pixel(idx, r, g, b);
}

/* Set one LED's color at full scale and push to the strip. */
static void set_led(uint32_t idx, uint8_t r, uint8_t g, uint8_t b)
{
    s_led_r[idx] = r;
    s_led_g[idx] = g;
    s_led_b[idx] = b;
    apply_led(idx);
    rgb_led_refresh();
}

/* Re-apply the selected target (used after brightness changes). */
static void reapply_target(void)
{
    if (s_sel == SEL_ALL) {
        for (uint32_t i = 0; i < RGB_LED_NUM; i++) {
            apply_led(i);
        }
    } else {
        apply_led((uint32_t)s_sel);
    }
    rgb_led_refresh();
}

int ui_led_get_target(void)
{
    return s_sel;
}

void ui_led_apply_preset(int idx)
{
    if (idx < 0 || idx >= (int)COLOR_COUNT) {
        return;
    }
    const led_color_t *c = &s_colors[idx];
    if (s_sel == SEL_ALL) {
        for (uint32_t i = 0; i < RGB_LED_NUM; i++) {
            set_led(i, c->r, c->g, c->b);
        }
    } else {
        set_led((uint32_t)s_sel, c->r, c->g, c->b);
    }
}

void ui_led_apply_rgb(int r, int g, int b)
{
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;

    if (s_sel == SEL_ALL) {
        for (uint32_t i = 0; i < RGB_LED_NUM; i++) {
            set_led(i, (uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
    } else {
        set_led((uint32_t)s_sel, (uint8_t)r, (uint8_t)g, (uint8_t)b);
    }
}

void ui_led_get_rgb(int *r, int *g, int *b)
{
    int ref = s_sel == SEL_ALL ? 0 : s_sel;
    /* Return the LIVE output color (full-scale color x brightness),
     * matching what the LED actually shows right now. */
    if (r != NULL) *r = (int)((uint32_t)s_led_r[ref] * s_bri[ref] / 100);
    if (g != NULL) *g = (int)((uint32_t)s_led_g[ref] * s_bri[ref] / 100);
    if (b != NULL) *b = (int)((uint32_t)s_led_b[ref] * s_bri[ref] / 100);
}

/* ============================ UI callbacks ============================ */

/* Refresh the per-LED value labels (the "LED1".. name rows above them are
 * static) and the target highlight. */
static void update_ui(void)
{
    for (int i = 0; i < RGB_LED_NUM; i++) {
        lv_label_set_text_fmt(s_led_labels[i], "%d,%d,%d   %d%%",
                              s_led_r[i], s_led_g[i], s_led_b[i],
                              s_bri[i]);
    }

    for (int i = 0; i < 4; i++) {
        if (i == s_sel) {
            lv_obj_set_style_bg_color(s_led_btns[i], lv_color_hex(0x1565C0), 0);
        } else {
            lv_obj_set_style_bg_color(s_led_btns[i], lv_color_hex(0x2A323A), 0);
        }
    }
}

/* Move the slider to the selected target's brightness. */
static void sync_slider(void)
{
    int bri = s_sel == SEL_ALL ? s_bri[0] : s_bri[s_sel];
    lv_slider_set_value(s_bri_slider, bri, LV_ANIM_OFF);
}

static void target_click_cb(lv_event_t *e)
{
    s_sel = (int)(intptr_t)lv_event_get_user_data(e);
    sync_slider();
    update_ui();
}

/* Brightness slider moved: snap to 5% steps, apply to the selected
 * target, and update its status line in real time. */
static void bri_slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(slider);
    int snapped = (v + 2) / 5 * 5; /* round to nearest multiple of 5 */
    if (snapped != v) {
        lv_slider_set_value(slider, snapped, LV_ANIM_OFF);
    }

    if (s_sel == SEL_ALL) {
        for (int i = 0; i < RGB_LED_NUM; i++) {
            s_bri[i] = snapped;
        }
    } else {
        s_bri[s_sel] = snapped;
    }
    reapply_target();
    update_ui();
}

static void preset_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_preset_cb != NULL) {
        s_preset_cb();
    }
}

static void custom_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_custom_cb != NULL) {
        s_custom_cb();
    }
}

/* One-tap: turn off all LEDs. */
static void off_all_click_cb(lv_event_t *e)
{
    (void)e;
    for (uint32_t i = 0; i < RGB_LED_NUM; i++) {
        set_led(i, 0, 0, 0);
    }
    update_ui();
}

/* Periodically refresh the status lines so changes made on the preset /
 * custom sub-pages show up after returning, without needing a slider
 * interaction. */
static void led_refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    update_ui();
}

/* ============================ page build ============================ */

lv_obj_t *ui_led_create(void)
{
    /* Landscape 320x240, like the Home/PC-Perf pages: main.c rotates the
     * whole display to landscape before this screen is loaded.
     * Five stacked rows:
     *   1. LED1..LED3 info side by side (name + RGB/brightness, 12 px)
     *   2. target buttons: LED1 | LED2 | LED3 | ALL
     *   3. brightness slider (as wide as possible)
     *   4. Preset Colors | Custom RGB
     *   5. Turn Off All (full width) */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 240);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "LED Control");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    /* Row 1: per-LED info in three columns (96 px wide each) */
    for (int i = 0; i < RGB_LED_NUM; i++) {
        lv_coord_t x = 8 + i * 104;

        lv_obj_t *name = lv_label_create(scr);
        lv_label_set_text_fmt(name, "LED%d", i + 1);
        lv_obj_set_style_text_color(name, lv_color_hex(0x8AB4F8), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_12, 0);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, x, 24);

        s_led_labels[i] = lv_label_create(scr);
        lv_label_set_text_fmt(s_led_labels[i], "0,0,0   100%%");
        lv_obj_set_style_text_color(s_led_labels[i], lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(s_led_labels[i], &lv_font_montserrat_12, 0);
        lv_obj_align(s_led_labels[i], LV_ALIGN_TOP_LEFT, x, 42);
    }

    /* Row 2: target selection buttons (LED1 | LED2 | LED3 | ALL) */
    const char *targets[4] = { "LED1", "LED2", "LED3", "ALL" };
    for (int i = 0; i < 4; i++) {
        s_led_btns[i] = lv_btn_create(scr);
        lv_obj_set_size(s_led_btns[i], 71, 30);
        lv_obj_align(s_led_btns[i], LV_ALIGN_TOP_LEFT, 8 + i * 77, 62);
        lv_obj_set_style_bg_color(s_led_btns[i], lv_color_hex(0x2A323A), 0);
        lv_obj_set_style_text_color(s_led_btns[i], lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(s_led_btns[i], &lv_font_montserrat_12, 0);
        lv_obj_t *lbl = lv_label_create(s_led_btns[i]);
        lv_label_set_text(lbl, targets[i]);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(s_led_btns[i], target_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }

    /* Row 3: brightness slider (applied to the selected target live).
     * Label sits above the slider; the slider itself spans almost the full
     * width but keeps its right end ~32 px off the screen edge, so the
     * touch panel can still reach the 100 % end reliably. */
    lv_obj_t *bri_label = lv_label_create(scr);
    lv_label_set_text(bri_label, "Brightness");
    lv_obj_set_style_text_color(bri_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_set_style_text_font(bri_label, &lv_font_montserrat_12, 0);
    lv_obj_align(bri_label, LV_ALIGN_TOP_LEFT, 8, 96);

    s_bri_slider = lv_slider_create(scr);
    lv_obj_set_size(s_bri_slider, 280, 16);
    lv_obj_align(s_bri_slider, LV_ALIGN_TOP_LEFT, 8, 112);
    lv_slider_set_range(s_bri_slider, 0, 100); /* 5% steps, 0..100 */
    lv_slider_set_value(s_bri_slider, s_bri[0], LV_ANIM_OFF);
    lv_obj_add_event_cb(s_bri_slider, bri_slider_cb, LV_EVENT_VALUE_CHANGED,
                        NULL);

    /* Row 4: Preset Colors | Custom RGB (side by side) */
    lv_obj_t *preset_btn = lv_btn_create(scr);
    lv_obj_set_size(preset_btn, 148, 36);
    lv_obj_align(preset_btn, LV_ALIGN_TOP_LEFT, 8, 138);
    lv_obj_set_style_bg_color(preset_btn, lv_color_hex(0x1565C0), 0);
    lv_obj_set_style_text_color(preset_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *preset_label = lv_label_create(preset_btn);
    lv_label_set_text(preset_label, "Preset Colors");
    lv_obj_center(preset_label);
    lv_obj_add_event_cb(preset_btn, preset_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *custom_btn = lv_btn_create(scr);
    lv_obj_set_size(custom_btn, 148, 36);
    lv_obj_align(custom_btn, LV_ALIGN_TOP_LEFT, 164, 138);
    lv_obj_set_style_bg_color(custom_btn, lv_color_hex(0x2E7D32), 0);
    lv_obj_set_style_text_color(custom_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *custom_label = lv_label_create(custom_btn);
    lv_label_set_text(custom_label, "Custom RGB");
    lv_obj_center(custom_label);
    lv_obj_add_event_cb(custom_btn, custom_click_cb, LV_EVENT_CLICKED, NULL);

    /* Row 5: one-tap turn off all LEDs (full width) */
    lv_obj_t *off_btn = lv_btn_create(scr);
    lv_obj_set_size(off_btn, 304, 40);
    lv_obj_align(off_btn, LV_ALIGN_TOP_MID, 0, 186);
    lv_obj_set_style_bg_color(off_btn, lv_color_hex(0xC62828), 0);
    lv_obj_set_style_text_color(off_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_t *off_label = lv_label_create(off_btn);
    lv_label_set_text(off_label, "Turn Off All");
    lv_obj_center(off_label);
    lv_obj_add_event_cb(off_btn, off_all_click_cb, LV_EVENT_CLICKED, NULL);

    /* Keep the status lines in sync even when colors change from the
     * sub-pages (preset / custom) */
    lv_timer_create(led_refresh_timer_cb, 500, NULL);

    update_ui();
    return scr;
}
