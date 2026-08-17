#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "hal/gpio_types.h"
#include "driver/spi_master.h"
#include "sdmmc_cmd.h"

/*============================================================================
 * Pin Configuration (from materials/ESP32模块GPIO连接关系.md)
 *============================================================================*/
#define SD_CS           GPIO_NUM_1
#define SD_MOSI         GPIO_NUM_38
#define SD_MISO         GPIO_NUM_41
#define SD_SCLK         GPIO_NUM_39
#define LCD_CS          GPIO_NUM_21   /* deselect LCD during SD init */

/*============================================================================
 * SPI / VFS Parameters
 *============================================================================*/
#define SD_SPI_HOST     SPI3_HOST
#define SD_MOUNT_PATH   "/sdcard"

/*============================================================================
 * API
 *============================================================================*/

/**
 * @brief Initialize SPI bus, mount SD card at SD_MOUNT_PATH.
 *        Must be called BEFORE lcd_init() since SD must enter SPI mode first.
 * @return ESP_OK on success, error code on failure (non-fatal - LCD still works).
 */
esp_err_t sd_card_init(void);

/**
 * @brief Unmount SD card and release resources.
 * @return ESP_OK on success.
 */
esp_err_t sd_card_deinit(void);

/**
 * @brief Get the sdmmc_card_t pointer for the mounted card.
 * @param[out] out_card  Pointer to receive the card handle.
 * @return ESP_OK if card is mounted, ESP_ERR_INVALID_STATE otherwise.
 */
esp_err_t sd_card_get_info(sdmmc_card_t **out_card);
