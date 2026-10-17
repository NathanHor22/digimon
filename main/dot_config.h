#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define DOT_DEVICE_ID_MAX 63
#define DOT_REQUEST_ID_MAX 63
#define DOT_TEXT_MAX 2000
#define DOT_KEY_MAX 255
#define DOT_RECORD_SECONDS 15
#define DOT_SAMPLE_RATE 16000
#define DOT_SPEAKER_RATE 24000

typedef struct {
    char device_id[DOT_DEVICE_ID_MAX + 1];
    char wifi_ssid[33];
    char wifi_password[65];
    char audio_api_key[DOT_KEY_MAX + 1];
    char tunnel_api_key[DOT_KEY_MAX + 1];
    char tunnel_id[128];
    char mcp_bearer[DOT_KEY_MAX + 1];
    bool tunnel_scope_auth;
    char stt_model[64];
    char tts_model[64];
    char voice[32];
    int64_t provisioned_epoch;
} dot_config_t;

esp_err_t dot_config_init(dot_config_t *config);
esp_err_t dot_config_save(const dot_config_t *config);
bool dot_config_audio_ready(const dot_config_t *config);
bool dot_config_tunnel_ready(const dot_config_t *config);
bool dot_config_wifi_ready(const dot_config_t *config);
