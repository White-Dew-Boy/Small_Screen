#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*============================================================================
 * SD card file-tree access layer.
 *
 * The card is mounted by sd_card.c as a POSIX filesystem at SD_MOUNT_PATH.
 * This module lists directories (one level at a time) for the file-browser
 * UI. Only one directory handle is open at a time and it is closed as soon
 * as the listing finishes: the FAT VFS file table is tiny (max_files in the
 * sd_card mount config), so directory handles must not accumulate.
 *
 * All functions perform blocking SPI traffic and must be called from the
 * LVGL task only (same constraint as sd_card.c — the SD card shares the
 * SPI bus with the LCD).
 *============================================================================*/

#define SD_FILE_NAME_MAX 63

typedef struct {
    char   name[SD_FILE_NAME_MAX + 1];
    bool   is_dir;   /* true: directory, false: regular file */
    uint32_t size;   /* file size in bytes (0 for directories) */
} sd_file_entry_t;

/**
 * @brief List one directory: regular entries only (skips "." and ".."),
 *        sorted with directories first, then alphabetically (case-insensitive).
 * @param path        Directory to list (e.g. SD_MOUNT_PATH).
 * @param entries     Output array, filled with at most max_entries entries.
 * @param max_entries Capacity of the output array.
 * @param out_count   Receives the number of entries written.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if the directory cannot be
 *         opened (card removed, path invalid).
 * @note  Blocks while reading the directory over SPI; call from LVGL task.
 */
esp_err_t sd_file_list(const char *path, sd_file_entry_t *entries,
                       size_t max_entries, size_t *out_count);

/**
 * @brief Join a directory path and an entry name into a full path.
 * @param dir  Parent directory (must not end with '/').
 * @param name Entry name.
 * @param out  Output buffer.
 * @param cap  Output buffer capacity.
 */
void sd_file_join(const char *dir, const char *name, char *out, size_t cap);
