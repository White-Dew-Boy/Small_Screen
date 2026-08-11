#include "serial_file.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_card.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "serial_file";

static void send_str(const char *s)
{
    fputs(s, stdout);
    fflush(stdout);
}

/*============================================================================
 * Write handler — receives raw binary after sending RDY
 *============================================================================*/
static void handle_write(const char *filename, size_t size)
{
    if (strchr(filename, '/') || strchr(filename, '\\') ||
        filename[0] == '\0' || strlen(filename) > 64) {
        send_str("ERR FORMAT\r\n");
        return;
    }

    char path[128];
    snprintf(path, sizeof(path), "/sdcard/%s", filename);

    uint8_t *buf = malloc(size);
    if (!buf) {
        ESP_LOGE(TAG, "malloc(%d) failed", (int)size);
        send_str("ERR MEM\r\n");
        return;
    }

    send_str("RDY\r\n");
    ESP_LOGI(TAG, "Receiving %s (%d bytes)...", filename, (int)size);

    size_t received = 0;
    while (received < size) {
        size_t chunk = fread(buf + received, 1, size - received, stdin);
        if (chunk == 0) {
            ESP_LOGE(TAG, "EOF after %d/%d bytes", (int)received, (int)size);
            free(buf);
            send_str("ERR TIMEOUT\r\n");
            return;
        }
        received += chunk;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        free(buf);
        send_str("ERR OPEN\r\n");
        return;
    }

    size_t written = fwrite(buf, 1, size, f);
    fclose(f);
    free(buf);

    if (written != size) {
        ESP_LOGE(TAG, "Write incomplete: %d/%d", (int)written, (int)size);
        send_str("ERR WRITE\r\n");
        return;
    }

    char resp[64];
    snprintf(resp, sizeof(resp), "OK %d\r\n", (int)written);
    send_str(resp);
    ESP_LOGI(TAG, "Saved %s (%d bytes)", filename, (int)written);
}

/*============================================================================
 * Background task — reads text commands from stdin (console UART)
 *============================================================================*/
static void serial_file_task(void *arg)
{
    ESP_LOGI(TAG, "Serial file service started");
    send_str("SD card file receiver ready.\r\n");

    while (1) {
        char line[256];
        if (!fgets(line, sizeof(line), stdin)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n'))
            line[--len] = '\0';

        char cmd[16], fname[128];
        int fsize;
        if (sscanf(line, "%15s %127s %d", cmd, fname, &fsize) == 3) {
            if (strcasecmp(cmd, "WRITE") == 0 && fsize > 0) {
                handle_write(fname, fsize);
                continue;
            }
        }
        send_str("ERR FORMAT\r\n");
    }
}

esp_err_t serial_file_init(void)
{
    xTaskCreate(serial_file_task, "serial_file", 4096, NULL, 1, NULL);
    return ESP_OK;
}
