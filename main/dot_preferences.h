#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

#define DOT_PREF_NAME_MAX 32
#define DOT_PREF_ALIAS_MAX 32
#define DOT_PREF_ALIASES_MAX 4
#define DOT_PREF_PERSONALITY_MAX 512

typedef struct {
    uint8_t volume_percent;
    bool microphone_muted;
    bool hands_free;
    uint8_t alias_count;
    char device_name[DOT_PREF_NAME_MAX + 1];
    char aliases[DOT_PREF_ALIASES_MAX][DOT_PREF_ALIAS_MAX + 1];
    char personality[DOT_PREF_PERSONALITY_MAX + 1];
    uint32_t revision;
} dot_preferences_t;

esp_err_t dot_preferences_init(void);
/* Thread-safe snapshots. No user preferences are embedded in source files. */
esp_err_t dot_preferences_get(dot_preferences_t *out);
cJSON *dot_preferences_json(void); /* caller owns result */
/* An atomic validated patch; persists only if changed. Remote callers may
 * never change microphone_muted. Only physical UI passes physical_control=true.
 * Aliases replace the array; name/personality empty strings clear only the
 * personality field (name must remain nonempty). */
esp_err_t dot_preferences_update(const cJSON *patch, bool physical_control);
