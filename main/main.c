#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lcd_driver.h"
#include "picture1.h"

static const char *TAG = "app_main";

static void gb_swap_buf(uint16_t *pixels, int count)
{
    for (int i = 0; i < count; i++) {
        uint16_t c = pixels[i];
        uint16_t r_out = (c & 0x001F) << 11;
        uint16_t g_out = (c & 0x07E0);
        uint16_t b_out = (c & 0xF800) >> 11;
        pixels[i] = __builtin_bswap16(r_out | g_out | b_out);
    }
}

void app_main(void)
{
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_ERROR_CHECK(lcd_driver_init(&panel_handle, &io_handle));

    int w = 240, h = 320;
    size_t buf_size = w * h * sizeof(uint16_t);
    uint16_t *buf = heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
    assert(buf);

    memcpy(buf, picture1_data, buf_size);
    gb_swap_buf(buf, w * h);
    esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, w, h, buf);
    ESP_LOGI(TAG, "Picture drawn");
    free(buf);
}
