/* ESP-IDF client of OpenAI's documented, language-neutral tunnel protocol.
 * Contract inspected: openai/tunnel-client docs/protocol.md + docs/openapi.json
 * pinned commit 3e52fa0400c9126dbc92c6b764b4e712506927cf, 2026-10-17.
 * Source snapshots and SHA256 are in protocol/.
 * Canonical API: https://api.openai.com/v1/tunnels/{opaque-id}/{poll,response}
 * This is an independent constrained implementation, not an official SDK.
 */
#include "dot_tunnel.h"
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _MSC_VER
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

bool dot_tunnel_parse_timeout_us(const char *value, int64_t *duration_us)
{
    if (!value || !duration_us || *value < '0' || *value > '9') return false;
    uint64_t number = 0;
    const char *p = value;
    while (*p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p++ - '0');
        if (number > ((uint64_t)INT64_MAX - digit) / 10) return false;
        number = number * 10 + digit;
    }
    /* Match Go's signed-nanosecond duration range, then convert to the
     * microsecond monotonic clock, rounding toward earlier expiry. */
    uint64_t multiplier;
    if (!strcmp(p, "ns")) multiplier = 1;
    else if (!strcmp(p, "us")) multiplier = 1000;
    else if (!strcmp(p, "ms")) multiplier = 1000000;
    else if (!strcmp(p, "s")) multiplier = 1000000000;
    else if (!strcmp(p, "m")) multiplier = UINT64_C(60000000000);
    else if (!strcmp(p, "h")) multiplier = UINT64_C(3600000000000);
    else return false;
    if (number > (uint64_t)INT64_MAX / multiplier) return false;
    *duration_us = (int64_t)(number * multiplier / 1000);
    return true;
}

static const cJSON *unique_header(const cJSON *command, const char *name)
{
    const cJSON *headers = cJSON_GetObjectItemCaseSensitive(command, "headers");
    if (!cJSON_IsObject(headers)) return NULL;
    const cJSON *found = NULL;
    const cJSON *header;
    cJSON_ArrayForEach(header, headers) {
        if (!header->string || strcasecmp(header->string, name)) continue;
        /* Duplicate field names and multiple auth values are ambiguous. */
        if (found || !cJSON_IsArray(header) || cJSON_GetArraySize(header) != 1)
            return NULL;
        found = cJSON_GetArrayItem(header, 0);
        if (!cJSON_IsString(found) || !found->valuestring) return NULL;
    }
    return found;
}

bool dot_tunnel_command_authenticated(const cJSON *command, const char *bearer)
{
    if (!bearer || !*bearer) return false;
    const cJSON *header = unique_header(command, "Authorization");
    if (!header || strncasecmp(header->valuestring, "Bearer ", 7)) return false;
    const char *value = header->valuestring + 7;
    size_t size = strlen(bearer);
    if (strlen(value) != size) return false;
    unsigned difference = 0;
    for (size_t i = 0; i < size; ++i)
        difference |= (unsigned char)value[i] ^ (unsigned char)bearer[i];
    return difference == 0;
}

bool dot_tunnel_command_authorized(const cJSON *command, const char *bearer,
    bool private_tunnel_scope)
{
    /* No caller-asserted ID or header becomes a user identity here. The only
     * scope-mode principal is the dedicated private tunnel's service ACL.
     * This helper's caller must have already established that transport. */
    return cJSON_IsObject(command) &&
        (private_tunnel_scope || dot_tunnel_command_authenticated(command, bearer));
}

bool dot_tunnel_protocol_header_matches(const cJSON *command)
{
    const cJSON *rpc = cJSON_GetObjectItemCaseSensitive(command, "jsonrpc");
    if (!cJSON_IsObject(rpc)) return false;
    /* Notifications have no request capability metadata and cannot cause any
     * application-side mutation. ID-bearing MCP2 requests must agree exactly
     * with the HTTP transport's single protocol header value. */
    if (!cJSON_GetObjectItemCaseSensitive(rpc, "id")) return true;
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(rpc, "params");
    const cJSON *meta = cJSON_GetObjectItemCaseSensitive(params, "_meta");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(meta,
        "io.modelcontextprotocol/protocolVersion");
    const cJSON *header = unique_header(command, "MCP-Protocol-Version");
    return cJSON_IsString(version) && version->valuestring &&
        header && !strcmp(version->valuestring, header->valuestring);
}

int dot_tunnel_rpc_http_status(const cJSON *rpc_response)
{
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(rpc_response, "error");
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
    if (!cJSON_IsNumber(code)) return 200;
    /* MCP2 schema mandates 400 for missing required client capabilities and
     * unsupported versions. Invalid JSON-RPC/params also represent bad input.
     * Keep their exact error body and data unchanged. */
    switch (code->valueint) {
        case -32700: case -32600: case -32602:
        case -32021: case -32022: return 400;
        default: return 200;
    }
}

cJSON *dot_tunnel_response_envelope(const cJSON *command, cJSON *rpc_response,
    int http_status, const char *response_type)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(command, "request_id");
    const cJSON *channel = cJSON_GetObjectItemCaseSensitive(command, "channel");
    if (!cJSON_IsString(id) || !id->valuestring || !*id->valuestring ||
        http_status < 100 || http_status > 599 || !response_type ||
        (channel && !cJSON_IsString(channel))) {
        cJSON_Delete(rpc_response);
        return NULL;
    }
    cJSON *envelope = cJSON_CreateObject();
    if (!envelope) { cJSON_Delete(rpc_response); return NULL; }
    if (!cJSON_AddStringToObject(envelope, "request_id", id->valuestring) ||
        !cJSON_AddStringToObject(envelope, "channel",
            channel ? channel->valuestring : "main") ||
        !cJSON_AddNumberToObject(envelope, "resp_code", http_status) ||
        !cJSON_AddStringToObject(envelope, "resp_type", response_type)) {
        cJSON_Delete(rpc_response);
        cJSON_Delete(envelope);
        return NULL;
    }
    if (rpc_response && !cJSON_AddItemToObject(envelope, "resp_json", rpc_response)) {
        cJSON_Delete(rpc_response);
        cJSON_Delete(envelope);
        return NULL;
    }
    cJSON *headers = cJSON_AddObjectToObject(envelope, "resp_headers");
    cJSON *content_type = headers ? cJSON_AddArrayToObject(headers, "Content-Type") : NULL;
    cJSON *content_value = content_type ? cJSON_CreateString("application/json") : NULL;
    if (!content_value || !cJSON_AddItemToArray(content_type, content_value)) {
        cJSON_Delete(content_value);
        cJSON_Delete(envelope);
        return NULL;
    }
    if (http_status == 401) {
        cJSON *challenge = cJSON_AddArrayToObject(headers, "WWW-Authenticate");
        cJSON *challenge_value = challenge ? cJSON_CreateString("Bearer realm=\"esp32-dot\"") : NULL;
        if (!challenge_value || !cJSON_AddItemToArray(challenge, challenge_value)) {
            cJSON_Delete(challenge_value);
            cJSON_Delete(envelope);
            return NULL;
        }
    }
    return envelope;
}

#ifndef DOT_TUNNEL_PROTOCOL_ONLY
#include "dot_clock.h"
#include "dot_mcp.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>

/* Device-specific bound, deliberately below the service's 10 MiB maximum.
 * Small text/tool traffic only; never tunnel microphone audio through MCP. */
#define TUNNEL_BODY_MAX (128U * 1024U)
#define TUNNEL_IO_TIMEOUT_MS 25000
#define TUNNEL_BACKOFF_MAX_MS 30000U
#define TUNNEL_RETRY_AFTER_MAX_MS 300000U
#define TUNNEL_RESPONSE_ATTEMPTS 5

static const char *TAG = "dot_tunnel";
static dot_config_t s_config;
static TaskHandle_t s_task;
static volatile bool s_connected;
static volatile int s_last_status;
static volatile int64_t s_last_poll_us;
static char s_poll_url[320];
static char s_response_url[256];
static char s_instance_id[48];

typedef struct {
    char *body;
    size_t length;
    size_t capacity;
    bool overflow;
    int64_t received_us;
    int64_t deadline_us; /* zero: no caller deadline */
    char retry_after[96];
    char server_date[96];
} http_capture_t;

static bool header_safe(const char *value, size_t maximum)
{
    if (!value || !*value || strlen(value) > maximum) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (*p < 0x20 || *p == 0x7f) return false;
    return true;
}

static bool tunnel_id_safe(const char *value)
{
    /* Opaque ID: this merely ensures a single safe URL path segment, and
     * does not parse a namespace or infer placement. */
    if (!value || !*value || strlen(value) >= sizeof(s_config.tunnel_id)) return false;
    for (const char *p = value; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_'))
            return false;
    return true;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    http_capture_t *capture = event->user_data;
    if (capture->deadline_us) {
        int64_t remaining = capture->deadline_us - esp_timer_get_time();
        if (remaining <= 0) return ESP_ERR_TIMEOUT;
        int timeout_ms = (int)(remaining / 1000);
        if (timeout_ms < 1) timeout_ms = 1;
        if (timeout_ms > TUNNEL_IO_TIMEOUT_MS) timeout_ms = TUNNEL_IO_TIMEOUT_MS;
        esp_http_client_set_timeout_ms(event->client, timeout_ms);
    }
    if (event->event_id == HTTP_EVENT_ON_HEADER) {
        /* First observed header is a conservative receipt timestamp. */
        if (!capture->received_us) capture->received_us = esp_timer_get_time();
        if (event->header_key && event->header_value &&
            !strcasecmp(event->header_key, "Retry-After"))
            snprintf(capture->retry_after, sizeof(capture->retry_after), "%s", event->header_value);
        if (event->header_key && event->header_value &&
            !strcasecmp(event->header_key, "Date") &&
            strlen(event->header_value) < sizeof(capture->server_date))
            snprintf(capture->server_date, sizeof(capture->server_date), "%s", event->header_value);
    }
    return ESP_OK;
}

static bool capture_append(http_capture_t *capture, const char *data, size_t length)
{
    if (length > TUNNEL_BODY_MAX - capture->length) return false;
    size_t need = capture->length + length + 1;
    if (need > capture->capacity) {
        size_t capacity = capture->capacity ? capture->capacity : 2048;
        while (capacity < need) capacity *= 2;
        if (capacity > TUNNEL_BODY_MAX + 1) capacity = TUNNEL_BODY_MAX + 1;
        char *body = realloc(capture->body, capacity);
        if (!body) return false;
        capture->body = body;
        capture->capacity = capacity;
    }
    memcpy(capture->body + capture->length, data, length);
    capture->length += length;
    capture->body[capture->length] = '\0';
    return true;
}

static esp_err_t set_remaining_timeout(esp_http_client_handle_t client, int64_t deadline_us)
{
    int timeout_ms = TUNNEL_IO_TIMEOUT_MS;
    if (deadline_us) {
        int64_t remaining = deadline_us - esp_timer_get_time();
        if (remaining <= 0) return ESP_ERR_TIMEOUT;
        if (remaining / 1000 < timeout_ms) timeout_ms = (int)(remaining / 1000);
        if (timeout_ms < 1) timeout_ms = 1;
    }
    return esp_http_client_set_timeout_ms(client, timeout_ms);
}

static esp_err_t request(const char *url, const char *body, const char *shard_token,
    int64_t deadline_us, http_capture_t *capture, int *status)
{
    memset(capture, 0, sizeof(*capture));
    capture->deadline_us = deadline_us;
    *status = 0;
    int timeout_ms = TUNNEL_IO_TIMEOUT_MS;
    if (deadline_us) {
        int64_t remaining = deadline_us - esp_timer_get_time();
        if (remaining <= 0) return ESP_ERR_TIMEOUT;
        if (remaining / 1000 < timeout_ms) timeout_ms = (int)(remaining / 1000);
        if (timeout_ms < 1) timeout_ms = 1;
    }
    esp_http_client_config_t config = {
        .url = url,
        .method = body ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .event_handler = http_event,
        .user_data = capture,
        .buffer_size = 2048,
        .buffer_size_tx = 6144,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    char auth[DOT_KEY_MAX + 16];
    snprintf(auth, sizeof(auth), "Bearer %s", s_config.tunnel_api_key);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "X-Tunnel-Client-Name", "esp32-dot");
    esp_http_client_set_header(client, "X-Tunnel-Client-Version", "0.2.0");
    esp_http_client_set_header(client, "X-Tunnel-Client-Instance-Id", s_instance_id);
    esp_http_client_set_header(client, "X-Tunnel-Client-Wire-Protocol-Version", "2026-08-25");
    /* MCP2 accepts self-contained requests. Local subscription/conversation
     * state still requires process affinity; these are independent flags.
     * Do not advertise wrong-cluster-v1: routing correction is not implemented. */
    esp_http_client_set_header(client, "X-Tunnel-MCP-Server-Info",
        "{\"version\":2,\"channels\":[{\"name\":\"main\",\"stateless\":true,\"proc_affinity\":true}]}");
    if (shard_token) esp_http_client_set_header(client, "X-Tunnel-Shard-Token", shard_token);
    if (body) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
    }
    /* Streaming API gives this task an explicit cancellation/deadline and
     * memory-bound check between every block. Event callback return codes are
     * not relied upon to abort esp_http_client_perform(). */
    size_t body_length = body ? strlen(body) : 0;
    esp_err_t result = esp_http_client_open(client, (int)body_length);
    for (size_t sent = 0; result == ESP_OK && sent < body_length;) {
        result = set_remaining_timeout(client, deadline_us);
        if (result != ESP_OK) break;
        int written = esp_http_client_write(client, body + sent, (int)(body_length - sent));
        if (written <= 0) { result = ESP_FAIL; break; }
        sent += (size_t)written;
    }
    if (result == ESP_OK) {
        result = set_remaining_timeout(client, deadline_us);
        if (result == ESP_OK) {
            int64_t content_length = esp_http_client_fetch_headers(client);
            if (content_length < 0) result = ESP_FAIL;
            else if (content_length > TUNNEL_BODY_MAX) result = ESP_ERR_NO_MEM;
        }
    }
    *status = esp_http_client_get_status_code(client);
    /* Date is trusted only after verified TLS to the fixed OpenAI origin and
     * successful response headers. Bootstrap time never authorizes events. */
    if (result == ESP_OK && *status >= 200 && *status < 300 && *capture->server_date)
        dot_clock_observe_http_date(capture->server_date);
    if (!capture->received_us) capture->received_us = esp_timer_get_time();
    char block[2048];
    while (result == ESP_OK) {
        result = set_remaining_timeout(client, deadline_us);
        if (result != ESP_OK) break;
        int count = esp_http_client_read(client, block, sizeof(block));
        if (count < 0) { result = ESP_FAIL; break; }
        if (!count) {
            if (!esp_http_client_is_complete_data_received(client)) result = ESP_FAIL;
            break;
        }
        if (!capture_append(capture, block, (size_t)count)) { result = ESP_ERR_NO_MEM; break; }
    }
    if (deadline_us && esp_timer_get_time() >= deadline_us) result = ESP_ERR_TIMEOUT;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    memset(auth, 0, sizeof(auth));
    return result;
}

/* IMF-fixdate Retry-After uses UTC. Integer calendar conversion avoids any
 * dependence on the process TZ environment or nonportable timegm(). */
static int64_t civil_days(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    int era = (year >= 0 ? year : year - 399) / 400;
    unsigned yoe = (unsigned)(year - era * 400);
    unsigned mp = month > 2 ? month - 3 : month + 9;
    unsigned doy = (153 * mp + 2) / 5 + day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;
}

static uint32_t retry_after_ms(const char *value)
{
    if (!value || !*value) return 0;
    bool digits = true;
    uint64_t seconds = 0;
    for (const char *p = value; *p; ++p) {
        if (*p < '0' || *p > '9') { digits = false; break; }
        if (seconds < TUNNEL_RETRY_AFTER_MAX_MS / 1000U)
            seconds = seconds * 10 + (unsigned)(*p - '0');
    }
    if (digits) return seconds > TUNNEL_RETRY_AFTER_MAX_MS / 1000U ?
        TUNNEL_RETRY_AFTER_MAX_MS : (uint32_t)(seconds * 1000);
    char weekday[4], month_name[4], zone[4], tail;
    int day, year, hour, minute, second;
    if (sscanf(value, "%3s, %d %3s %d %d:%d:%d %3s%c", weekday, &day,
            month_name, &year, &hour, &minute, &second, zone, &tail) != 8 ||
        strcmp(zone, "GMT") || year < 1970 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59)
        return 0;
    static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"};
    unsigned month;
    for (month = 0; month < 12 && strcmp(month_name, months[month]); ++month) {}
    if (month == 12) return 0;
    int64_t epoch = civil_days(year, month + 1, (unsigned)day) * 86400 +
        hour * 3600 + minute * 60 + second;
    int64_t delta = epoch - (int64_t)time(NULL);
    if (delta <= 0) return 0;
    return delta > TUNNEL_RETRY_AFTER_MAX_MS / 1000 ? TUNNEL_RETRY_AFTER_MAX_MS :
        (uint32_t)(delta * 1000);
}

static bool backoff(unsigned attempt, uint32_t retry_after, int64_t deadline_us)
{
    uint32_t delay = 500U << (attempt > 6 ? 6 : attempt);
    if (delay > TUNNEL_BACKOFF_MAX_MS) delay = TUNNEL_BACKOFF_MAX_MS;
    delay += esp_random() % 251U;
    if (retry_after > delay) delay = retry_after;
    while (delay) {
        uint32_t part = delay > 250 ? 250 : delay;
        if (deadline_us && deadline_us - esp_timer_get_time() <= (int64_t)part * 1000)
            return false;
        vTaskDelay(pdMS_TO_TICKS(part));
        delay -= part;
    }
    return true;
}

static bool response_retryable(esp_err_t result, int status)
{
    return (result != ESP_OK && result != ESP_ERR_NO_MEM) || status == 408 ||
        status == 429 || status == 502 || status == 503 || status == 504;
}

static bool organization_context_required(const http_capture_t *capture)
{
    cJSON *body = capture->body ?
        cJSON_ParseWithLength(capture->body, capture->length + 1) : NULL;
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(body, "error");
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
    bool required = cJSON_IsString(code) &&
        !strcmp(code->valuestring, "tunnel_active_organization_required");
    cJSON_Delete(body);
    return required;
}

static cJSON *protocol_header_error(const cJSON *rpc)
{
    cJSON *response = cJSON_CreateObject();
    if (!response) return NULL;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(rpc, "id");
    cJSON_AddStringToObject(response, "jsonrpc", "2.0");
    bool integer_id = cJSON_IsNumber(id) && id->valuedouble >= -9007199254740991.0 &&
        id->valuedouble <= 9007199254740991.0 &&
        (double)(int64_t)id->valuedouble == id->valuedouble;
    if (cJSON_IsString(id) || integer_id)
        cJSON_AddItemToObject(response, "id", cJSON_Duplicate(id, true));
    cJSON *error = cJSON_AddObjectToObject(response, "error");
    if (!error) { cJSON_Delete(response); return NULL; }
    cJSON_AddNumberToObject(error, "code", -32602);
    cJSON_AddStringToObject(error, "message",
        "MCP-Protocol-Version header must match params._meta.io.modelcontextprotocol/protocolVersion");
    return response;
}

static void post_response(cJSON *envelope, const char *shard_token, int64_t deadline_us)
{
    char *body = cJSON_PrintUnformatted(envelope);
    if (!body) return;
    if (strlen(body) > TUNNEL_BODY_MAX) { cJSON_free(body); return; }
    /* Retry delivery only. The MCP operation is NEVER executed again. */
    for (unsigned attempt = 0; attempt < TUNNEL_RESPONSE_ATTEMPTS; ++attempt) {
        if (deadline_us && esp_timer_get_time() >= deadline_us) break;
        http_capture_t capture;
        int status;
        esp_err_t result = request(s_response_url, body, shard_token, deadline_us, &capture, &status);
        uint32_t retry_after = (status == 429 || status == 503) ? retry_after_ms(capture.retry_after) : 0;
        bool organization_required = status >= 400 && status < 500 &&
            organization_context_required(&capture);
        free(capture.body);
        s_last_status = status;
        if (organization_required) {
            s_connected = false;
            ESP_LOGE(TAG, "tunnel_active_organization_required: this build needs an organization-context configuration extension");
            vTaskSuspend(NULL);
            break;
        }
        if (result == ESP_OK && status >= 200 && status < 300) break;
        if (status == 401 || status == 403) {
            s_connected = false;
            ESP_LOGE(TAG, "Tunnel credential/permission rejected (HTTP %d); reprovision and reboot", status);
            /* Prevent repeated authenticated requests with rejected credentials. */
            vTaskSuspend(NULL);
            break;
        }
        if (status == 404 || !response_retryable(result, status)) break;
        if (attempt + 1 == TUNNEL_RESPONSE_ATTEMPTS || !backoff(attempt, retry_after, deadline_us)) break;
    }
    cJSON_free(body);
}

static void process_command(const cJSON *command, int64_t received_us)
{
    const cJSON *request_id = cJSON_GetObjectItemCaseSensitive(command, "request_id");
    const cJSON *shard = cJSON_GetObjectItemCaseSensitive(command, "shard_token");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(command, "command_type");
    const cJSON *channel = cJSON_GetObjectItemCaseSensitive(command, "channel");
    if (!cJSON_IsString(request_id) || !request_id->valuestring || !*request_id->valuestring ||
        !cJSON_IsString(shard) || !header_safe(shard->valuestring, 4096) ||
        (type && !cJSON_IsString(type)) || (channel && !cJSON_IsString(channel))) {
        ESP_LOGW(TAG, "Discarding malformed tunnel command");
        return;
    }
    const cJSON *timeout = cJSON_GetObjectItemCaseSensitive(command, "response_timeout");
    int64_t duration_us = 0, deadline_us = 0;
    if (cJSON_IsString(timeout) && dot_tunnel_parse_timeout_us(timeout->valuestring, &duration_us)) {
        if (duration_us > INT64_MAX - received_us) return;
        deadline_us = received_us + duration_us;
        if (esp_timer_get_time() >= deadline_us) return;
    }
    /* openapi.json makes this field optional with default "jsonrpc".
     * An explicitly unknown discriminator must never be reinterpreted. */
    bool is_jsonrpc = !type || !strcmp(type->valuestring, "jsonrpc");
    bool is_termination = type && !strcmp(type->valuestring, "session_termination");
    if (!is_jsonrpc && !is_termination) {
        ESP_LOGW(TAG, "Unsupported tunnel command discriminator; continuing");
        return;
    }
    cJSON *rpc_result = NULL;
    int response_status = 200;
    const char *response_type = is_termination ? "session_termination_response" : "jsonrpc_response";
    if (!dot_tunnel_command_authorized(command, s_config.mcp_bearer, s_config.tunnel_scope_auth)) {
        response_status = 401;
    } else if (channel && strcmp(channel->valuestring, "main")) {
        response_status = 404;
    } else if (is_termination) {
        /* This MCP server issues no protocol session IDs: no transport state
         * exists to tear down. Persistent event subscriptions are independent. */
        response_status = 204;
        response_type = "session_termination_response";
    } else {
        const cJSON *rpc = cJSON_GetObjectItemCaseSensitive(command, "jsonrpc");
        if (!cJSON_IsObject(rpc)) response_status = 400;
        else if (!dot_tunnel_protocol_header_matches(command)) {
            response_status = 400;
            rpc_result = protocol_header_error(rpc);
        }
        else {
            rpc_result = dot_mcp_dispatch(rpc, true);
            if (rpc_result) response_status = dot_tunnel_rpc_http_status(rpc_result);
            if (!rpc_result) {
                if (cJSON_GetObjectItemCaseSensitive(rpc, "id")) {
                    /* The application failed to allocate/build its response.
                     * Never relabel an ID-bearing request as a notification. */
                    response_status = 500;
                } else {
                    response_status = 202;
                    response_type = "notify_ack";
                }
            }
        }
    }
    if (deadline_us && esp_timer_get_time() >= deadline_us) {
        cJSON_Delete(rpc_result);
        return;
    }
    cJSON *envelope = dot_tunnel_response_envelope(command, rpc_result, response_status, response_type);
    if (envelope) {
        post_response(envelope, shard->valuestring, deadline_us);
        cJSON_Delete(envelope);
    }
}

static void tunnel_task(void *argument)
{
    (void)argument;
    unsigned failures = 0;
    for (;;) {
        http_capture_t capture;
        int status;
        esp_err_t result = request(s_poll_url, NULL, NULL, 0, &capture, &status);
        s_last_status = status;
        uint32_t retry_after = (status == 429 || status == 503) ? retry_after_ms(capture.retry_after) : 0;
        if (status >= 400 && status < 500 && organization_context_required(&capture)) {
            free(capture.body);
            s_connected = false;
            ESP_LOGE(TAG, "tunnel_active_organization_required: this build needs an organization-context configuration extension");
            vTaskSuspend(NULL);
            continue;
        }
        if (status == 401 || status == 403) {
            free(capture.body);
            s_connected = false;
            ESP_LOGE(TAG, "Tunnel credential/permission rejected (HTTP %d); reprovision and reboot", status);
            vTaskSuspend(NULL);
            continue;
        }
        if (result != ESP_OK || (status != 200 && status != 204)) {
            free(capture.body);
            s_connected = false;
            ESP_LOGW(TAG, "Poll unavailable (HTTP %d, %s); reconnecting", status, esp_err_to_name(result));
            backoff(failures++, retry_after, 0);
            continue;
        }
        failures = 0;
        s_connected = true;
        s_last_poll_us = esp_timer_get_time();
        if (status == 204) { free(capture.body); continue; }
        cJSON *poll = capture.body ? cJSON_ParseWithLength(capture.body, capture.length + 1) : NULL;
        free(capture.body);
        const cJSON *commands = cJSON_GetObjectItemCaseSensitive(poll, "commands");
        if (!cJSON_IsArray(commands)) {
            cJSON_Delete(poll);
            s_connected = false;
            ESP_LOGW(TAG, "Invalid poll envelope; backing off");
            backoff(failures++, 0, 0);
            continue;
        }
        /* limit=1 is a hint; preserve every returned command. No concurrent
         * tool calls avoids races in local conversation/subscription state. */
        const cJSON *command;
        cJSON_ArrayForEach(command, commands) process_command(command, capture.received_us);
        cJSON_Delete(poll);
    }
}

esp_err_t dot_tunnel_start(const dot_config_t *config)
{
    if (!config || !dot_config_tunnel_ready(config) || !tunnel_id_safe(config->tunnel_id) ||
        !header_safe(config->tunnel_api_key, DOT_KEY_MAX) ||
        (!config->tunnel_scope_auth && !header_safe(config->mcp_bearer, DOT_KEY_MAX)))
        return ESP_ERR_INVALID_ARG;
    if (s_task) return ESP_ERR_INVALID_STATE;
    memcpy(&s_config, config, sizeof(s_config));
    snprintf(s_poll_url, sizeof(s_poll_url),
        "https://api.openai.com/v1/tunnels/%s/poll?limit=1&timeout_ms=15000", config->tunnel_id);
    snprintf(s_response_url, sizeof(s_response_url),
        "https://api.openai.com/v1/tunnels/%s/response", config->tunnel_id);
    snprintf(s_instance_id, sizeof(s_instance_id), "esp32-%08" PRIx32 "-%08" PRIx32,
        esp_random(), esp_random());
    s_connected = false;
    s_last_status = 0;
    if (config->tunnel_scope_auth)
        ESP_LOGI(TAG, "Private tunnel ACL authorization enabled; dedicated personal tunnel only");
    if (xTaskCreate(tunnel_task, "dot_tunnel", 16384, NULL, 4, &s_task) != pdPASS) {
        s_task = NULL;
        memset(&s_config, 0, sizeof(s_config));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool dot_tunnel_connected(void)
{
    return s_connected && esp_timer_get_time() - s_last_poll_us < 90000000;
}

int dot_tunnel_last_status(void)
{
    return s_last_status;
}
#endif /* DOT_TUNNEL_PROTOCOL_ONLY */
