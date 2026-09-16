#include "max98357a.h"

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "max98357a";

static i2s_chan_handle_t s_tx_handle;
static bool s_started;

esp_err_t max98357a_init(uint32_t sample_rate_hz)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    /* NOTE: this driver deliberately does NOT touch the amp supply rail
     * (AUDIO_PWR / GPIO15) — the caller owns power management and must have
     * called power_on(POWER_ID_AUDIO) before max98357a_init(). */

    /* DMA kept small: buffers come from internal RAM (no PSRAM on this
     * board). 4 desc x 240 frames x 4 B/frame ~ 3.8 KB total. */
    i2s_chan_config_t chan_cfg = {
        .id            = I2S_NUM_0,
        .role          = I2S_ROLE_MASTER,
        .dma_desc_num  = 4,
        .dma_frame_num = 240,
        .auto_clear    = true, /* output silence when no data queued */
    };
    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel alloc failed: %s", esp_err_to_name(ret));
        s_tx_handle = NULL;
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz),
        /* Philips (standard I2S), 16-bit stereo: caller duplicates mono. */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_NC, /* MAX98357A derives clocks from BCLK */
            .bclk = SPK_BCLK_GPIO,
            .ws   = SPK_LRCLK_GPIO,
            .dout = SPK_DIN_GPIO,
            .din  = GPIO_NUM_NC,
        },
    };
    ret = i2s_channel_init_std_mode(s_tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S std mode init failed: %s", esp_err_to_name(ret));
        goto del_channel;
    }

    ret = i2s_channel_enable(s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel enable failed: %s", esp_err_to_name(ret));
        goto del_channel;
    }

    s_started = true;
    ESP_LOGI(TAG, "MAX98357A started @ %lu Hz (BCLK=%d, WS=%d, DIN=%d)",
             (unsigned long)sample_rate_hz, SPK_BCLK_GPIO, SPK_LRCLK_GPIO,
             SPK_DIN_GPIO);
    return ESP_OK;

del_channel:
    i2s_del_channel(s_tx_handle);
    s_tx_handle = NULL;
    return ret;
}

esp_err_t max98357a_write(const int16_t *pcm, size_t frames)
{
    if (pcm == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t bytes_written = 0;
    esp_err_t ret = i2s_channel_write(s_tx_handle, pcm,
                                      frames * 2 * sizeof(int16_t),
                                      &bytes_written, portMAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S write failed (%u/%u bytes sent): %s",
                 (unsigned)bytes_written,
                 (unsigned)(frames * 2 * sizeof(int16_t)),
                 esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t max98357a_deinit(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = i2s_channel_disable(s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel disable failed: %s", esp_err_to_name(ret));
    }

    ret = i2s_del_channel(s_tx_handle);
    s_tx_handle = NULL;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel delete failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Supply rail is owned by the caller — deinit only stops I2S. */
    s_started = false;
    ESP_LOGI(TAG, "MAX98357A stopped");
    return ESP_OK;
}
