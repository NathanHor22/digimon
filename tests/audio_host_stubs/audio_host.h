#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_HTTP_CONNECTING 0x7006
#define ESP_ERR_HTTP_EAGAIN 0x7007
typedef int esp_err_t;
typedef void *SemaphoreHandle_t;
typedef void *StreamBufferHandle_t;
typedef void *TaskHandle_t;
typedef struct { int unused; } StaticStreamBuffer_t;
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
#define pdPASS 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOG_WARN 2
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
void *heap_caps_malloc(size_t, unsigned);
void *heap_caps_calloc(size_t, size_t, unsigned);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
int xSemaphoreTake(SemaphoreHandle_t, unsigned);
int xSemaphoreGive(SemaphoreHandle_t);
StreamBufferHandle_t xStreamBufferCreateStatic(size_t, size_t, uint8_t *, StaticStreamBuffer_t *);
size_t xStreamBufferBytesAvailable(StreamBufferHandle_t);
size_t xStreamBufferSend(StreamBufferHandle_t, const void *, size_t, unsigned);
size_t xStreamBufferReceive(StreamBufferHandle_t, void *, size_t, unsigned);
int xStreamBufferReset(StreamBufferHandle_t);
void vTaskDelay(unsigned);
unsigned ulTaskNotifyTake(int, unsigned);
int xTaskNotifyGive(TaskHandle_t);
int xTaskCreatePinnedToCore(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *, unsigned);
uint32_t esp_random(void);
int64_t esp_timer_get_time(void);
void esp_log_level_set(const char *, int);
int esp_crt_bundle_attach(void *);

typedef void *i2s_chan_handle_t;
typedef struct { int unused; unsigned dma_desc_num, dma_frame_num; bool auto_clear; } i2s_chan_config_t;
typedef struct {
    struct { int unused; } clk_cfg;
    struct { unsigned slot_mask; } slot_cfg;
    struct { int mclk, bclk, ws, dout, din; } gpio_cfg;
} i2s_std_config_t;
#define I2S_NUM_0 0
#define I2S_NUM_1 1
#define I2S_ROLE_MASTER 1
#define I2S_DATA_BIT_WIDTH_32BIT 32
#define I2S_SLOT_MODE_MONO 1
#define I2S_STD_SLOT_LEFT 1
#define I2S_GPIO_UNUSED -1
#define GPIO_NUM_4 4
#define GPIO_NUM_5 5
#define GPIO_NUM_6 6
#define GPIO_NUM_7 7
#define GPIO_NUM_15 15
#define GPIO_NUM_16 16
#define I2S_CHANNEL_DEFAULT_CONFIG(a,b) {0}
#define I2S_STD_CLK_DEFAULT_CONFIG(a) {0}
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(a,b) {0}
int i2s_new_channel(const i2s_chan_config_t *, i2s_chan_handle_t *, i2s_chan_handle_t *);
int i2s_channel_init_std_mode(i2s_chan_handle_t, const i2s_std_config_t *);
int i2s_channel_enable(i2s_chan_handle_t);
int i2s_channel_disable(i2s_chan_handle_t);
int i2s_channel_read(i2s_chan_handle_t, void *, size_t, size_t *, unsigned);
int i2s_channel_write(i2s_chan_handle_t, const void *, size_t, size_t *, unsigned);
int i2s_channel_preload_data(i2s_chan_handle_t, const void *, size_t, size_t *);

typedef struct host_audio_client *esp_http_client_handle_t;
typedef struct {
    int event_id;
    void *user_data;
    int data_len;
    char *data;
    char *header_key, *header_value;
    esp_http_client_handle_t client;
} esp_http_client_event_t;
#define HTTP_EVENT_ON_HEADER 1
#define HTTP_EVENT_ON_DATA 2
#define HTTP_METHOD_POST 1
typedef struct {
    const char *url;
    int method;
    int (*crt_bundle_attach)(void *);
    int timeout_ms, buffer_size, buffer_size_tx;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
    bool is_async, disable_auto_redirect;
    int max_authorization_retries;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
int esp_http_client_set_header(esp_http_client_handle_t, const char *, const char *);
int esp_http_client_set_post_field(esp_http_client_handle_t, const char *, int);
int esp_http_client_get_post_field(esp_http_client_handle_t, char **);
int esp_http_client_get_status_code(esp_http_client_handle_t);
int esp_http_client_open(esp_http_client_handle_t, int);
int esp_http_client_write(esp_http_client_handle_t, const char *, int);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t, char *, int);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
void esp_http_client_cleanup(esp_http_client_handle_t);
