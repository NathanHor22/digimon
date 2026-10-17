#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"
#include "esp_err.h"
#include "dot_config.h"

/* All returned cJSON values are owned by the caller. */
cJSON *dot_controller_status(void);
/* Accepts a correlated reply into a bounded queue; never performs HTTPS. */
esp_err_t dot_controller_accept_reply(const char *request_id, const char *text,
    bool *duplicate);
esp_err_t dot_controller_submit_text(const char *text, const char *input_kind);
void dot_controller_cancel(void);
bool dot_controller_is_pending(const char *request_id);

