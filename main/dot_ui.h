#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DOT_UI_NETWORKS_MAX 8
typedef enum { DOT_UI_UP, DOT_UI_DOWN, DOT_UI_SELECT, DOT_UI_BACK } dot_ui_event_t;
typedef struct {
    char ssid[33];
    int rssi;
    bool secured;
} dot_ui_network_t;
typedef struct {
    bool listening;
    bool speaking;
    bool thinking;
    bool muted;
    bool wifi_connected;
    bool configured;
    uint8_t speech_level; /* 0..100, derived from played PCM amplitude */
    uint32_t now_ms;
} dot_ui_snapshot_t;
typedef enum {
    DOT_UI_FACE, DOT_UI_SETTINGS, DOT_UI_WIFI, DOT_UI_SSID,
    DOT_UI_PASSWORD, DOT_UI_VOLUME, DOT_UI_JOINING
} dot_ui_page_t;
typedef struct {
    dot_ui_page_t page;
    unsigned selection;
    unsigned character_index;
    unsigned input_length;
    unsigned input_limit;
    uint8_t volume;
    bool muted;
    bool handsfree;
    bool scanning;
    bool input_full;
} dot_ui_view_t;

/* All model calls belong to one task (normally app_main). Wi-Fi workers must
 * hand scan results to that task before set_networks; no UI function blocks. */
void dot_ui_init(void);
void dot_ui_set_preferences(uint8_t volume, bool muted, bool handsfree);
void dot_ui_settings_open(void);
bool dot_ui_is_open(void);
void dot_ui_event(dot_ui_event_t event);
void dot_ui_render(const dot_ui_snapshot_t *snapshot);
void dot_ui_get_view(dot_ui_view_t *view); /* No password plaintext. */
/* Main consumes scan request, starts an asynchronous scan, then feeds results. */
bool dot_ui_scan_requested(void);
void dot_ui_set_networks(const dot_ui_network_t *networks, size_t count, bool failed);
/* Main saves/joins the submitted network. Password buffer is wiped on take. */
bool dot_ui_take_wifi(char ssid[33], char password[65]);
/* Report a terminal connection result, not every transient disconnected poll. */
void dot_ui_wifi_result(bool connected);
bool dot_ui_take_volume(uint8_t *volume);
bool dot_ui_take_mute(bool *muted);
bool dot_ui_take_handsfree(bool *handsfree);
