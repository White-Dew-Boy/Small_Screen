#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "hal/gpio_types.h"

/*============================================================================
 * Pin Configuration
 *
 * MAX98357A digital Class-D amplifier, 3-wire I2S input (no MCLK needed).
 * Board wiring (materials/ESP32模块GPIO连接关系.md):
 *   SPK_BCLK  -> GPIO17
 *   SPK_LRCLK -> GPIO18
 *   SPK_DIN   -> GPIO16
 * The amplifier supply rail (AUDIO_PWR) is switched by GPA0 = GPIO15
 * (power.h POWER_ID_AUDIO) and is OWNED BY THE CALLER: this driver never
 * touches it — call power_on(POWER_ID_AUDIO) before max98357a_init() and
 * power_off(POWER_ID_AUDIO) when the amp is no longer needed.
 *============================================================================*/
#define SPK_BCLK_GPIO   GPIO_NUM_17
#define SPK_LRCLK_GPIO  GPIO_NUM_18
#define SPK_DIN_GPIO    GPIO_NUM_16

/*============================================================================
 * API
 *============================================================================*/
/**
 * @brief Initialize the MAX98357A I2S output.
 *
 * Allocates the I2S TX channel, configures it as I2S master in standard
 * Philips format (16-bit stereo) and enables it. While no data is written
 * the channel outputs silence (auto_clear), so calling this is pop-free.
 *
 * @note Does NOT power the amp supply rail — the caller must call
 *       power_on(POWER_ID_AUDIO) first (or keep the rail permanently on).
 *
 * @param[in] sample_rate_hz  Sample rate in Hz (8k .. 96k supported by chip).
 * @return ESP_OK on success.
 */
esp_err_t max98357a_init(uint32_t sample_rate_hz);

/**
 * @brief Write interleaved 16-bit stereo PCM frames to the amplifier.
 *
 * Blocking; returns once all frames have been copied into the DMA queue.
 * MAX98357A is a mono amp whose exact slot selection is module-dependent,
 * so callers should duplicate the mono signal into L/R (see drv header
 * notes) — that works regardless of which slot the chip samples.
 *
 * @param[in]  pcm     Interleaved L/R int16 samples (little-endian).
 * @param[in]  frames  Number of stereo frames (= L/R sample pairs).
 * @return ESP_OK on success.
 */
esp_err_t max98357a_write(const int16_t *pcm, size_t frames);

/**
 * @brief Stop the amplifier: disable + delete the I2S TX channel.
 * Does NOT power off the supply rail (caller-owned).
 * Safe to call again (returns ESP_ERR_INVALID_STATE if not init).
 * @return ESP_OK on success.
 */
esp_err_t max98357a_deinit(void);
