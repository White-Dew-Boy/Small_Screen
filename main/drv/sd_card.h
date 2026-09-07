#pragma once

#include <stdbool.h>
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

/**
 * @brief Check whether a card is currently mounted (no SPI traffic).
 * @return true if a card is mounted, false otherwise.
 */
bool sd_card_is_mounted(void);

/**
 * @brief Cheap card-presence probe: runs the card init sequence with a
 *        short per-command timeout on a temporary SD device, so an empty
 *        slot fails in ~250 ms instead of the default ~1 s per command.
 * @note  Blocks the calling task for up to ~250 ms when no card is inserted.
 *        Must be called from the LVGL thread (or while the SPI bus is idle)
 *        — see the LCD/sync-flush note in lcd_driver.h.
 * @return true if a card seems present, false otherwise.
 */
bool sd_card_probe(void);

/**
 * @brief Check whether a card is present and responding on the bus.
 *        Returns false immediately when no card is mounted. When mounted,
 *        issues CMD13 (SEND_STATUS); a missing or unresponsive card makes
 *        it return false.
 * @note  Blocks the calling task for up to ~1 s when the card does not
 *        respond (SDMMC_DEFAULT_CMD_TIMEOUT_MS). Must be called from the
 *        LVGL thread (see sd_card_probe).
 * @return true if a card is mounted and responding, false otherwise.
 */
bool sd_card_is_present(void);
