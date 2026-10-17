#include "dot_preferences.h"
#include <math.h>
#include <stddef.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define PREFERENCES_VERSION 1U
typedef struct { uint32_t version; dot_preferences_t value; } stored_preferences_t;
static SemaphoreHandle_t s_preferences_lock;
static dot_preferences_t s_preferences;
static bool s_preferences_initialized;

static void defaults(dot_preferences_t *value)
{
    memset(value, 0, sizeof(*value));
    value->volume_percent = 60;
    value->hands_free = true; /* Explicitly selected for this device build. */
    strcpy(value->device_name, "Digimon");
    strcpy(value->aliases[0], "mommy");
    value->alias_count = 1;
    value->revision = 1;
}

static bool text_valid(const char *value, size_t max, bool empty)
{
    if (!value) return false;
    size_t length = 0;
    while (length <= max && value[length]) ++length;
    if ((!empty && !length) || length > max) return false;
    /* Validate UTF-8, excluding control bytes and overlong/surrogate encodings. */
    for (size_t i = 0; i < length;) {
        unsigned char a = (unsigned char)value[i++];
        if (a < 32 || a == 127) return false;
        if (a < 128) continue;
        unsigned count; uint32_t code;
        if (a >= 0xc2 && a <= 0xdf) { count = 1; code = a & 0x1f; }
        else if (a >= 0xe0 && a <= 0xef) { count = 2; code = a & 0x0f; }
        else if (a >= 0xf0 && a <= 0xf4) { count = 3; code = a & 7; }
        else return false;
        if (count > length - i) return false;
        for (unsigned j = 0; j < count; ++j) {
            unsigned char next = (unsigned char)value[i++];
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if ((count == 1 && code < 0x80) || (count == 2 && code < 0x800) ||
            (count == 3 && code < 0x10000) || code > 0x10ffff ||
            (code >= 0xd800 && code <= 0xdfff) || (code >= 0x80 && code <= 0x9f)) return false;
    }
    if (!empty && length && (value[0] == ' ' || value[length - 1] == ' ')) return false;
    return true;
}

static bool ascii_equal_folded(const char *a, const char *b)
{
    while (*a && *b) {
        unsigned char ac = (unsigned char)*a++, bc = (unsigned char)*b++;
        if (ac >= 'A' && ac <= 'Z') ac = (unsigned char)(ac + 32);
        if (bc >= 'A' && bc <= 'Z') bc = (unsigned char)(bc + 32);
        if (ac != bc) return false;
    }
    return *a == *b;
}

static bool valid_preferences(const dot_preferences_t *value)
{
    unsigned char muted, handsfree;
    memcpy(&muted, &value->microphone_muted, 1);
    memcpy(&handsfree, &value->hands_free, 1);
    if (muted > 1 || handsfree > 1 || value->volume_percent > 100 ||
        value->alias_count > DOT_PREF_ALIASES_MAX || !value->revision ||
        !text_valid(value->device_name, DOT_PREF_NAME_MAX, false) ||
        !text_valid(value->personality, DOT_PREF_PERSONALITY_MAX, true)) return false;
    for (unsigned i = 0; i < DOT_PREF_ALIASES_MAX; ++i) {
        if (!text_valid(value->aliases[i], DOT_PREF_ALIAS_MAX, i >= value->alias_count)) return false;
        if (i >= value->alias_count && value->aliases[i][0]) return false;
        for (unsigned j = 0; j < i; ++j)
            if (i < value->alias_count && ascii_equal_folded(value->aliases[i], value->aliases[j])) return false;
    }
    return true;
}

esp_err_t dot_preferences_init(void)
{
    if (!s_preferences_lock) s_preferences_lock = xSemaphoreCreateMutex();
    if (!s_preferences_lock) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_preferences_lock, portMAX_DELAY);
    defaults(&s_preferences);
    nvs_handle_t handle;
    esp_err_t err = nvs_open("dot_prefs", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        stored_preferences_t stored = {0};
        size_t length = sizeof(stored);
        err = nvs_get_blob(handle, "profile", &stored, &length);
        nvs_close(handle);
        if (err == ESP_OK) {
            if (length == sizeof(stored) && stored.version == PREFERENCES_VERSION && valid_preferences(&stored.value))
                s_preferences = stored.value;
            else err = ESP_ERR_INVALID_STATE;
        }
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err != ESP_OK) {
        /* A damaged privacy preference never silently enables recording. */
        s_preferences.microphone_muted = true;
        s_preferences.hands_free = false;
    }
    s_preferences_initialized = true;
    xSemaphoreGive(s_preferences_lock);
    return err;
}

esp_err_t dot_preferences_get(dot_preferences_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_preferences_lock || !s_preferences_initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_preferences_lock, portMAX_DELAY);
    *out = s_preferences;
    xSemaphoreGive(s_preferences_lock);
    return ESP_OK;
}

cJSON *dot_preferences_json(void)
{
    dot_preferences_t value;
    if (dot_preferences_get(&value) != ESP_OK) return NULL;
    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;
    cJSON_AddNumberToObject(json, "volume_percent", value.volume_percent);
    cJSON_AddBoolToObject(json, "microphone_muted", value.microphone_muted);
    cJSON_AddBoolToObject(json, "hands_free", value.hands_free);
    cJSON_AddStringToObject(json, "device_name", value.device_name);
    cJSON_AddStringToObject(json, "personality", value.personality);
    cJSON_AddNumberToObject(json, "revision", value.revision);
    cJSON_AddBoolToObject(json, "microphone_control_is_physical_only", true);
    cJSON *aliases = cJSON_AddArrayToObject(json, "aliases");
    for (unsigned i = 0; i < value.alias_count; ++i)
        cJSON_AddItemToArray(aliases, cJSON_CreateString(value.aliases[i]));
    return json;
}

esp_err_t dot_preferences_update(const cJSON *patch, bool physical_control)
{
    if (!cJSON_IsObject(patch)) return ESP_ERR_INVALID_ARG;
    if (!s_preferences_lock || !s_preferences_initialized) return ESP_ERR_INVALID_STATE;
    /* Validate the entire patch before touching live state or writing flash. */
    static const char *const allowed[] = {"volume_percent", "microphone_muted", "hands_free", "device_name", "aliases", "personality"};
    const cJSON *field;
    cJSON_ArrayForEach(field, patch) {
        bool known = false;
        for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i)
            if (field->string && !strcmp(field->string, allowed[i])) known = true;
        if (!known) return ESP_ERR_INVALID_ARG;
        for (const cJSON *next = field->next; next; next = next->next)
            if (next->string && !strcmp(field->string, next->string)) return ESP_ERR_INVALID_ARG;
        if (!strcmp(field->string, "microphone_muted") && !physical_control) return ESP_ERR_NOT_SUPPORTED;
    }
    xSemaphoreTake(s_preferences_lock, portMAX_DELAY);
    dot_preferences_t value = s_preferences;
    esp_err_t err = ESP_OK;
    cJSON_ArrayForEach(field, patch) {
        const char *name = field->string;
        if (!strcmp(name, "volume_percent")) {
            if (!cJSON_IsNumber(field) || !isfinite(field->valuedouble) || field->valuedouble < 0 ||
                field->valuedouble > 100 || floor(field->valuedouble) != field->valuedouble) { err = ESP_ERR_INVALID_ARG; break; }
            value.volume_percent = (uint8_t)field->valuedouble;
        } else if (!strcmp(name, "microphone_muted") || !strcmp(name, "hands_free")) {
            if (!cJSON_IsBool(field)) { err = ESP_ERR_INVALID_ARG; break; }
            if (!strcmp(name, "microphone_muted")) value.microphone_muted = cJSON_IsTrue(field);
            else value.hands_free = cJSON_IsTrue(field);
        } else if (!strcmp(name, "aliases")) {
            if (!cJSON_IsArray(field) || cJSON_GetArraySize(field) > DOT_PREF_ALIASES_MAX) { err = ESP_ERR_INVALID_ARG; break; }
            memset(value.aliases, 0, sizeof(value.aliases)); value.alias_count = 0;
            const cJSON *alias;
            cJSON_ArrayForEach(alias, field) {
                if (!cJSON_IsString(alias) || !text_valid(alias->valuestring, DOT_PREF_ALIAS_MAX, false)) { err = ESP_ERR_INVALID_ARG; break; }
                strcpy(value.aliases[value.alias_count++], alias->valuestring);
            }
            if (err != ESP_OK) break;
        } else {
            size_t max = !strcmp(name, "personality") ? DOT_PREF_PERSONALITY_MAX : DOT_PREF_NAME_MAX;
            if (!cJSON_IsString(field) || !text_valid(field->valuestring, max, !strcmp(name, "personality"))) { err = ESP_ERR_INVALID_ARG; break; }
            char *destination = !strcmp(name, "personality") ? value.personality : value.device_name;
            memset(destination, 0, max + 1); strcpy(destination, field->valuestring);
        }
    }
    if (err == ESP_OK && !valid_preferences(&value)) err = ESP_ERR_INVALID_ARG;
    if (err == ESP_OK && memcmp(&value, &s_preferences, sizeof(value))) {
        value.revision = value.revision == UINT32_MAX ? 1 : value.revision + 1;
        stored_preferences_t stored = { .version = PREFERENCES_VERSION, .value = value };
        nvs_handle_t handle;
        err = nvs_open("dot_prefs", NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            err = nvs_set_blob(handle, "profile", &stored, sizeof(stored));
            if (err == ESP_OK) err = nvs_commit(handle);
            nvs_close(handle);
        }
        if (err == ESP_OK) s_preferences = value;
    }
    xSemaphoreGive(s_preferences_lock);
    return err;
}
