#pragma once

#include "esp_err.h"

/**
 * @brief Start the serial file transfer service.
 *
 * Listens on console UART for commands:
 *   WRITE <filename> <size>\r\n  — receive <size> bytes and save as <filename> on SD card
 *
 * Protocol:
 *   PC -> ESP:  WRITE photo.bin 153600\r\n
 *   ESP -> PC:  RDY\r\n
 *   PC -> ESP:  <raw binary data, exactly 153600 bytes>
 *   ESP -> PC:  OK 153600\r\n     (or ERR <code>\r\n)
 *
 * @return ESP_OK on success.
 */
esp_err_t serial_file_init(void);
