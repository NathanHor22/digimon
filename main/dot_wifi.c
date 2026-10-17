#include "dot_wifi.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static EventGroupHandle_t s_events;
static bool s_started;
static bool s_netif_initialized;
static SemaphoreHandle_t s_radio_lock;
static atomic_bool s_scanning;
static atomic_bool s_has_network;

static void reconnect_once(void)
{
    if (!s_radio_lock || !atomic_load(&s_has_network) || atomic_load(&s_scanning) || dot_wifi_connected()) return;
    /* Serialize the connect call with scan/disconnect. The atomic flag alone
     * would leave a check-to-connect race against scan startup. */
    if (xSemaphoreTake(s_radio_lock, 0) != pdTRUE) return;
    if (atomic_load(&s_has_network) && !atomic_load(&s_scanning) && !dot_wifi_connected())
        (void)esp_wifi_connect();
    xSemaphoreGive(s_radio_lock);
}
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) reconnect_once();
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_DISCONNECTED || id == WIFI_EVENT_STA_STOP))
        xEventGroupClearBits(s_events, 1);
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP && !atomic_load(&s_scanning))
        xEventGroupSetBits(s_events, 1);
    if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP)
        xEventGroupClearBits(s_events, 1);
}
static void reconnect_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        reconnect_once();
    }
}
esp_err_t dot_wifi_start(const dot_config_t *config)
{
    if (!config || !memchr(config->wifi_ssid, 0, sizeof(config->wifi_ssid)) ||
        !memchr(config->wifi_password, 0, sizeof(config->wifi_password))) return ESP_ERR_INVALID_ARG;
    if (s_started) return ESP_ERR_INVALID_STATE;
    /* Keep the event group alive across a failed startup: an already queued
     * event callback can still reference it while the driver is stopping. */
    if (!s_events) s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;
    if (!s_radio_lock) s_radio_lock = xSemaphoreCreateMutex();
    if (!s_radio_lock) return ESP_ERR_NO_MEM;
    atomic_store(&s_scanning, false);
    atomic_store(&s_has_network, config->wifi_ssid[0] != 0);
    xEventGroupClearBits(s_events, 1);
    esp_err_t err;
    if (!s_netif_initialized) {
        err = esp_netif_init();
        if (err != ESP_OK) return err;
        s_netif_initialized = true;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    if (!netif) return ESP_ERR_NO_MEM;
    bool initialized = false, driver_started = false, wifi_handler = false, ip_handler = false;
    wifi_config_t wifi = {0};
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&init)) != ESP_OK) goto fail;
    initialized = true;
    if ((err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL)) != ESP_OK) goto fail;
    wifi_handler = true;
    if ((err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL)) != ESP_OK) goto fail;
    ip_handler = true;
    memcpy(wifi.sta.ssid, config->wifi_ssid, strlen(config->wifi_ssid));
    memcpy(wifi.sta.password, config->wifi_password, strlen(config->wifi_password));
    wifi.sta.pmf_cfg.capable = true;
    wifi.sta.pmf_cfg.required = false;
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) goto fail;
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) goto fail;
    if (atomic_load(&s_has_network) && (err = esp_wifi_set_config(WIFI_IF_STA, &wifi)) != ESP_OK) goto fail;
    memset(&wifi, 0, sizeof(wifi));
    if ((err = esp_wifi_start()) != ESP_OK) goto fail;
    driver_started = true;
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    if (xTaskCreate(reconnect_task, "wifi_retry", 3072, NULL, 3, NULL) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    s_started = true;
    return ESP_OK;
fail:
    atomic_store(&s_has_network, false);
    atomic_store(&s_scanning, false);
    memset(&wifi, 0, sizeof(wifi));
    if (driver_started) (void)esp_wifi_stop();
    if (ip_handler) (void)esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event);
    if (wifi_handler) (void)esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event);
    if (initialized) (void)esp_wifi_deinit();
    esp_netif_destroy_default_wifi(netif);
    xEventGroupClearBits(s_events, 1);
    return err;
}

static int strongest_first(const void *left, const void *right)
{
    const wifi_ap_record_t *a = left, *b = right;
    return (int)b->rssi - (int)a->rssi;
}

esp_err_t dot_wifi_scan(dot_wifi_network_t *out, size_t *count)
{
    if (!count) return ESP_ERR_INVALID_ARG;
    size_t capacity = *count;
    *count = 0;
    if (!out || capacity == 0 || capacity > 8) return ESP_ERR_INVALID_ARG;
    if (!s_started || !s_radio_lock) return ESP_ERR_INVALID_STATE;
    if (atomic_exchange(&s_scanning, true)) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_radio_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    wifi_ap_record_t *records = NULL;
    if (atomic_load(&s_has_network)) {
        err = esp_wifi_disconnect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) goto done;
        xEventGroupClearBits(s_events, 1);
        // Disconnect completion is asynchronous; let a connecting station stop.
        if (err == ESP_OK) vTaskDelay(pdMS_TO_TICKS(150));
    }
    // NULL selects the SDK's documented all-channel active scan defaults:
    // hidden SSIDs excluded and a 120 ms maximum active dwell per channel.
    err = esp_wifi_scan_start(NULL, true);
    if (err == ESP_ERR_WIFI_STATE) {
        // A connection may still be unwinding inside the SDK after disconnect.
        (void)esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
        err = esp_wifi_scan_start(NULL, true);
    }
    if (err != ESP_OK) goto done;
    uint16_t available = 0;
    err = esp_wifi_scan_get_ap_num(&available);
    if (err != ESP_OK || !available) goto done;
    // Bound temporary memory in dense networks. ESP-IDF supplies strongest APs
    // first; sort again for deterministic deduplication and native fixtures.
    if (available > 64) available = 64;
    records = calloc(available, sizeof(*records));
    if (!records) { err = ESP_ERR_NO_MEM; goto done; }
    err = esp_wifi_scan_get_ap_records(&available, records);
    if (err != ESP_OK) goto done;
    qsort(records, available, sizeof(*records), strongest_first);
    for (uint16_t i = 0; i < available && *count < capacity; ++i) {
        char ssid[33] = {0};
        size_t length = 0;
        while (length < 32 && records[i].ssid[length]) ++length;
        if (!length) continue;
        memcpy(ssid, records[i].ssid, length);
        bool duplicate = false;
        for (size_t j = 0; j < *count; ++j)
            if (!strcmp(out[j].ssid, ssid)) { duplicate = true; break; }
        if (duplicate) continue;
        dot_wifi_network_t *network = &out[(*count)++];
        memset(network, 0, sizeof(*network));
        memcpy(network->ssid, ssid, length);
        network->rssi = records[i].rssi;
        network->secured = records[i].authmode != WIFI_AUTH_OPEN;
    }
done:
    free(records);
    // Release the SDK's dynamically allocated AP list on every error path too.
    (void)esp_wifi_clear_ap_list();
    atomic_store(&s_scanning, false);
    xSemaphoreGive(s_radio_lock);
    reconnect_once();
    return err;
}
bool dot_wifi_connected(void)
{ return s_events && (xEventGroupGetBits(s_events) & 1) != 0; }
int dot_wifi_rssi(void)
{
    wifi_ap_record_t ap;
    return dot_wifi_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : -127;
}
