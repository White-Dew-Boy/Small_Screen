#pragma once
#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Set the timezone (from Kconfig) and start SNTP time sync on the
 *        first WiFi connection. Must be called after wifi_manager_init()
 *        (it registers an IP_EVENT handler on the default event loop).
 * @return ESP_OK on success.
 */
esp_err_t time_manager_init(void);

/**
 * @brief True once SNTP has completed at least one sync, i.e. time(NULL)
 *        and localtime() return a valid wall-clock time.
 */
bool time_manager_is_synced(void);
