#include "serial_file.h"

#include "esp_log.h"
#include "driver/uart.h"
#include "sd_card.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static const char *TAG = "serial_file";

#define UART_NUM         UART_NUM_0
#define BUF_SIZE         1024
#define RDY_TIMEOUT_MS   100

static void send_str(const char *s)
{
    uart_write_bytes(UART_NUM, s, strlen(s));
}

/*============================================================================
 * Command parser
 *============================================================================*/
static esp_err_t handle_write(const char *filename, size_t size)
{
    /* Validate filename — no path traversal */
    if (strchr(filename, '/') || strchr(filename, '\\') ||
        filename[0] == '\0' || strlen(filename) > 64) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[128];
    snprintf(path, sizeof(path), "/sdcard/%s", filename);

    /* Allocate receive buffer */
    uint8_t *buf = malloc(size);
    if (!buf) {
        ESP_LOGE(TAG, "malloc(%d) failed", size);
        return ESP_ERR_NO_MEM;
    }

    /* Signal ready */
    send_str("RDY\r\n");
    ESP_LOGI(TAG, "Receiving %s (%d bytes)...", filename, size);

    /* Read exactly <size> bytes from UART */
    size_t received = 0;
    while (received < size) {
        int len = uart_read_bytes(UART_NUM, buf + received,
                                   size - received,
                                   pdMS_TO_TICKS(RDY_TIMEOUT_MS));
        if (len < 0) {
            ESP_LOGE(TAG, "UART read error");
            free(buf);
            return ESP_FAIL;
        }
        if (len == 0) {
            ESP_LOGE(TAG, "Timeout after %d/%d bytes", received, size);
            free(buf);
            send_str("ERR TIMEOUT\r\n");
            return ESP_ERR_TIMEOUT;
        }
        received += len;
    }

    /* Write to SD card */
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s for writing", path);
        free(buf);
        send_str("ERR OPEN\r\n");
        return ESP_FAIL;
    }

    size_t written = fwrite(buf, 1, size, f);
    fclose(f);
    free(buf);

    if (written != size) {
        ESP_LOGE(TAG, "Write incomplete: %d/%d", written, size);
        send_str("ERR WRITE\r\n");
        return ESP_FAIL;
    }

    char resp[64];
    snprintf(resp, sizeof(resp), "OK %d\r\n", (int)written);
    send_str(resp);
    ESP_LOGI(TAG, "Saved %s (%d bytes)", filename, (int)written);
    return ESP_OK;
}

/*============================================================================
 * Background task
 *============================================================================*/
static void serial_file_task(void *arg)
{
    char line_buf[256];
    int line_pos = 0;

    ESP_LOGI(TAG, "Serial file service started");
    send_str("SD card file receiver ready.\r\n");

    while (1) {
        uint8_t ch;
        int len = uart_read_bytes(UART_NUM, &ch, 1,
                                   pdMS_TO_TICKS(100));
        if (len <= 0) continue;

        if (ch == '\r' || ch == '\n') {
            if (line_pos > 0) {
                line_buf[line_pos] = '\0';
                line_pos = 0;

                /* Parse: WRITE <filename> <size> */
                char cmd[16], fname[128];
                int fsize;
                if (sscanf(line_buf, "%15s %127s %d", cmd, fname, &fsize) == 3) {
                    if (strcasecmp(cmd, "WRITE") == 0 && fsize > 0) {
                        handle_write(fname, fsize);
                        continue;
                    }
                }
                send_str("ERR FORMAT\r\n");
            }
        } else if (line_pos < (int)sizeof(line_buf) - 1) {
            line_buf[line_pos++] = (char)ch;
        }
    }
}

/*============================================================================
 * Public API
 *============================================================================*/
esp_err_t serial_file_init(void)
{
    xTaskCreate(serial_file_task, "serial_file", 4096, NULL, 1, NULL);
    return ESP_OK;
}
