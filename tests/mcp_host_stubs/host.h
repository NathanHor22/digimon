#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_NVS_NOT_FOUND 0x1102
typedef int esp_err_t;
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY 0xffffffff
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGE(...) ((void)0)
#define AF_INET 2
#define SOCK_STREAM 1
#define INET_ADDRSTRLEN 16
struct in_addr { uint32_t s_addr; };
struct sockaddr { uint16_t family; char bytes[14]; };
struct sockaddr_in { uint16_t family; uint16_t port; struct in_addr sin_addr; char bytes[8]; };
struct addrinfo { int ai_flags, ai_family, ai_socktype, ai_protocol; size_t ai_addrlen; struct sockaddr *ai_addr; char *ai_canonname; struct addrinfo *ai_next; };
uint32_t ntohl(uint32_t value);
int getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **);
void freeaddrinfo(struct addrinfo *);
const char *inet_ntop(int, const void *, char *, size_t);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t, unsigned long);
int xSemaphoreGive(SemaphoreHandle_t);
void vTaskDelay(unsigned);
void esp_fill_random(void *, size_t);
int64_t esp_timer_get_time(void);
const char *esp_err_to_name(int);
int esp_crt_bundle_attach(void *);
typedef unsigned nvs_handle_t;
#define NVS_READWRITE 1
#define NVS_READONLY 0
int nvs_open(const char *, int, nvs_handle_t *);
int nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
int nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
int nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
typedef struct host_client *esp_http_client_handle_t;
typedef struct { int event_id; void *user_data; int data_len; char *data; esp_http_client_handle_t client; char *header_key; char *header_value; } esp_http_client_event_t;
#define HTTP_EVENT_ON_DATA 1
#define HTTP_EVENT_ON_HEADER 2
#define HTTP_METHOD_POST 1
typedef struct {
 const char *url, *common_name;
 int (*crt_bundle_attach)(void *);
 int timeout_ms;
 bool disable_auto_redirect;
 int max_authorization_retries, method;
 esp_err_t (*event_handler)(esp_http_client_event_t *);
 void *user_data;
 int buffer_size, buffer_size_tx;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
int esp_http_client_set_header(esp_http_client_handle_t, const char *, const char *);
int esp_http_client_set_post_field(esp_http_client_handle_t, const char *, int);
int esp_http_client_set_timeout_ms(esp_http_client_handle_t, int);
int esp_http_client_open(esp_http_client_handle_t, int);
int esp_http_client_write(esp_http_client_handle_t, const char *, int);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t, char *, int);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
int esp_http_client_close(esp_http_client_handle_t);
int esp_http_client_perform(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
void esp_http_client_cleanup(esp_http_client_handle_t);
typedef struct { void *handle; void *algorithm; void *buffer; size_t buffer_size; bool hmac; } mbedtls_md_context_t;
typedef struct { int unused; } mbedtls_md_info_t;
#define MBEDTLS_MD_SHA256 1
const mbedtls_md_info_t *mbedtls_md_info_from_type(int);
void mbedtls_md_init(mbedtls_md_context_t *);
int mbedtls_md_setup(mbedtls_md_context_t *, const mbedtls_md_info_t *, int);
int mbedtls_md_starts(mbedtls_md_context_t *);
int mbedtls_md_update(mbedtls_md_context_t *, const unsigned char *, size_t);
int mbedtls_md_finish(mbedtls_md_context_t *, unsigned char *);
void mbedtls_md_free(mbedtls_md_context_t *);
int mbedtls_md_hmac_starts(mbedtls_md_context_t *, const unsigned char *, size_t);
int mbedtls_md_hmac_update(mbedtls_md_context_t *, const unsigned char *, size_t);
int mbedtls_md_hmac_finish(mbedtls_md_context_t *, unsigned char *);
int mbedtls_base64_encode(unsigned char *, size_t, size_t *, const unsigned char *, size_t);
int mbedtls_base64_decode(unsigned char *, size_t, size_t *, const unsigned char *, size_t);
static inline struct tm *host_gmtime(const time_t *time, struct tm *tm)
{ return gmtime_s(tm, time) == 0 ? tm : NULL; }
#define gmtime_r(a,b) host_gmtime(a,b)
