#include "sd_card.h"

#include <stdio.h>
#include <inttypes.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "sd_card";
static sdmmc_card_t *s_card = NULL;

esp_err_t sd_card_init(void)
{
    /* Prevent double initialization */
    if (s_card != NULL) {
        ESP_LOGW(TAG, "SD card already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Ensure printf output goes to UART immediately */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Hold LCD CS high to deselect LCD during SD init */
    gpio_config_t lcd_cs_cfg = {
        .pin_bit_mask = BIT64(LCD_CS),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&lcd_cs_cfg);
    gpio_set_level(LCD_CS, 1);

    /* Initialize SPI bus (shared with LCD) */
    spi_bus_config_t buscfg = {
        .mosi_io_num     = SD_MOSI,
        .miso_io_num     = SD_MISO,
        .sclk_io_num     = SD_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 240 * 320 * sizeof(uint16_t),
    };

    esp_err_t ret = spi_bus_initialize(SD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Configure SD SPI device */
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs   = SD_CS;
    slot_config.host_id   = SD_SPI_HOST;
    slot_config.gpio_cd   = GPIO_NUM_NC;
    slot_config.gpio_wp   = GPIO_NUM_NC;

    /* Mount FAT filesystem */
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_PATH, &host, &slot_config,
                                   &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed: %s", esp_err_to_name(ret));
        s_card = NULL;
        /* Do NOT free SPI bus — LCD shares it and still needs it */
        return ret;
    }

    /* Custom serial summary */
    uint64_t capacity_mb = ((uint64_t)s_card->csd.capacity *
                            s_card->csd.sector_size) / (1024 * 1024);
    const char *type = (s_card->ocr & 0x40000000) ? "SDHC/SDXC" : "SDSC";
    printf("\n"
           "===== SD Card Info =====\n"
           "  Name:     %s\n"
           "  Type:     %s\n"
           "  Capacity: %" PRIu64 " MB (%d sectors x %d bytes)\n"
           "  Speed:    %d kHz\n"
           "========================\n\n",
           s_card->cid.name, type,
           capacity_mb,
            (int)s_card->csd.capacity, (int)s_card->csd.sector_size,
            (int)s_card->real_freq_khz);
    fflush(stdout);

    ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_PATH);
    return ESP_OK;
}

esp_err_t sd_card_deinit(void)
{
    if (s_card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(SD_MOUNT_PATH, s_card);
    s_card = NULL;
    return ret;
}

esp_err_t sd_card_get_info(sdmmc_card_t **out_card)
{
    if (s_card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    *out_card = s_card;
    return ESP_OK;
}
