#include "dot_config.h"
#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"

#define CONFIG_VERSION 1U
typedef struct { uint32_t version; dot_config_t config; } stored_config_t;

static bool terminated(const char *s, size_t cap) { return memchr(s, 0, cap) != NULL; }
static void clear_secret(void *memory, size_t size)
{ volatile unsigned char *p = memory; while (size--) *p++ = 0; }
static bool token(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
        if (*p < 33 || *p > 126) return false; /* No whitespace/header injection. */
    return true;
}
static bool identifier(const char *s, bool allow_path)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        bool alnum = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9');
        if (!alnum && *p != '-' && *p != '_' && *p != '.' &&
            !(allow_path && (*p == ':' || *p == '/' || *p == '+'))) return false;
    }
    return true;
}
static bool wifi_password_valid(const char *password)
{
    size_t length = strlen(password);
    if (!length) return true; /* Open Wi-Fi. */
    if (length < 8 || length > 64) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)password[i];
        if (length == 64) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
        } else if (c < 32 || c > 126) return false;
    }
    return true;
}
static bool valid_config(const dot_config_t *c)
{
    if (!c) return false;
#define CHECK(field) if (!terminated(c->field, sizeof(c->field))) return false
    CHECK(device_id); CHECK(wifi_ssid); CHECK(wifi_password); CHECK(audio_api_key);
    CHECK(tunnel_api_key); CHECK(tunnel_id); CHECK(mcp_bearer); CHECK(stt_model);
    CHECK(tts_model); CHECK(voice);
#undef CHECK
    unsigned char scope_flag;
    memcpy(&scope_flag, &c->tunnel_scope_auth, 1);
    if (scope_flag > 1) return false;
    return c->device_id[0] && identifier(c->device_id, false) &&
        identifier(c->tunnel_id, false) && identifier(c->stt_model, true) &&
        identifier(c->tts_model, true) && identifier(c->voice, false) &&
        token(c->audio_api_key) && token(c->tunnel_api_key) && token(c->mcp_bearer) &&
        wifi_password_valid(c->wifi_password) &&
        (c->provisioned_epoch == 0 || (c->provisioned_epoch >= INT64_C(1704067200) &&
                                     c->provisioned_epoch < INT64_C(4102444800)));
}

esp_err_t dot_config_init(dot_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    /* Never erase the existing device's NVS automatically. */
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) return err;
    memset(config, 0, sizeof(*config));
    strcpy(config->device_id, "quipus-original-01");
    strcpy(config->stt_model, "gpt-transcribe");
    strcpy(config->tts_model, "gpt-4o-mini-tts");
    strcpy(config->voice, "coral");
    nvs_handle_t h;
    err = nvs_open("dot_cfg", NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    stored_config_t stored = {0};
    size_t size = sizeof(stored);
    err = nvs_get_blob(h, "config", &stored, &size);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) { clear_secret(&stored, sizeof(stored)); return ESP_OK; }
    if (err != ESP_OK) { clear_secret(&stored, sizeof(stored)); return err; }
    if (size != sizeof(stored) || stored.version != CONFIG_VERSION ||
        !valid_config(&stored.config)) {
        clear_secret(&stored, sizeof(stored)); return ESP_ERR_INVALID_STATE;
    }
    *config = stored.config;
    clear_secret(&stored, sizeof(stored));
    return ESP_OK;
}

esp_err_t dot_config_save(const dot_config_t *config)
{
    if (!config || !valid_config(config)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open("dot_cfg", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    stored_config_t stored = { .version = CONFIG_VERSION, .config = *config };
    err = nvs_set_blob(h, "config", &stored, sizeof(stored));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    clear_secret(&stored, sizeof(stored));
    return err;
}

bool dot_config_audio_ready(const dot_config_t *c)
{ return valid_config(c) && c->audio_api_key[0] && c->stt_model[0] && c->tts_model[0] && c->voice[0]; }
bool dot_config_tunnel_ready(const dot_config_t *c)
{ return valid_config(c) && c->tunnel_api_key[0] && c->tunnel_id[0] && (c->mcp_bearer[0] || c->tunnel_scope_auth); }
bool dot_config_wifi_ready(const dot_config_t *c) { return valid_config(c) && c->wifi_ssid[0]; }
