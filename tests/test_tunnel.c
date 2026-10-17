/* Native tests execute the production protocol helpers, not a reimplementation.
 * Compile dot_tunnel.c with DOT_TUNNEL_PROTOCOL_ONLY and link pinned cJSON.
 */
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "dot_tunnel.h"

static void test_relative_deadlines(void)
{
    static const struct { const char *wire; int64_t expected; } valid[] = {
        {"0s", 0}, {"000ms", 0}, {"1ns", 0}, {"999ns", 0},
        {"1000ns", 1}, {"1us", 1}, {"5ms", 5000}, {"30s", 30000000},
        {"4500ms", 4500000}, {"1m", 60000000}, {"1h", INT64_C(3600000000)},
        {"000001s", 1000000},
        {"9223372036854775807ns", INT64_C(9223372036854775)},
        {"9223372036854775us", INT64_C(9223372036854775)},
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        int64_t actual = -1;
        assert(dot_tunnel_parse_timeout_us(valid[i].wire, &actual));
        assert(actual == valid[i].expected);
    }
    static const char *invalid[] = {
        "", "30", "s", " 1s", "1s ", "\t1s", "+1s", "-1s", "4.5s",
        "1m30s", "1e3s", "30d", "1S", "1MS", "1ms\n", "0x10s",
        "9223372036854775808ns", "9223372036854776us", "9223372037s",
        "999999999999999999999999h", "1\x80s",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        int64_t output = 42;
        assert(!dot_tunnel_parse_timeout_us(invalid[i], &output));
        assert(output == 42);
    }
    int64_t output = 0;
    assert(!dot_tunnel_parse_timeout_us(NULL, &output));
    assert(!dot_tunnel_parse_timeout_us("1s", NULL));
}

static bool authenticated(const char *wire, const char *secret)
{
    cJSON *command = cJSON_Parse(wire);
    assert(command);
    bool result = dot_tunnel_command_authenticated(command, secret);
    cJSON_Delete(command);
    return result;
}

static void test_connector_identity(void)
{
    assert(authenticated("{\"headers\":{\"Authorization\":[\"Bearer device-secret\"]}}", "device-secret"));
    assert(authenticated("{\"headers\":{\"authorization\":[\"bearer device-secret\"]}}", "device-secret"));
    assert(!authenticated("{}", "device-secret"));
    assert(!authenticated("{\"headers\":null}", "device-secret"));
    assert(!authenticated("{\"headers\":{\"Authorization\":\"Bearer device-secret\"}}", "device-secret"));
    assert(!authenticated("{\"headers\":{\"Authorization\":[]}}", "device-secret"));
    assert(!authenticated("{\"headers\":{\"Authorization\":[9]}}", "device-secret"));
    assert(!authenticated("{\"headers\":{\"Authorization\":[\"Bearer device-secret\",\"Bearer device-secret\"]}}", "device-secret"));
    assert(!authenticated("{\"headers\":{\"Authorization\":[\"Bearer device-secret\"],\"authorization\":[\"Bearer device-secret\"]}}", "device-secret"));
    assert(!authenticated("{\"headers\":{\"Authorization\":[\"Bearer Device-secret\"]}}", "device-secret"));
    assert(!authenticated("{\"headers\":{\"Authorization\":[\"Basic device-secret\"]}}", "device-secret"));
    assert(!authenticated("{\"headers\":{\"Authorization\":[\"Bearer device-secret-extra\"]}}", "device-secret"));
    assert(!authenticated("{\"headers\":{\"Authorization\":[\"Bearer device-secret \"]}}", "device-secret"));
    assert(!authenticated("{\"headers\":{\"Authorization\":[\"Bearer  device-secret\"]}}", "device-secret"));
    assert(!authenticated("{\"headers\":{\"Authorization\":[\"Bearer \"]}}", ""));
    assert(!authenticated("{}", NULL));
    assert(!dot_tunnel_command_authorized(NULL, "", true));
    cJSON *no_auth = cJSON_Parse("{}");
    assert(no_auth);
    assert(!dot_tunnel_command_authorized(no_auth, "", false));
    assert(!dot_tunnel_command_authorized(no_auth, "secret", false));
    assert(dot_tunnel_command_authorized(no_auth, "", true));
    assert(dot_tunnel_command_authorized(no_auth, "unused-bearer", true));
    cJSON_Delete(no_auth);
    cJSON *with_auth = cJSON_Parse("{\"headers\":{\"Authorization\":[\"Bearer secret\"]}}");
    assert(with_auth && dot_tunnel_command_authorized(with_auth, "secret", false));
    assert(!dot_tunnel_command_authorized(with_auth, "different-secret", false));
    cJSON_Delete(with_auth);
}

static const cJSON *field(const cJSON *object, const char *name)
{
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

static void test_http_protocol_version(void)
{
    cJSON *command = cJSON_Parse("{\"headers\":{\"MCP-Protocol-Version\":[\"2026-07-28\"]},"
        "\"jsonrpc\":{\"id\":1,\"params\":{\"_meta\":{"
        "\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"}}}}");
    assert(command && dot_tunnel_protocol_header_matches(command));
    cJSON *headers = cJSON_GetObjectItemCaseSensitive(command, "headers");
    cJSON *versions = cJSON_GetObjectItemCaseSensitive(headers, "MCP-Protocol-Version");
    cJSON_ReplaceItemInArray(versions, 0, cJSON_CreateString("2025-06-18"));
    assert(!dot_tunnel_protocol_header_matches(command));
    cJSON_ReplaceItemInArray(versions, 0, cJSON_CreateString("2026-07-28"));
    cJSON_AddItemToArray(versions, cJSON_CreateString("2026-07-28"));
    assert(!dot_tunnel_protocol_header_matches(command));
    cJSON_DeleteItemFromArray(versions, 1);
    cJSON_AddItemToObject(headers, "mcp-protocol-version", cJSON_Duplicate(versions, true));
    assert(!dot_tunnel_protocol_header_matches(command));
    cJSON_Delete(command);
    command = cJSON_Parse("{\"jsonrpc\":{\"id\":1,\"params\":{\"_meta\":{"
        "\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"}}}}");
    assert(!dot_tunnel_protocol_header_matches(command));
    cJSON_Delete(command);
    command = cJSON_Parse("{\"headers\":{\"MCP-Protocol-Version\":[\"2026-07-28\"]},"
        "\"jsonrpc\":{\"id\":1,\"params\":{}}}");
    assert(!dot_tunnel_protocol_header_matches(command));
    cJSON_Delete(command);
    command = cJSON_Parse("{\"jsonrpc\":{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\"}}");
    assert(dot_tunnel_protocol_header_matches(command));
    cJSON_Delete(command);
    const int codes[] = {-32700, -32600, -32602, -32021, -32022};
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        cJSON *rpc = cJSON_CreateObject();
        cJSON_AddNumberToObject(cJSON_AddObjectToObject(rpc, "error"), "code", codes[i]);
        assert(dot_tunnel_rpc_http_status(rpc) == 400);
        cJSON_Delete(rpc);
    }
    cJSON *rpc = cJSON_Parse("{\"error\":{\"code\":-32601}}");
    assert(dot_tunnel_rpc_http_status(rpc) == 200);
    cJSON_Delete(rpc);
    rpc = cJSON_Parse("{\"result\":{}}");
    assert(dot_tunnel_rpc_http_status(rpc) == 200);
    cJSON_Delete(rpc);
}

static void test_correlations_and_rpc_preservation(void)
{
    cJSON *command = cJSON_Parse("{\"request_id\":\"opaque/request-123\",\"channel\":\"main\","
        "\"shard_token\":\"SHARD-SECRET\",\"unknown_future_field\":true}");
    cJSON *rpc = cJSON_Parse("{\"jsonrpc\":\"2.0\",\"id\":9,\"error\":{\"code\":-32003,"
        "\"message\":\"capability denied\",\"data\":{\"unicode\":\"\\u7231\\ud83d\\ude00\"}}}");
    cJSON *expected = cJSON_Duplicate(rpc, true);
    assert(command && rpc && expected);
    cJSON *response = dot_tunnel_response_envelope(command, rpc, 200, "jsonrpc_response");
    assert(response);
    assert(!strcmp(field(response, "request_id")->valuestring, "opaque/request-123"));
    assert(!strcmp(field(response, "channel")->valuestring, "main"));
    assert(field(response, "resp_code")->valueint == 200);
    assert(!strcmp(field(response, "resp_type")->valuestring, "jsonrpc_response"));
    assert(cJSON_Compare(field(response, "resp_json"), expected, true));
    assert(!field(response, "shard_token"));
    assert(!field(response, "unknown_future_field"));
    const cJSON *content_types = field(field(response, "resp_headers"), "Content-Type");
    assert(cJSON_IsArray(content_types) && cJSON_GetArraySize(content_types) == 1);
    assert(!strcmp(cJSON_GetArrayItem(content_types, 0)->valuestring, "application/json"));
    char *serialized = cJSON_PrintUnformatted(response);
    assert(serialized && !strstr(serialized, "SHARD-SECRET"));
    cJSON_free(serialized);
    cJSON_Delete(response);
    cJSON_Delete(expected);
    cJSON_Delete(command);
}

static void test_response_discriminators(void)
{
    cJSON *command = cJSON_Parse("{\"request_id\":\"r\"}");
    assert(command);
    const char *types[] = {"notify_ack", "session_termination_response", "jsonrpc_notify"};
    const int statuses[] = {202, 204, 200};
    for (unsigned i = 0; i < 3; ++i) {
        cJSON *notification = i == 2 ? cJSON_Parse("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\"}") : NULL;
        cJSON *response = dot_tunnel_response_envelope(command, notification, statuses[i], types[i]);
        assert(response);
        assert(!strcmp(field(response, "channel")->valuestring, "main"));
        assert(!strcmp(field(response, "resp_type")->valuestring, types[i]));
        assert(field(response, "resp_code")->valueint == statuses[i]);
        assert((field(response, "resp_json") != NULL) == (i == 2));
        cJSON_Delete(response);
    }
    cJSON *unauthorized = dot_tunnel_response_envelope(command, NULL, 401, "jsonrpc_response");
    assert(unauthorized);
    const cJSON *challenge = field(field(unauthorized, "resp_headers"), "WWW-Authenticate");
    assert(cJSON_IsArray(challenge));
    assert(!strcmp(cJSON_GetArrayItem(challenge, 0)->valuestring, "Bearer realm=\"esp32-dot\""));
    cJSON_Delete(unauthorized);
    assert(!dot_tunnel_response_envelope(command, NULL, 99, "jsonrpc_response"));
    assert(!dot_tunnel_response_envelope(command, NULL, 600, "jsonrpc_response"));
    assert(!dot_tunnel_response_envelope(command, NULL, 200, NULL));
    cJSON_Delete(command);
    command = cJSON_Parse("{\"request_id\":3}");
    assert(!dot_tunnel_response_envelope(command, NULL, 200, "jsonrpc_response"));
    cJSON_Delete(command);
    command = cJSON_Parse("{\"request_id\":\"r\",\"channel\":1}");
    assert(!dot_tunnel_response_envelope(command, NULL, 200, "jsonrpc_response"));
    cJSON_Delete(command);
}

int main(void)
{
    test_relative_deadlines();
    test_connector_identity();
    test_http_protocol_version();
    test_correlations_and_rpc_preservation();
    test_response_discriminators();
    puts("PASS: production tunnel duration, authentication, correlation, Unicode/error preservation, response discriminator fixtures");
    return 0;
}
