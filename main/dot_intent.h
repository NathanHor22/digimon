#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "dot_config.h"
#include "esp_err.h"

#define DOT_INTENT_CONTEXT_MAX 512
#ifndef DOT_INTENT_MODEL
#define DOT_INTENT_MODEL "gpt-5.6-luna"
#endif
typedef enum {
    DOT_INTENT_UNCERTAIN = 0,
    DOT_INTENT_ADDRESSED,
    DOT_INTENT_AMBIENT
} dot_intent_kind_t;
typedef struct {
    dot_intent_kind_t kind;
    bool valid;
    bool addressed;
    bool ambient;
    bool uncertain;
} dot_intent_decision_t;

/* Worker-only: one bounded Responses API classification, never an assistant
 * answer. context may be NULL; maximum 512 UTF-8 bytes. On any transport,
 * cancellation, refusal, incomplete response or parse error valid stays false.
 * expected_token is a snapshot of dot_audio_cancel_token(). */
esp_err_t dot_intent_classify(const dot_config_t *config, const char *transcript,
    const char *context, bool followup_active, uint32_t expected_token,
    dot_intent_decision_t *decision);
bool dot_intent_should_accept(const dot_intent_decision_t *decision,
    bool followup_active);
