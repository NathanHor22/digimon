/* Device-local MCP 2.0 server and outbound MCP Events delivery.
 * Protocol: https://developers.openai.com/plugins/build/mcp-events
 * No account cookie is stored. The owner is either the dedicated MCP bearer,
 * or (only after explicit opt-in) the personally restricted private tunnel's
 * ACL principal. The transport establishes this; caller headers are not an
 * identity in private-tunnel mode. Switching credentials/tunnels revokes state.
 */
#include "dot_mcp.h"
#include "dot_controller.h"
#include "dot_clock.h"
#include "dot_preferences.h"
#include "dot_audio.h"
#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "nvs.h"

#define PROTOCOL_VERSION "2026-07-28"
#define EVENT_NAME "device.utterance"
#define CALLBACK_URL_MAX 1023
#define SECRET_MAX 94
#define MIN_EPOCH INT64_C(1704067200)
#define SUB_TTL_MS INT64_C(86400000)
#define VERIFY_CACHE_SECONDS 300
#define KEY_ROTATION_SECONDS 300
#define WEBHOOK_TIMEOUT_MS 10000
#define WEBHOOK_REPLY_MAX 1023

typedef struct {
    uint32_t version;
    bool active;
    char id[69];
    char owner[65];
    char device_id[DOT_DEVICE_ID_MAX + 1];
    char url[CALLBACK_URL_MAX + 1];
    char secret[SECRET_MAX + 1];
    char previous_secret[SECRET_MAX + 1];
    int64_t previous_secret_until;
    int64_t expires_ms;
} subscription_t;

typedef struct {
    char host[254];
    char path[CALLBACK_URL_MAX + 1];
    char address[INET_ADDRSTRLEN];
} destination_t;

static const char *TAG = "dot_mcp";
static subscription_t s_sub;
static SemaphoreHandle_t s_lock;
static char s_device[DOT_DEVICE_ID_MAX + 1];
static char s_owner[65];
static uint32_t s_generation;
static int64_t s_verified_until;

static const cJSON *get(const cJSON *o, const char *name)
{
    return cJSON_GetObjectItemCaseSensitive(o, name);
}

static bool strval(const cJSON *v, size_t lo, size_t hi)
{
    return cJSON_IsString(v) && v->valuestring && strlen(v->valuestring) >= lo &&
           strlen(v->valuestring) <= hi;
}

static bool only_keys(const cJSON *o, const char *const *keys, size_t count)
{
    if (!cJSON_IsObject(o)) return false;
    const cJSON *v;
    cJSON_ArrayForEach(v, o) {
        bool found = false;
        for (size_t i = 0; i < count; ++i) {
            if (v->string && !strcmp(v->string, keys[i])) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

/* Reject duplicate keys instead of permitting ambiguous first/last-key parsing.
 * A structural bound also prevents hostile deeply nested tool arguments. */
static bool safe_json(const cJSON *v, unsigned depth, unsigned *budget)
{
    if (!v || depth > 12 || !*budget) return false;
    --*budget;
    if (cJSON_IsNumber(v) && !isfinite(v->valuedouble)) return false;
    const cJSON *a;
    cJSON_ArrayForEach(a, v) {
        if (cJSON_IsObject(v)) {
            if (!a->string) return false;
            for (const cJSON *b = a->next; b; b = b->next) {
                if (b->string && !strcmp(a->string, b->string)) return false;
            }
        }
        if (!safe_json(a, depth + 1, budget)) return false;
    }
    return true;
}

static bool constant_equal(const char *a, const char *b)
{
    if (!a || !b) return false;
    size_t an = strlen(a), bn = strlen(b);
    if (an != bn) return false;
    volatile unsigned char diff = 0;
    for (size_t i = 0; i < an; ++i) diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return diff == 0;
}

static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

static bool digest_parts(char hex[65], const char *const *parts, size_t count)
{
    unsigned char hash[32];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    int rc = info ? mbedtls_md_setup(&ctx, info, 0) : -1;
    if (!rc) rc = mbedtls_md_starts(&ctx);
    for (size_t i = 0; !rc && i < count; ++i) {
        rc = mbedtls_md_update(&ctx, (const unsigned char *)parts[i], strlen(parts[i]));
        if (!rc) rc = mbedtls_md_update(&ctx, (const unsigned char *)"\n", 1);
    }
    if (!rc) rc = mbedtls_md_finish(&ctx, hash);
    mbedtls_md_free(&ctx);
    if (rc) return false;
    for (size_t i = 0; i < sizeof(hash); ++i) snprintf(hex + 2 * i, 3, "%02x", hash[i]);
    wipe(hash, sizeof(hash));
    return true;
}

static int64_t now_ms(void)
{
    time_t now = time(NULL);
    return (int64_t)now * 1000;
}

static bool clock_ready(void)
{
    return dot_clock_fresh() && now_ms() >= MIN_EPOCH * 1000;
}

static void iso_time(int64_t epoch_ms, char out[25])
{
    time_t seconds = (time_t)(epoch_ms / 1000);
    struct tm tm;
    gmtime_r(&seconds, &tm);
    strftime(out, 25, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static esp_err_t save_subscription(const subscription_t *sub)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("dot_events", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, "subscription", sub, sizeof(*sub));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static bool decode_secret(const char *secret, unsigned char key[64], size_t *length)
{
    if (!secret || strncmp(secret, "whsec_", 6) || strlen(secret) > SECRET_MAX) return false;
    int rc = mbedtls_base64_decode(key, 64, length,
        (const unsigned char *)secret + 6, strlen(secret) - 6);
    return !rc && *length >= 24 && *length <= 64;
}

/* IPv4 is deliberate: this small implementation fails closed on IPv6-only
 * callbacks. Resolve every attempt; all returned addresses must be public, and
 * the actual connection is made to one of these addresses (no second DNS).
 */
static bool public_ipv4(uint32_t net_address)
{
    uint32_t ip = ntohl(net_address);
    unsigned a = ip >> 24, b = (ip >> 16) & 255, c = (ip >> 8) & 255;
    if (a == 0 || a == 10 || a == 127 || a >= 224) return false;
    if (a == 100 && b >= 64 && b <= 127) return false;
    if (a == 169 && b == 254) return false;
    if (a == 172 && b >= 16 && b <= 31) return false;
    if (a == 192 && (b == 168 || (b == 0 && (c == 0 || c == 2)) ||
                     (b == 88 && c == 99))) return false;
    if (a == 198 && (b == 18 || b == 19 || (b == 51 && c == 100))) return false;
    if (a == 203 && b == 0 && c == 113) return false;
    return true;
}

static bool parse_callback(const char *url, destination_t *dest)
{
    if (!url || strlen(url) > CALLBACK_URL_MAX || strncmp(url, "https://", 8)) return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; ++p) {
        if (*p <= 32 || *p >= 127 || *p == '\\' || *p == '#') return false;
    }
    const char *start = url + 8;
    const char *slash = strchr(start, '/');
    const char *query = strchr(start, '?');
    const char *end = slash ? slash : start + strlen(start);
    if (query && query < end) end = query;
    size_t hostlen = (size_t)(end - start);
    /* Only standard HTTPS port, no credentials, escaped host, bracketed IPv6,
     * numeric port, unicode IDN, or alternate IP representations. */
    if (hostlen > 4 && !strncmp(end - 4, ":443", 4)) hostlen -= 4;
    if (!hostlen || hostlen >= sizeof(dest->host)) return false;
    memcpy(dest->host, start, hostlen);
    dest->host[hostlen] = 0;
    for (size_t i = 0; i < hostlen; ++i) {
        unsigned char ch = (unsigned char)dest->host[i];
        if (!(isalnum(ch) || ch == '-' || ch == '.')) return false;
    }
    if (dest->host[0] == '.' || dest->host[hostlen - 1] == '.') return false;
    if (*end == '?') snprintf(dest->path, sizeof(dest->path), "/%s", end);
    else snprintf(dest->path, sizeof(dest->path), "%s", *end ? end : "/");
    return strlen(dest->path) <= CALLBACK_URL_MAX;
}

static bool resolve_callback(const char *url, destination_t *dest)
{
    memset(dest, 0, sizeof(*dest));
    if (!parse_callback(url, dest)) return false;
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(dest->host, "443", &hints, &addresses) || !addresses) return false;
    bool valid = true, selected = false;
    for (struct addrinfo *ai = addresses; ai; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET || ai->ai_addrlen < sizeof(struct sockaddr_in)) {
            valid = false; break;
        }
        struct sockaddr_in *address = (struct sockaddr_in *)ai->ai_addr;
        if (!public_ipv4(address->sin_addr.s_addr)) { valid = false; break; }
        if (!selected) {
            selected = inet_ntop(AF_INET, &address->sin_addr, dest->address,
                                 sizeof(dest->address)) != NULL;
        }
    }
    freeaddrinfo(addresses);
    return valid && selected;
}

static bool signature(const char *secret, const char *id, const char *timestamp,
                      const char *body, char out[49])
{
    unsigned char key[64], hash[32];
    size_t keylen = 0, encoded = 0;
    if (!decode_secret(secret, key, &keylen)) { wipe(key, sizeof(key)); return false; }
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int rc = info ? mbedtls_md_setup(&ctx, info, 1) : -1;
    if (!rc) rc = mbedtls_md_hmac_starts(&ctx, key, keylen);
    if (!rc) rc = mbedtls_md_hmac_update(&ctx, (const unsigned char *)id, strlen(id));
    if (!rc) rc = mbedtls_md_hmac_update(&ctx, (const unsigned char *)".", 1);
    if (!rc) rc = mbedtls_md_hmac_update(&ctx, (const unsigned char *)timestamp, strlen(timestamp));
    if (!rc) rc = mbedtls_md_hmac_update(&ctx, (const unsigned char *)".", 1);
    if (!rc) rc = mbedtls_md_hmac_update(&ctx, (const unsigned char *)body, strlen(body));
    if (!rc) rc = mbedtls_md_hmac_finish(&ctx, hash);
    mbedtls_md_free(&ctx);
    wipe(key, sizeof(key));
    if (!rc) rc = mbedtls_base64_encode((unsigned char *)out + 3, 46, &encoded, hash, sizeof(hash));
    wipe(hash, sizeof(hash));
    if (rc || encoded != 44) return false;
    memcpy(out, "v1,", 3);
    out[3 + encoded] = 0;
    return true;
}

typedef struct {
    char body[WEBHOOK_REPLY_MAX + 1];
    size_t used;
    bool overflow;
    int64_t deadline_us;
} webhook_response_t;

static esp_err_t webhook_event(esp_http_client_event_t *evt)
{
    webhook_response_t *response = evt->user_data;
    if (!response || !evt->client) return ESP_OK;
    int64_t remaining = (response->deadline_us - esp_timer_get_time()) / 1000;
    /* IDF does not use event callback return values to abort reads. This only
     * shrinks an in-progress header/read timeout; the explicit I/O loop below
     * owns all deadline and size checks. */
    if (remaining < 1) remaining = 1;
    if (remaining > WEBHOOK_TIMEOUT_MS) remaining = WEBHOOK_TIMEOUT_MS;
    (void)esp_http_client_set_timeout_ms(evt->client, (int)remaining);
    return ESP_OK;
}

static esp_err_t webhook_set_timeout(esp_http_client_handle_t client, int64_t deadline_us)
{
    int64_t remaining = deadline_us - esp_timer_get_time();
    if (remaining <= 0) return ESP_ERR_TIMEOUT;
    int timeout_ms = (int)(remaining / 1000);
    if (timeout_ms < 1) timeout_ms = 1;
    return esp_http_client_set_timeout_ms(client, timeout_ms);
}

/* Uses an IP-literal URL to pin the checked DNS result. common_name is passed
 * by IDF to mbedtls_ssl_set_hostname: it supplies both SNI and certificate name
 * verification. Host retains the original hostname. TLS validation stays on. */
static esp_err_t post_signed(const subscription_t *sub, const char *id,
                            const char *body, webhook_response_t *response,
                            int *status)
{
    *status = 0;
    if (!clock_ready()) return ESP_ERR_INVALID_STATE;
    const int64_t deadline_us = esp_timer_get_time() + WEBHOOK_TIMEOUT_MS * INT64_C(1000);
    destination_t *destination = calloc(1, sizeof(*destination));
    if (!destination) return ESP_ERR_NO_MEM;
    if (!resolve_callback(sub->url, destination)) { free(destination); return ESP_ERR_INVALID_ARG; }
    int64_t remaining_ms = (deadline_us - esp_timer_get_time()) / 1000;
    if (remaining_ms <= 0) { free(destination); return ESP_ERR_TIMEOUT; }
    char *pinned_url = malloc(strlen(destination->path) + INET_ADDRSTRLEN + 10);
    if (!pinned_url) { free(destination); return ESP_ERR_NO_MEM; }
    sprintf(pinned_url, "https://%s%s", destination->address, destination->path);
    char timestamp[24], sig[49], oldsig[49], signatures[100];
    snprintf(timestamp, sizeof(timestamp), "%" PRId64, now_ms() / 1000);
    if (!signature(sub->secret, id, timestamp, body, sig)) {
        free(pinned_url); free(destination); return ESP_ERR_INVALID_ARG;
    }
    snprintf(signatures, sizeof(signatures), "%s", sig);
    if (sub->previous_secret[0] && now_ms() < sub->previous_secret_until * 1000 &&
        signature(sub->previous_secret, id, timestamp, body, oldsig)) {
        snprintf(signatures, sizeof(signatures), "%s %s", sig, oldsig);
    }
    memset(response, 0, sizeof(*response));
    response->deadline_us = deadline_us;
    esp_http_client_config_t config = {
        .url = pinned_url,
        .common_name = destination->host,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = (int)remaining_ms,
        .disable_auto_redirect = true,
        .max_authorization_retries = -1,
        .method = HTTP_METHOD_POST,
        .event_handler = webhook_event,
        .user_data = response,
        .buffer_size = 1024,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = client ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Host", destination->host);
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err == ESP_OK) err = esp_http_client_set_header(client, "webhook-id", id);
    if (err == ESP_OK) err = esp_http_client_set_header(client, "webhook-timestamp", timestamp);
    if (err == ESP_OK) err = esp_http_client_set_header(client, "webhook-signature", signatures);
    if (err == ESP_OK) err = esp_http_client_set_header(client, "X-MCP-Subscription-Id", sub->id);
    const size_t body_length = strlen(body);
    if (err == ESP_OK) err = webhook_set_timeout(client, deadline_us);
    if (err == ESP_OK) err = esp_http_client_open(client, (int)body_length);
    for (size_t sent = 0; err == ESP_OK && sent < body_length;) {
        err = webhook_set_timeout(client, deadline_us);
        if (err != ESP_OK) break;
        int written = esp_http_client_write(client, body + sent, (int)(body_length - sent));
        if (written <= 0 || (size_t)written > body_length - sent) { err = ESP_FAIL; break; }
        sent += (size_t)written;
    }
    if (err == ESP_OK) {
        err = webhook_set_timeout(client, deadline_us);
        if (err == ESP_OK) {
            int64_t length = esp_http_client_fetch_headers(client);
            if (length < 0) err = ESP_FAIL;
            else if (length > WEBHOOK_REPLY_MAX) { response->overflow = true; err = ESP_ERR_INVALID_SIZE; }
        }
    }
    /* These responses are at most 1 KiB. Read one body byte at a time so IDF's
     * internal fill-buffer loop cannot delay our deadline check while waiting
     * for a large caller-supplied read buffer to fill. TLS maintains its own
     * record buffer; this does not create a network request for every byte. */
    while (err == ESP_OK) {
        err = webhook_set_timeout(client, deadline_us);
        if (err != ESP_OK) break;
        char byte;
        int count = esp_http_client_read(client, &byte, 1);
        if (count < 0) { err = ESP_FAIL; break; }
        if (!count) {
            if (!esp_http_client_is_complete_data_received(client)) err = ESP_FAIL;
            break;
        }
        if (count != 1 || response->used >= WEBHOOK_REPLY_MAX) {
            response->overflow = true; err = ESP_ERR_INVALID_SIZE; break;
        }
        response->body[response->used++] = byte;
        response->body[response->used] = 0;
    }
    if (client) {
        *status = esp_http_client_get_status_code(client);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    if (response->overflow) err = ESP_ERR_INVALID_SIZE;
    if (esp_timer_get_time() > response->deadline_us) err = ESP_ERR_TIMEOUT;
    wipe(signatures, sizeof(signatures));
    free(pinned_url);
    free(destination);
    return err;
}

static bool verify_callback(const subscription_t *sub, const char **reason)
{
    const int64_t challenge_created_us = esp_timer_get_time();
    unsigned char random[24];
    char challenge[49], webhook_id[70];
    esp_fill_random(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); ++i) snprintf(challenge + 2 * i, 3, "%02x", random[i]);
    snprintf(webhook_id, sizeof(webhook_id), "msg_verification_%s", challenge);
    cJSON *body = cJSON_CreateObject();
    if (!body) { *reason = "internal_error"; return false; }
    cJSON_AddStringToObject(body, "type", "verification");
    cJSON_AddStringToObject(body, "challenge", challenge);
    char *bytes = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    webhook_response_t *response = calloc(1, sizeof(*response));
    if (!bytes || !response) {
        cJSON_free(bytes); free(response); *reason = "internal_error"; return false;
    }
    int status;
    esp_err_t err = post_signed(sub, webhook_id, bytes, response, &status);
    cJSON_free(bytes);
    cJSON *reply = err == ESP_OK ? cJSON_Parse(response->body) : NULL;
    unsigned budget = 64;
    bool valid = err == ESP_OK && status >= 200 && status < 300 &&
                 esp_timer_get_time() - challenge_created_us <= WEBHOOK_TIMEOUT_MS * INT64_C(1000) &&
                 reply && safe_json(reply, 0, &budget) && cJSON_IsObject(reply) &&
                 strval(get(reply, "challenge"), sizeof(challenge) - 1, sizeof(challenge) - 1) &&
                 constant_equal(challenge, get(reply, "challenge")->valuestring);
    cJSON_Delete(reply);
    free(response);
    *reason = err == ESP_ERR_TIMEOUT ? "timeout" :
              err == ESP_ERR_INVALID_ARG ? "invalid_url" : "challenge_failed";
    return valid;
}

static cJSON *rpc_error(const cJSON *id, int code, const char *message, const char *reason)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    /* MCP 2.0 errors omit id when no valid request ID can be recovered. */
    if (id) cJSON_AddItemToObject(root, "id", cJSON_Duplicate(id, true));
    cJSON *error = cJSON_AddObjectToObject(root, "error");
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    if (reason) cJSON_AddStringToObject(cJSON_AddObjectToObject(error, "data"), "reason", reason);
    return root;
}

static cJSON *rpc_result(const cJSON *id, cJSON *result)
{
    if (!result) return rpc_error(id, -32603, "Internal error", NULL);
    /* MCP 2.0 requires resultType on every result, including extension RPCs. */
    if (!get(result, "resultType")) cJSON_AddStringToObject(result, "resultType", "complete");
    cJSON *meta = cJSON_GetObjectItemCaseSensitive(result, "_meta");
    if (!meta) meta = cJSON_AddObjectToObject(result, "_meta");
    cJSON *info = cJSON_AddObjectToObject(meta, "io.modelcontextprotocol/serverInfo");
    cJSON_AddStringToObject(info, "name", "esp32-dot");
    cJSON_AddStringToObject(info, "version", "0.2.0");
    cJSON *root = cJSON_CreateObject();
    if (!root) { cJSON_Delete(result); return NULL; }
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddItemToObject(root, "id", cJSON_Duplicate(id, true));
    cJSON_AddItemToObject(root, "result", result);
    return root;
}

static cJSON *discovery(void)
{
    return cJSON_Parse("{\"resultType\":\"complete\",\"supportedVersions\":[\"" PROTOCOL_VERSION "\"],"
        "\"ttlMs\":0,\"cacheScope\":\"private\","
        "\"capabilities\":{\"tools\":{},\"events\":{}}}");
}

static cJSON *tool_catalog(void)
{
    return cJSON_Parse("{\"ttlMs\":0,\"cacheScope\":\"private\",\"tools\":["
        "{\"name\":\"device_status\",\"title\":\"Read ESP32 status\","
        "\"description\":\"Read the paired ESP32 device ID, connection state, and pending request.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false},"
        "\"annotations\":{\"readOnlyHint\":true,\"destructiveHint\":false,\"idempotentHint\":true,\"openWorldHint\":false}},"
        "{\"name\":\"reply_to_device\",\"title\":\"Speak a reply on the ESP32\","
        "\"description\":\"Queue a spoken reply to a pending device.utterance event. Copy request_id from that event. A reply is accepted once per request; this never starts a new conversation.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
        "\"request_id\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":63},"
        "\"text\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":2000}},"
        "\"required\":[\"request_id\",\"text\"],\"additionalProperties\":false},"
        "\"annotations\":{\"readOnlyHint\":false,\"destructiveHint\":false,\"idempotentHint\":true,\"openWorldHint\":false}},"
        "{\"name\":\"get_device_preferences\",\"title\":\"Read device preferences\","
        "\"description\":\"Read this device's display name, aliases, response preferences, volume, hands-free and physical microphone mute state. Use when personalizing replies or reviewing settings. These are local device preferences.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false},"
        "\"annotations\":{\"readOnlyHint\":true,\"destructiveHint\":false,\"idempotentHint\":true,\"openWorldHint\":false}},"
        "{\"name\":\"update_device_preferences\",\"title\":\"Save device preferences\","
        "\"description\":\"Save explicitly requested device settings or response style. Fields are an atomic patch; aliases replaces the list. Text limits are UTF-8 bytes. Microphone mute can only be changed on the physical device. Do not invent or store sensitive personal facts.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
        "\"volume_percent\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100},"
        "\"hands_free\":{\"type\":\"boolean\"},"
        "\"device_name\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":32},"
        "\"aliases\":{\"type\":\"array\",\"maxItems\":4,\"uniqueItems\":true,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":32}},"
        "\"personality\":{\"type\":\"string\",\"maxLength\":512}},\"minProperties\":1,\"additionalProperties\":false},"
        "\"annotations\":{\"readOnlyHint\":false,\"destructiveHint\":false,\"idempotentHint\":true,\"openWorldHint\":false}}]}");
}

static cJSON *event_catalog(void)
{
    return cJSON_Parse("{\"ttlMs\":0,\"cacheScope\":\"private\",\"events\":[{\"name\":\"" EVENT_NAME "\","
        "\"description\":\"An accepted spoken utterance or USB text from the paired ESP32 device. To answer aloud, call reply_to_device with its request_id and reply text.\","
        "\"delivery\":[\"webhook\"],\"inputSchema\":{\"type\":\"object\","
        "\"properties\":{\"device_id\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":63}},"
        "\"required\":[\"device_id\"],\"additionalProperties\":false},"
        "\"payloadSchema\":{\"type\":\"object\",\"properties\":{"
        "\"device_id\":{\"type\":\"string\"},\"request_id\":{\"type\":\"string\"},"
        "\"text\":{\"type\":\"string\"},\"input_kind\":{\"type\":\"string\"}},"
        "\"required\":[\"device_id\",\"request_id\",\"text\",\"input_kind\"],"
        "\"additionalProperties\":false}}]}");
}

static bool empty_or_cursor(const cJSON *params)
{
    if (!params) return true;
    static const char *const keys[] = {"cursor", "_meta"};
    if (!only_keys(params, keys, 2)) return false;
    const cJSON *cursor = get(params, "cursor");
    return !cursor || cJSON_IsNull(cursor) || (strval(cursor, 0, 0));
}

static cJSON *device_preferences_result(void)
{
    cJSON *data = dot_preferences_json();
    if (!data) return NULL;
    const cJSON *stored = get(data, "microphone_muted");
    /* The runtime physical latch can be stricter than flash after a failed
     * mute write. Report actual privacy state; retain the stored value only
     * as a diagnostic. The storage module stays independent of audio. */
    if (!cJSON_IsBool(stored) || !cJSON_AddBoolToObject(data, "stored_microphone_muted", cJSON_IsTrue(stored))) {
        cJSON_Delete(data); return NULL;
    }
    cJSON *actual = cJSON_CreateBool(dot_audio_muted());
    if (!actual || !cJSON_ReplaceItemInObjectCaseSensitive(data, "microphone_muted", actual)) {
        cJSON_Delete(actual); cJSON_Delete(data); return NULL;
    }
    return data;
}

static cJSON *call_tool(const cJSON *id, const cJSON *params)
{
    static const char *const keys[] = {"name", "arguments", "_meta", "inputResponses", "requestState"};
    if (!only_keys(params, keys, 5) || !strval(get(params, "name"), 1, 64) ||
        get(params, "inputResponses") || get(params, "requestState"))
        return rpc_error(id, -32602, "Invalid tool parameters", NULL);
    const char *name = get(params, "name")->valuestring;
    const cJSON *args = get(params, "arguments");
    cJSON *data = NULL;
    const char *message = NULL;
    bool is_error = false;
    if (!strcmp(name, "device_status")) {
        if (args && (!cJSON_IsObject(args) || args->child))
            return rpc_error(id, -32602, "device_status takes no arguments", NULL);
        data = dot_controller_status();
        message = "ESP32 status";
    } else if (!strcmp(name, "get_device_preferences")) {
        if (args && (!cJSON_IsObject(args) || args->child))
            return rpc_error(id, -32602, "get_device_preferences takes no arguments", NULL);
        data = device_preferences_result();
        message = "Device preferences";
    } else if (!strcmp(name, "update_device_preferences")) {
        static const char *const argkeys[] = {"volume_percent", "hands_free", "device_name", "aliases", "personality"};
        if (!only_keys(args, argkeys, 5) || !args->child)
            return rpc_error(id, -32602, "Provide a preference patch; microphone mute is controlled physically", NULL);
        esp_err_t err = dot_preferences_update(args, false);
        if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_NOT_SUPPORTED)
            return rpc_error(id, -32602, "Invalid preference value or microphone control is physical only", NULL);
        data = device_preferences_result();
        if (data) cJSON_AddBoolToObject(data, "saved", err == ESP_OK);
        is_error = err != ESP_OK;
        message = err == ESP_OK ? "Device preferences saved" : "Could not persist device preferences";
    } else if (!strcmp(name, "reply_to_device")) {
        static const char *const argkeys[] = {"request_id", "text"};
        if (!only_keys(args, argkeys, 2) ||
            !strval(get(args, "request_id"), 1, DOT_REQUEST_ID_MAX) ||
            !strval(get(args, "text"), 1, DOT_TEXT_MAX))
            return rpc_error(id, -32602, "Expected request_id and text within device limits", NULL);
        bool duplicate = false;
        esp_err_t err = dot_controller_accept_reply(get(args, "request_id")->valuestring,
                                                   get(args, "text")->valuestring, &duplicate);
        data = cJSON_CreateObject();
        if (data) {
            cJSON_AddBoolToObject(data, "accepted", err == ESP_OK);
            cJSON_AddBoolToObject(data, "duplicate", duplicate);
            cJSON_AddStringToObject(data, "request_id", get(args, "request_id")->valuestring);
        }
        is_error = err != ESP_OK;
        message = err == ESP_OK ? (duplicate ? "Reply already accepted" : "Reply queued for speech") :
                  "Reply rejected: request expired, was cancelled, is unknown, or queue is full";
    } else return rpc_error(id, -32602, "Unknown tool", NULL);
    if (!data) return rpc_error(id, -32603, "Internal error", NULL);
    cJSON *result = cJSON_CreateObject();
    if (!result) { cJSON_Delete(data); return NULL; }
    cJSON_AddItemToObject(result, "structuredContent", data);
    cJSON_AddBoolToObject(result, "isError", is_error);
    cJSON *text = cJSON_CreateObject();
    cJSON_AddStringToObject(text, "type", "text");
    cJSON_AddStringToObject(text, "text", message);
    cJSON_AddItemToArray(cJSON_AddArrayToObject(result, "content"), text);
    return rpc_result(id, result);
}

static cJSON *subscription_request(const cJSON *id, const cJSON *params, bool remove)
{
    static const char *const keys[] = {"name", "arguments", "delivery", "cursor", "ttlMs", "_meta", "maxAgeMs"};
    static const char *const argkeys[] = {"device_id"};
    static const char *const deliverykeys[] = {"mode", "url", "secret"};
    const cJSON *args = get(params, "arguments"), *delivery = get(params, "delivery");
    if (!only_keys(params, keys, 7) || !strval(get(params, "name"), 1, 64) ||
        strcmp(get(params, "name")->valuestring, EVENT_NAME) ||
        !only_keys(args, argkeys, 1) || !strval(get(args, "device_id"), 1, DOT_DEVICE_ID_MAX) ||
        strcmp(get(args, "device_id")->valuestring, s_device) ||
        !only_keys(delivery, deliverykeys, 3) || !strval(get(delivery, "mode"), 1, 16) ||
        strcmp(get(delivery, "mode")->valuestring, "webhook") ||
        !strval(get(delivery, "url"), 9, CALLBACK_URL_MAX))
        return rpc_error(id, -32602, "Invalid event, device filter, or webhook delivery", NULL);
    if (get(params, "cursor") && !cJSON_IsNull(get(params, "cursor")))
        return rpc_error(id, -32602, "This event does not support replay cursors", NULL);
    const cJSON *max_age = get(params, "maxAgeMs");
    if (max_age && (!cJSON_IsNumber(max_age) || max_age->valuedouble < 0 ||
        max_age->valuedouble > 9007199254740991.0 || floor(max_age->valuedouble) != max_age->valuedouble))
        return rpc_error(id, -32602, "maxAgeMs must be a nonnegative integer", NULL);
    const char *url = get(delivery, "url")->valuestring;
    destination_t *dest = calloc(1, sizeof(*dest));
    bool valid_url = dest && parse_callback(url, dest);
    free(dest);
    if (!valid_url) return rpc_error(id, -32015, "CallbackEndpointError", "invalid_url");
    const char *const parts[] = {s_owner, url, EVENT_NAME, s_device};
    char subhash[65], subid[69];
    if (!digest_parts(subhash, parts, 4)) return rpc_error(id, -32603, "Internal error", NULL);
    snprintf(subid, sizeof(subid), "sub_%s", subhash);
    if (remove) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_sub.active && constant_equal(s_sub.owner, s_owner) && !strcmp(s_sub.id, subid)) {
            subscription_t cleared = { .version = 1 };
            esp_err_t err = save_subscription(&cleared);
            if (err != ESP_OK) {
                xSemaphoreGive(s_lock);
                return rpc_error(id, -32603, "Could not persist unsubscribe", NULL);
            }
            wipe(&s_sub, sizeof(s_sub));
            ++s_generation;
            s_verified_until = 0;
        }
        xSemaphoreGive(s_lock);
        return rpc_result(id, cJSON_CreateObject());
    }
    if (!clock_ready()) return rpc_error(id, -32015, "CallbackEndpointError", "clock_not_synchronized");
    const cJSON *secret = get(delivery, "secret"), *ttl = get(params, "ttlMs");
    unsigned char decoded[64]; size_t decoded_len = 0;
    bool valid_secret = strval(secret, 38, SECRET_MAX) &&
        decode_secret(secret->valuestring, decoded, &decoded_len);
    wipe(decoded, sizeof(decoded));
    if (!valid_secret) return rpc_error(id, -32602, "Expected whsec_ signing key containing 24 to 64 bytes", NULL);
    int64_t granted_ms = SUB_TTL_MS;
    if (ttl && !cJSON_IsNull(ttl)) {
        if (!cJSON_IsNumber(ttl) || ttl->valuedouble < 0 ||
            ttl->valuedouble > 9007199254740991.0 || floor(ttl->valuedouble) != ttl->valuedouble)
            return rpc_error(id, -32602, "ttlMs must be null or a nonnegative integer", NULL);
        if (ttl->valuedouble < granted_ms) granted_ms = (int64_t)ttl->valuedouble;
        if (granted_ms < 1000) granted_ms = 1000; /* Server minimum avoids refresh storms. */
    }
    subscription_t *candidate = calloc(1, sizeof(*candidate));
    if (!candidate) return rpc_error(id, -32603, "Internal error", NULL);
    candidate->version = 1;
    candidate->active = true;
    snprintf(candidate->id, sizeof(candidate->id), "%s", subid);
    snprintf(candidate->owner, sizeof(candidate->owner), "%s", s_owner);
    snprintf(candidate->device_id, sizeof(candidate->device_id), "%s", s_device);
    snprintf(candidate->url, sizeof(candidate->url), "%s", url);
    snprintf(candidate->secret, sizeof(candidate->secret), "%s", secret->valuestring);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t generation = s_generation;
    bool matching = s_sub.active && !strcmp(s_sub.id, subid) && constant_equal(s_sub.owner, s_owner);
    if (s_sub.active && s_sub.expires_ms > now_ms() && !matching) {
        xSemaphoreGive(s_lock);
        wipe(candidate, sizeof(*candidate)); free(candidate);
        return rpc_error(id, -32000, "Only one dot subscription is supported; unsubscribe the current one first", NULL);
    }
    bool cached = matching && !strcmp(s_sub.secret, candidate->secret) &&
                  s_verified_until > now_ms() / 1000;
    if (matching && strcmp(s_sub.secret, candidate->secret)) {
        snprintf(candidate->previous_secret, sizeof(candidate->previous_secret), "%s", s_sub.secret);
        candidate->previous_secret_until = now_ms() / 1000 + KEY_ROTATION_SECONDS;
    } else if (matching && s_sub.previous_secret_until > now_ms() / 1000) {
        snprintf(candidate->previous_secret, sizeof(candidate->previous_secret), "%s", s_sub.previous_secret);
        candidate->previous_secret_until = s_sub.previous_secret_until;
    }
    xSemaphoreGive(s_lock);
    const char *reason = "challenge_failed";
    if (!cached && !verify_callback(candidate, &reason)) {
        wipe(candidate, sizeof(*candidate)); free(candidate);
        return rpc_error(id, -32015, "CallbackEndpointError", reason);
    }
    candidate->expires_ms = now_ms() + granted_ms;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_generation != generation) {
        xSemaphoreGive(s_lock);
        wipe(candidate, sizeof(*candidate)); free(candidate);
        return rpc_error(id, -32000, "Subscription changed; retry the request", NULL);
    }
    esp_err_t err = save_subscription(candidate);
    if (err == ESP_OK) {
        s_sub = *candidate;
        s_verified_until = now_ms() / 1000 + VERIFY_CACHE_SECONDS;
        ++s_generation;
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        wipe(candidate, sizeof(*candidate)); free(candidate);
        return rpc_error(id, -32603, "Could not persist subscription", NULL);
    }
    char expiry[25]; iso_time(candidate->expires_ms, expiry);
    wipe(candidate, sizeof(*candidate)); free(candidate);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "id", subid);
    cJSON_AddStringToObject(result, "refreshBefore", expiry);
    cJSON_AddNullToObject(result, "cursor");
    cJSON_AddBoolToObject(result, "truncated", false);
    return rpc_result(id, result);
}

esp_err_t dot_mcp_init(const dot_config_t *config)
{
    if (!config || !config->device_id[0]) return ESP_ERR_INVALID_ARG;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_device, sizeof(s_device), "%s", config->device_id);
    s_owner[0] = 0;
    const char *const bearer_parts[] = {"esp32-dot-owner-v1", config->mcp_bearer};
    const char *const scope_parts[] = {"esp32-dot-private-tunnel-owner-v1", config->tunnel_id};
    /* The explicit mode flag wins over any old bearer remaining in NVS, just
     * as it does in the tunnel transport's authorization decision. */
    const bool scope_mode = config->tunnel_scope_auth && config->tunnel_id[0];
    const bool bearer_mode = !config->tunnel_scope_auth && config->mcp_bearer[0];
    if ((bearer_mode && !digest_parts(s_owner, bearer_parts, 2)) ||
        (scope_mode && !digest_parts(s_owner, scope_parts, 2))) {
        xSemaphoreGive(s_lock); return ESP_FAIL;
    }
    wipe(&s_sub, sizeof(s_sub));
    nvs_handle_t handle;
    esp_err_t err = nvs_open("dot_events", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t size = sizeof(s_sub);
        err = nvs_get_blob(handle, "subscription", &s_sub, &size);
        nvs_close(handle);
        if (err != ESP_OK || size != sizeof(s_sub) || s_sub.version != 1 ||
            !memchr(s_sub.id, 0, sizeof(s_sub.id)) ||
            !memchr(s_sub.owner, 0, sizeof(s_sub.owner)) ||
            !memchr(s_sub.device_id, 0, sizeof(s_sub.device_id)) ||
            !memchr(s_sub.url, 0, sizeof(s_sub.url)) ||
            !memchr(s_sub.secret, 0, sizeof(s_sub.secret)) ||
            !memchr(s_sub.previous_secret, 0, sizeof(s_sub.previous_secret)) ||
            strcmp(s_sub.owner, s_owner) || strcmp(s_sub.device_id, s_device))
            wipe(&s_sub, sizeof(s_sub));
    }
    ++s_generation;
    s_verified_until = 0;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool dot_mcp_subscription_ready(void)
{
    if (!s_lock || !clock_ready()) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ready = s_sub.active && s_sub.expires_ms > now_ms() &&
                 constant_equal(s_sub.owner, s_owner) && !strcmp(s_sub.device_id, s_device);
    xSemaphoreGive(s_lock);
    return ready;
}

void dot_mcp_subscription_clear(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    subscription_t cleared = { .version = 1 };
    esp_err_t err = save_subscription(&cleared);
    wipe(&s_sub, sizeof(s_sub));
    ++s_generation;
    s_verified_until = 0;
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) ESP_LOGE(TAG, "Unable to persist cleared subscription (%s)", esp_err_to_name(err));
}

cJSON *dot_mcp_dispatch(const cJSON *request, bool authenticated)
{
    unsigned budget = 256;
    const cJSON *id = get(request, "id");
    const cJSON *method = get(request, "method");
    const cJSON *params = get(request, "params");
    if (!cJSON_IsObject(request) || !safe_json(request, 0, &budget) ||
        !strval(get(request, "jsonrpc"), 3, 3) || strcmp(get(request, "jsonrpc")->valuestring, "2.0") ||
        !strval(method, 1, 128) || (params && !cJSON_IsObject(params)) ||
        (id && !cJSON_IsString(id) && !cJSON_IsNumber(id)) ||
        (cJSON_IsString(id) && !strval(id, 0, 128)) ||
        (cJSON_IsNumber(id) && (floor(id->valuedouble) != id->valuedouble || fabs(id->valuedouble) > 9007199254740991.0)))
        return rpc_error(NULL, -32600, "Invalid Request", NULL);
    /* Notifications never create/refresh subscriptions or queue audio. */
    if (!id) return NULL;
    if (!authenticated || !s_lock || !s_owner[0])
        return rpc_error(id, -32001, "Unauthorized", NULL);
    const cJSON *meta = get(params, "_meta");
    const cJSON *version = get(meta, "io.modelcontextprotocol/protocolVersion");
    const cJSON *capabilities = get(meta, "io.modelcontextprotocol/clientCapabilities");
    const cJSON *client_info = get(meta, "io.modelcontextprotocol/clientInfo");
    if (!cJSON_IsObject(meta) || !strval(version, 1, 32) || !cJSON_IsObject(capabilities) ||
        (client_info && (!cJSON_IsObject(client_info) ||
            !strval(get(client_info, "name"), 1, 128) || !strval(get(client_info, "version"), 1, 128))))
        return rpc_error(id, -32602, "MCP 2.0 requires per-request protocolVersion and clientCapabilities in params._meta", NULL);
    if (strcmp(version->valuestring, PROTOCOL_VERSION)) {
        cJSON *error = rpc_error(id, -32022, "Unsupported protocol version", NULL);
        cJSON *data = cJSON_AddObjectToObject(cJSON_GetObjectItemCaseSensitive(error, "error"), "data");
        cJSON_AddStringToObject(data, "requested", version->valuestring);
        cJSON_AddItemToArray(cJSON_AddArrayToObject(data, "supported"), cJSON_CreateString(PROTOCOL_VERSION));
        return error;
    }
    const char *name = method->valuestring;
    if (!strcmp(name, "server/discover")) {
        static const char *const keys[] = {"_meta"};
        if (!only_keys(params, keys, 1)) return rpc_error(id, -32602, "Discovery accepts only _meta", NULL);
        return rpc_result(id, discovery());
    }
    if (!strcmp(name, "ping")) return rpc_result(id, cJSON_CreateObject());
    if (!strcmp(name, "tools/list")) {
        if (!empty_or_cursor(params)) return rpc_error(id, -32602, "Invalid list parameters", NULL);
        return rpc_result(id, tool_catalog());
    }
    if (!strcmp(name, "events/list")) {
        if (!empty_or_cursor(params)) return rpc_error(id, -32602, "Invalid list parameters", NULL);
        return rpc_result(id, event_catalog());
    }
    if (!strcmp(name, "tools/call")) return call_tool(id, params);
    if (!strcmp(name, "events/subscribe")) return subscription_request(id, params, false);
    if (!strcmp(name, "events/unsubscribe")) return subscription_request(id, params, true);
    return rpc_error(id, -32601, "Method not found", NULL);
}

esp_err_t dot_mcp_publish_utterance(const char *request_id, const char *text, const char *input_kind)
{
    if (!request_id || !*request_id || strlen(request_id) > DOT_REQUEST_ID_MAX ||
        !text || !*text || strlen(text) > DOT_TEXT_MAX || !input_kind ||
        !*input_kind || strlen(input_kind) > 31) return ESP_ERR_INVALID_ARG;
    if (!dot_mcp_subscription_ready()) return ESP_ERR_INVALID_STATE;
    subscription_t *snapshot = calloc(1, sizeof(*snapshot));
    webhook_response_t *response = calloc(1, sizeof(*response));
    if (!snapshot || !response) { free(snapshot); free(response); return ESP_ERR_NO_MEM; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *snapshot = s_sub;
    uint32_t generation = s_generation;
    xSemaphoreGive(s_lock);
    const char *const parts[] = {s_device, request_id};
    char eventhash[65], event_id[69], occurred[25];
    if (!digest_parts(eventhash, parts, 2)) { free(snapshot); free(response); return ESP_FAIL; }
    snprintf(event_id, sizeof(event_id), "evt_%s", eventhash);
    iso_time(now_ms(), occurred);
    cJSON *event = cJSON_CreateObject();
    cJSON_AddStringToObject(event, "eventId", event_id);
    cJSON_AddStringToObject(event, "name", EVENT_NAME);
    cJSON_AddStringToObject(event, "timestamp", occurred);
    cJSON_AddNullToObject(event, "cursor");
    cJSON *data = cJSON_AddObjectToObject(event, "data");
    cJSON_AddStringToObject(data, "device_id", s_device);
    cJSON_AddStringToObject(data, "request_id", request_id);
    cJSON_AddStringToObject(data, "text", text);
    cJSON_AddStringToObject(data, "input_kind", input_kind);
    char *body = cJSON_PrintUnformatted(event);
    cJSON_Delete(event);
    esp_err_t err = body ? ESP_FAIL : ESP_ERR_NO_MEM;
    for (unsigned attempt = 0; body && attempt < 3; ++attempt) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool current = s_generation == generation && s_sub.active &&
                       s_sub.expires_ms > now_ms() && constant_equal(s_sub.owner, s_owner);
        xSemaphoreGive(s_lock);
        if (!current || !dot_controller_is_pending(request_id)) { err = ESP_ERR_INVALID_STATE; break; }
        int status = 0;
        err = post_signed(snapshot, event_id, body, response, &status);
        if (err == ESP_OK && status >= 200 && status < 300) break;
        if (status == 410) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_generation == generation) {
                subscription_t cleared = { .version = 1 };
                save_subscription(&cleared);
                wipe(&s_sub, sizeof(s_sub)); ++s_generation;
                s_verified_until = 0;
            }
            xSemaphoreGive(s_lock);
        }
        if (err == ESP_OK) err = ESP_FAIL;
        if (status == 410 || status == 413 || (status >= 400 && status < 500 &&
            status != 408 && status != 429)) break;
        if (attempt < 2) vTaskDelay(pdMS_TO_TICKS(500U << attempt));
    }
    cJSON_free(body);
    wipe(snapshot, sizeof(*snapshot)); free(snapshot);
    free(response);
    return err;
}
