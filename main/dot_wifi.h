#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "dot_config.h"
typedef struct {
    char ssid[33];
    int rssi;
    bool secured;
} dot_wifi_network_t;
/* Empty SSID starts the station driver for on-device network scanning only. */
esp_err_t dot_wifi_start(const dot_config_t *config);
/* Run on a worker. *count supplies capacity 1..8 and receives unique results.
 * Scanning can disconnect Wi-Fi; the saved network is resumed afterward. */
esp_err_t dot_wifi_scan(dot_wifi_network_t *out, size_t *count);
bool dot_wifi_connected(void);
int dot_wifi_rssi(void);
