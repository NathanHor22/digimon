#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "dot_config.h"
#include "esp_err.h"

/* All custom tunnel runtime executes on the device. start copies config.
 * Call only after Wi-Fi and system time are ready. One instance per tunnel. */
esp_err_t dot_tunnel_start(const dot_config_t *config);
bool dot_tunnel_connected(void);
int dot_tunnel_last_status(void);

/* Protocol helpers also used by the native fixture tests. They make no I/O. */
bool dot_tunnel_parse_timeout_us(const char *value, int64_t *duration_us);
bool dot_tunnel_command_authenticated(const cJSON *command, const char *bearer);
/* private_tunnel_scope is an explicit provisioning choice, valid only for work
 * obtained through our fixed-origin TLS-verified, API-key-authenticated private
 * tunnel poll. It authenticates the tunnel ACL, never an individual user. */
bool dot_tunnel_command_authorized(const cJSON *command, const char *bearer,
    bool private_tunnel_scope);
bool dot_tunnel_protocol_header_matches(const cJSON *command);
int dot_tunnel_rpc_http_status(const cJSON *rpc_response);
/* Takes ownership of rpc_response. Returns an envelope with original opaque
 * request_id/channel. shard_token is intentionally NEVER put in JSON. */
cJSON *dot_tunnel_response_envelope(const cJSON *command, cJSON *rpc_response,
    int http_status, const char *response_type);
