#include "serial_file.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "sd_card.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "serial_file";
#define UART_NUM  UART_NUM_0
#define BUF_SIZE  2048

static void send_str(const char *s)
{
    uart_write_bytes(UART_NUM, s, strlen(s));
}

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
        int len = uart_read_bytes(UART_NUM, buf + received,
                                   size - received,
                                   pdMS_TO_TICKS(5000));
        if (len <= 0) {
            ESP_LOGE(TAG, "Timeout after %d/%d bytes", (int)received, (int)size);
            free(buf);
            send_str("ERR TIMEOUT\r\n");
            return;
        }
        received += len;
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
        send_str("ERR WRITE\r\n");
        return;
    }
    char resp[64];
    snprintf(resp, sizeof(resp), "OK %d\r\n", (int)written);
    send_str(resp);
    ESP_LOGI(TAG, "Saved %s (%d bytes)", filename, (int)written);
}

static void serial_file_task(void *arg)
{
    uart_config_t uart_cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_driver_install(UART_NUM, BUF_SIZE, 0, 0, NULL, 0);
    uart_param_config(UART_NUM, &uart_cfg);
    uart_set_pin(UART_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    ESP_LOGI(TAG, "Serial file service started");
    send_str("RDY\r\n");

    char line[256];
    int pos = 0;
    while (1) {
        uint8_t ch;
        if (uart_read_bytes(UART_NUM, &ch, 1, pdMS_TO_TICKS(100)) <= 0)
            continue;
        if (ch == '\r' || ch == '\n') {
            if (pos > 0) {
                line[pos] = '\0';
                pos = 0;
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
        } else if (pos < (int)sizeof(line) - 1) {
            line[pos++] = (char)ch;
        }
    }
}

esp_err_t serial_file_init(void)
{
    xTaskCreate(serial_file_task, "serial_file", 4096, NULL, 1, NULL);
    return ESP_OK;
}
