#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/*============================================================================
 * SD card upload/download server.
 *
 * Runs an HTTP server (esp_http_server) that accepts file uploads into a
 * flat directory on the SD card (/sdcard/esp32_files by default) and can
 * serve the files back for download. Same-name uploads get a numeric
 * suffix automatically (file.txt -> file_1.txt -> file_2.txt ...).
 *
 * SPI constraint (see sd_card.h / lcd_driver.h): the SD card shares the
 * SPI bus with the LCD, so every SD read/write must happen on the LVGL
 * task. The HTTP handlers therefore never touch the SD card directly —
 * they post small commands to an internal mailbox and the LVGL task
 * executes the actual file I/O (upload_server_poll()). The HTTP side
 * waits for an acknowledgement after each command.
 *============================================================================*/

#define UPLOAD_NAME_MAX 60 /* max file name length accepted */

typedef enum {
    UPLOAD_SERVER_IDLE,
    UPLOAD_SERVER_UPLOADING,
    UPLOAD_SERVER_DOWNLOADING,
    UPLOAD_SERVER_BUSY, /* short internal op (directory listing) */
} upload_server_phase_t;

typedef struct {
    bool running;
    uint16_t port;
    upload_server_phase_t phase;
    char filename[UPLOAD_NAME_MAX + 1]; /* current upload/download file */
    uint32_t written;                   /* bytes transferred so far */
    uint32_t total;                     /* expected total (0 if unknown) */
} upload_server_status_t;

/**
 * @brief Start the HTTP server (must be connected to WiFi, card mounted).
 * @return ESP_OK on success.
 */
esp_err_t upload_server_start(void);

/**
 * @brief Stop the HTTP server. If an upload/download is in flight it is
 *        aborted (partial file removed) and httpd_stop() waits for the
 *        handler to finish (bounded by the ack/recv timeouts).
 * @return ESP_OK.
 */
esp_err_t upload_server_stop(void);

bool upload_server_is_running(void);

/**
 * @brief Snapshot of the server state for UI display.
 */
esp_err_t upload_server_get_status(upload_server_status_t *out);

/**
 * @brief Execute pending SD file I/O from the mailbox. MUST be called
 *        periodically from the LVGL task (e.g. a 10 ms LVGL timer) — all
 *        actual SD operations run here to keep the SPI bus serialized.
 */
void upload_server_poll(void);
