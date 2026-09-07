#include "sd_card.h"

#include <stdio.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "sd_card";
static sdmmc_card_t *s_card = NULL;

/* The SPI bus (shared with the LCD) and the LCD-CS hold are set up only
 * once, at the first init/probe. Repeated mount attempts must not redo
 * them: spi_bus_initialize() and the LCD_CS GPIO takeover would conflict
 * with the already-running LCD. */
static bool s_bus_ready = false;

/* Once-only SPI bus setup: hold LCD CS high (so SD can be selected during
 * init) and initialize the bus. */
static esp_err_t sd_card_setup_bus(void)
{
    if (s_bus_ready) {
        return ESP_OK;
    }

    /* Hold LCD CS high to deselect LCD during SD init (boot only — the LCD
     * driver later takes over this pin as its hardware CS). */
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

    /* With no card inserted, MISO (GPIO 41) floats and picks up crosstalk
     * from the LCD's 40 MHz SPI clock, producing garbage command responses
     * that spam the log (CMD52 "CRC error", CMD8 "invalid response") and
     * can even fake a "card present". A weak internal pull-up makes an
     * empty slot read a clean 0xFF ("no response") instead. A real card
     * drives MISO push-pull, so the pull-up is harmless when one is
     * inserted. */
    gpio_pullup_en(SD_MISO);

    /* The sdspi_transaction tag logs every rejected command response at
     * INFO level (e.g. CMD52 probing a non-SDIO card). The SDIO reset step
     * tolerates these by design (sdmmc_io_reset), so they are expected
     * noise, not failures — keep them out of the default log. */
    esp_log_level_set("sdspi_transaction", ESP_LOG_ERROR);

    s_bus_ready = true;
    return ESP_OK;
}

esp_err_t sd_card_init(void)
{
    /* Prevent double initialization */
    if (s_card != NULL) {
        ESP_LOGW(TAG, "SD card already mounted");
        return ESP_ERR_INVALID_STATE;
    }

    /* Ensure printf output goes to UART immediately */
    setvbuf(stdout, NULL, _IONBF, 0);

    esp_err_t ret = sd_card_setup_bus();
    if (ret != ESP_OK) {
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

    /* Print card info using ESP-IDF's built-in helper (sdmmc_cmd.h) */
    sdmmc_card_print_info(stdout, s_card);

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

bool sd_card_is_mounted(void)
{
    return s_card != NULL;
}

bool sd_card_is_present(void)
{
    /* No card is mounted: nothing to probe (fast path, no SPI I/O). */
    if (s_card == NULL) {
        return false;
    }

    /* CMD13 (SEND_STATUS) succeeds only while the card responds on the bus.
     * A removed card makes it time out (up to ~1 s) and return an error. */
    return sdmmc_get_status(s_card) == ESP_OK;
}

/* Cheap presence probe: run the card init sequence with a short per-command
 * timeout, so an empty slot fails in ~250 ms instead of the default ~1 s
 * (SDMMC_DEFAULT_CMD_TIMEOUT_MS) per command. A card that completes the
 * init sequence is present. Only public APIs are used; the temporary sdspi
 * device is removed again so the next mount attempt can create its own. */
#define SD_PROBE_TIMEOUT_MS 250

bool sd_card_probe(void)
{
    if (s_card != NULL) {
        return true; /* already mounted */
    }
    if (sd_card_setup_bus() != ESP_OK) {
        return false;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs   = SD_CS;
    slot_config.host_id   = SD_SPI_HOST;
    slot_config.gpio_cd   = GPIO_NUM_NC;
    slot_config.gpio_wp   = GPIO_NUM_NC;

    sdspi_dev_handle_t handle = -1;
    if (sdspi_host_init_device(&slot_config, &handle) != ESP_OK) {
        return false;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = handle;
    host.command_timeout_ms = SD_PROBE_TIMEOUT_MS;

    sdmmc_card_t card = {0};
    esp_err_t ret = sdmmc_card_init(&host, &card);

    sdspi_host_remove_device(handle);
    return ret == ESP_OK;
}
