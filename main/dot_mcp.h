#pragma once
#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"
#include "dot_config.h"

esp_err_t dot_mcp_init(const dot_config_t *config);
/* Dispatch one JSON-RPC request. Auth is established by the caller/transport.
 * NULL means a valid notification with no JSON-RPC response. Caller deletes result. */
cJSON *dot_mcp_dispatch(const cJSON *request, bool authenticated);
/* Tool catalog includes get_device_preferences/update_device_preferences.
 * Preferences must be initialized before the transport starts dispatching. */
bool dot_mcp_subscription_ready(void);
esp_err_t dot_mcp_publish_utterance(const char *request_id, const char *text,
    const char *input_kind);
void dot_mcp_subscription_clear(void);
