#include "dot_audio.h"
#include "dot_clock.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "driver/i2s_std.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#define MIC_FRAME 320
#define SPK_FRAME 240
#define RECORD_SAMPLES (DOT_SAMPLE_RATE * DOT_RECORD_SECONDS)
#define PCM_BUFFER_BYTES (64 * 1024)
#define PCM_PREFILL_BYTES (DOT_SPEAKER_RATE * sizeof(int16_t) / 8)
#define HTTP_RESPONSE_LIMIT (16 * 1024)
#define HTTP_DEADLINE_US (120LL * 1000 * 1000)
#define PREROLL_SAMPLES (DOT_SAMPLE_RATE * 350 / 1000)
#define MIN_VOICED_SAMPLES (DOT_SAMPLE_RATE * 200 / 1000)
#define END_SILENCE_SAMPLES (DOT_SAMPLE_RATE * 700 / 1000)

static const char *TAG = "dot_audio";
static i2s_chan_handle_t s_microphone;
static i2s_chan_handle_t s_speaker;
static int16_t *s_capture;
static size_t s_capture_samples;
static bool s_recording;
static bool s_record_full;
static int16_t *s_preroll;
static size_t s_preroll_write, s_preroll_count;
static size_t s_voiced_samples, s_silent_samples;
static bool s_candidate, s_detected_ready;
static uint32_t s_noise_floor = 120;
static atomic_uint s_capture_generation;
static atomic_uint s_volume = 100;
static atomic_uint s_speech_level;
static atomic_bool s_speaking;
static atomic_bool s_muted;
static atomic_bool s_handsfree;
static SemaphoreHandle_t s_capture_lock;
static SemaphoreHandle_t s_speak_lock;
static SemaphoreHandle_t s_pcm_done;
static StreamBufferHandle_t s_pcm;
static StaticStreamBuffer_t s_pcm_state;
static uint8_t *s_pcm_storage;
static TaskHandle_t s_pcm_task;
static atomic_uint s_cancel_generation;
static unsigned s_pcm_generation;
static atomic_bool s_pcm_input_finished;
static atomic_bool s_pcm_failed;
static atomic_bool s_pcm_playing;
static bool s_initialized;

static bool cancelled(unsigned generation) {
    return generation != atomic_load(&s_cancel_generation);
}

/* Called with s_capture_lock held. Keep a manual recording intact when merely
 * changing the hands-free preference; microphone mute/cancel clear it explicitly. */
static void clear_detection_locked(void) {
    s_candidate = false;
    s_detected_ready = false;
    s_preroll_write = s_preroll_count = 0;
    s_voiced_samples = s_silent_samples = 0;
    if (!s_recording) s_capture_samples = 0;
}

static void preroll_append(const int16_t *samples, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        s_preroll[s_preroll_write] = samples[i];
        s_preroll_write = (s_preroll_write + 1) % PREROLL_SAMPLES;
        if (s_preroll_count < PREROLL_SAMPLES) ++s_preroll_count;
    }
}

static void capture_frame(const int16_t *samples, size_t count, unsigned capture_generation) {
    if (!count) return;
    uint64_t magnitude = 0;
    for (size_t i = 0; i < count; ++i) {
        int32_t sample = samples[i];
        magnitude += sample < 0 ? -sample : sample;
    }
    uint32_t energy = (uint32_t)(magnitude / count);
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    if (capture_generation != atomic_load(&s_capture_generation) || atomic_load(&s_muted)) {
        xSemaphoreGive(s_capture_lock);
        return;
    }
    if (s_recording) {
        size_t remaining = RECORD_SAMPLES - s_capture_samples;
        if (count > remaining) count = remaining;
        memcpy(s_capture + s_capture_samples, samples, count * sizeof(int16_t));
        s_capture_samples += count;
        s_record_full = s_capture_samples == RECORD_SAMPLES;
        xSemaphoreGive(s_capture_lock);
        return;
    }
    if (!atomic_load(&s_handsfree) || atomic_load(&s_pcm_playing) || atomic_load(&s_speaking)) {
        clear_detection_locked();
        xSemaphoreGive(s_capture_lock);
        return;
    }
    if (s_detected_ready) {
        xSemaphoreGive(s_capture_lock);
        return;
    }
    // Experimental local energy segmentation, not word/intent recognition.
    // Speech must be louder than the learned quiet-room floor. Steady noise,
    // TV and quiet speech remain difficult; the cloud gate decides addressing.
    uint32_t threshold = s_noise_floor * 3;
    if (threshold < 500) threshold = 500;
    bool voiced = energy > threshold;
    if (!s_candidate) {
        if (!voiced) {
            s_noise_floor = (s_noise_floor * 31 + energy) / 32;
            preroll_append(samples, count);
            xSemaphoreGive(s_capture_lock);
            return;
        }
        s_candidate = true;
        s_capture_samples = s_preroll_count;
        size_t first = (s_preroll_write + PREROLL_SAMPLES - s_preroll_count) % PREROLL_SAMPLES;
        for (size_t i = 0; i < s_preroll_count; ++i)
            s_capture[i] = s_preroll[(first + i) % PREROLL_SAMPLES];
        s_voiced_samples = s_silent_samples = 0;
    }
    size_t remaining = RECORD_SAMPLES - s_capture_samples;
    if (count > remaining) count = remaining;
    memcpy(s_capture + s_capture_samples, samples, count * sizeof(int16_t));
    s_capture_samples += count;
    if (voiced) { s_voiced_samples += count; s_silent_samples = 0; }
    else s_silent_samples += count;
    if (s_capture_samples == RECORD_SAMPLES || s_silent_samples >= END_SILENCE_SAMPLES) {
        if (s_voiced_samples >= MIN_VOICED_SAMPLES) {
            s_candidate = false;
            s_detected_ready = true;
        } else clear_detection_locked();
    }
    xSemaphoreGive(s_capture_lock);
}

static void microphone_task(void *arg) {
    (void)arg;
    int32_t raw[MIC_FRAME];
    int16_t converted[MIC_FRAME];
    for (;;) {
        unsigned capture_generation = atomic_load(&s_capture_generation);
        size_t received = 0;
        esp_err_t err = i2s_channel_read(s_microphone, raw, sizeof(raw), &received, 250);
        if (err != ESP_OK || received == 0) continue;
        size_t count = received / sizeof(raw[0]);
        for (size_t i = 0; i < count; ++i) {
            // Original M1307 board: left-aligned 24-bit microphone in 32-bit slots.
            int32_t sample = raw[i] >> 14;
            if (sample > INT16_MAX) sample = INT16_MAX;
            if (sample < INT16_MIN) sample = INT16_MIN;
            converted[i] = (int16_t)sample;
        }
        capture_frame(converted, count, capture_generation);
    }
}

static esp_err_t speaker_start(void) {
    int32_t silence[SPK_FRAME] = {0};
    // Clear every descriptor, including audio abandoned by a previous cancel.
    for (unsigned i = 0; i < 6; ++i) {
        size_t loaded = 0;
        esp_err_t err = i2s_channel_preload_data(s_speaker, silence, sizeof(silence), &loaded);
        if (err != ESP_OK) return err;
        if (loaded != sizeof(silence)) return ESP_ERR_INVALID_SIZE;
    }
    return i2s_channel_enable(s_speaker);
}

static void speaker_stop(unsigned generation) {
    int32_t silence[SPK_FRAME] = {0};
    // Replace the entire six-descriptor DMA ring to drain final speech frames.
    // Cancellation skips this drain and disables transmission immediately.
    for (unsigned i = 0; i < 6 && !cancelled(generation); ++i) {
        size_t written = 0;
        if (i2s_channel_write(s_speaker, silence, sizeof(silence), &written, 100) != ESP_OK) break;
    }
    (void)i2s_channel_disable(s_speaker);
}

static esp_err_t speaker_write(const uint8_t *pcm, size_t length, unsigned generation) {
    int32_t output[SPK_FRAME];
    if ((length & 1) || length > SPK_FRAME * sizeof(int16_t)) return ESP_ERR_INVALID_SIZE;
    unsigned volume = atomic_load(&s_volume);
    uint64_t magnitude = 0;
    for (size_t i = 0; i < length / 2; ++i) {
        int16_t sample = (int16_t)((uint16_t)pcm[i * 2] | ((uint16_t)pcm[i * 2 + 1] << 8));
        int32_t scaled = (int32_t)sample * (int32_t)volume / 100;
        magnitude += scaled < 0 ? -scaled : scaled;
        // Preserve the original board's amplifier scaling (avoid signed left shift).
        output[i] = scaled * 32768;
    }
    const uint8_t *bytes = (const uint8_t *)output;
    size_t total = length * 2;
    size_t sent = 0;
    while (sent < total && !cancelled(generation)) {
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_speaker, bytes + sent, total - sent, &written, 100);
        sent += written;
        if (written && length) {
            unsigned level = (unsigned)(magnitude * 100 / (length / 2) / 6000);
            if (level > 100) level = 100;
            atomic_store(&s_speech_level, level);
        }
        if (err != ESP_OK) return err;
        if (written == 0) return ESP_ERR_TIMEOUT;
    }
    return sent == total ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void pcm_task(void *arg) {
    (void)arg;
    uint8_t input[SPK_FRAME * sizeof(int16_t)];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        bool started = false;
        unsigned generation = s_pcm_generation;
        for (;;) {
            if (cancelled(generation) || atomic_load(&s_pcm_failed)) break;
            size_t available = xStreamBufferBytesAvailable(s_pcm);
            bool finished = atomic_load(&s_pcm_input_finished);
            if (!started) {
                if (!finished && available < PCM_PREFILL_BYTES) {
                    vTaskDelay(pdMS_TO_TICKS(5));
                    continue;
                }
                if (available == 0) break;
                if (speaker_start() != ESP_OK) {
                    atomic_store(&s_pcm_failed, true);
                    break;
                }
                started = true;
                atomic_store(&s_speaking, true);
            }
            if (available == 0) {
                atomic_store(&s_speech_level, 0);
                if (finished) break;
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            size_t wanted = available < sizeof(input) ? available : sizeof(input);
            wanted &= ~(size_t)1;
            if (wanted == 0) {
                // The producer only commits complete PCM16 frames.
                atomic_store(&s_pcm_failed, true);
                break;
            }
            size_t received = xStreamBufferReceive(s_pcm, input, wanted, 0);
            if (speaker_write(input, received, generation) != ESP_OK) {
                if (!cancelled(generation)) atomic_store(&s_pcm_failed, true);
                break;
            }
        }
        if (started) speaker_stop(generation);
        atomic_store(&s_speech_level, 0);
        atomic_store(&s_speaking, false);
        atomic_store(&s_pcm_playing, false);
        xSemaphoreGive(s_pcm_done);
    }
}

esp_err_t dot_audio_init(void) {
    if (s_initialized) return ESP_OK;
    s_capture_lock = xSemaphoreCreateMutex();
    s_speak_lock = xSemaphoreCreateMutex();
    s_pcm_done = xSemaphoreCreateBinary();
    s_capture = heap_caps_malloc(RECORD_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_preroll = heap_caps_malloc(PREROLL_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pcm_storage = heap_caps_malloc(PCM_BUFFER_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_capture_lock || !s_speak_lock || !s_pcm_done || !s_capture || !s_preroll || !s_pcm_storage) return ESP_ERR_NO_MEM;
    // A static stream buffer reserves one byte internally to distinguish full/empty.
    s_pcm = xStreamBufferCreateStatic(PCM_BUFFER_BYTES + 1, 1, s_pcm_storage, &s_pcm_state);
    if (!s_pcm) return ESP_ERR_NO_MEM;

    i2s_chan_config_t tx = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    tx.dma_desc_num = 6;
    tx.dma_frame_num = SPK_FRAME;
    tx.auto_clear = true;
    esp_err_t err = i2s_new_channel(&tx, &s_speaker, NULL);
    if (err != ESP_OK) return err;
    i2s_std_config_t speaker = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(DOT_SPEAKER_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, .bclk = GPIO_NUM_15, .ws = GPIO_NUM_16,
            .dout = GPIO_NUM_7, .din = I2S_GPIO_UNUSED,
        },
    };
    speaker.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    err = i2s_channel_init_std_mode(s_speaker, &speaker);
    if (err != ESP_OK) return err;

    i2s_chan_config_t rx = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    rx.dma_desc_num = 6;
    rx.dma_frame_num = MIC_FRAME;
    err = i2s_new_channel(&rx, NULL, &s_microphone);
    if (err != ESP_OK) return err;
    i2s_std_config_t microphone = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(DOT_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, .bclk = GPIO_NUM_5, .ws = GPIO_NUM_4,
            .dout = I2S_GPIO_UNUSED, .din = GPIO_NUM_6,
        },
    };
    microphone.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    err = i2s_channel_init_std_mode(s_microphone, &microphone);
    if (err != ESP_OK) return err;
    err = i2s_channel_enable(s_microphone);
    if (err != ESP_OK) return err;
    if (xTaskCreatePinnedToCore(pcm_task, "dot_pcm", 4096, NULL, 7, &s_pcm_task, 1) != pdPASS) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(microphone_task, "dot_mic", 4096, NULL, 6, NULL, 1) != pdPASS) return ESP_ERR_NO_MEM;
    s_initialized = true;
    ESP_LOGI(TAG, "Microphone 16 kHz and speaker 24 kHz ready");
    return ESP_OK;
}

esp_err_t dot_audio_record_begin(void) {
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    if (s_recording || atomic_load(&s_muted)) {
        xSemaphoreGive(s_capture_lock);
        return ESP_ERR_INVALID_STATE;
    }
    atomic_fetch_add(&s_capture_generation, 1);
    clear_detection_locked();
    s_capture_samples = 0;
    s_record_full = false;
    s_recording = true;
    xSemaphoreGive(s_capture_lock);
    return ESP_OK;
}

static void put16(uint8_t *p, uint16_t value) { p[0] = value; p[1] = value >> 8; }
static void put32(uint8_t *p, uint32_t value) { put16(p, value); put16(p + 2, value >> 16); }

/* The capture lock must remain held until the owned snapshot is complete. */
static esp_err_t capture_wav_locked(uint8_t **wav, size_t *length) {
    size_t bytes = s_capture_samples * sizeof(int16_t);
    if (bytes == 0) return ESP_ERR_INVALID_SIZE;
    uint8_t *result = heap_caps_malloc(44 + bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!result) return ESP_ERR_NO_MEM;
    memcpy(result, "RIFF", 4); put32(result + 4, (uint32_t)(bytes + 36));
    memcpy(result + 8, "WAVEfmt ", 8); put32(result + 16, 16);
    put16(result + 20, 1); put16(result + 22, 1);
    put32(result + 24, DOT_SAMPLE_RATE); put32(result + 28, DOT_SAMPLE_RATE * 2);
    put16(result + 32, 2); put16(result + 34, 16);
    memcpy(result + 36, "data", 4); put32(result + 40, (uint32_t)bytes);
    // ESP32-S3 is little endian, matching the PCM WAV payload.
    memcpy(result + 44, s_capture, bytes);
    s_capture_samples = 0;
    s_record_full = false;
    clear_detection_locked();
    atomic_fetch_add(&s_capture_generation, 1);
    *wav = result;
    *length = bytes + 44;
    return ESP_OK;
}

esp_err_t dot_audio_record_end(uint8_t **wav, size_t *length) {
    if (!wav || !length) return ESP_ERR_INVALID_ARG;
    *wav = NULL;
    *length = 0;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    if (!s_recording) {
        esp_err_t err = s_candidate || s_detected_ready ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_SIZE;
        xSemaphoreGive(s_capture_lock);
        return err;
    }
    s_recording = false;
    atomic_fetch_add(&s_capture_generation, 1);
    esp_err_t err = capture_wav_locked(wav, length);
    xSemaphoreGive(s_capture_lock);
    return err;
}

void dot_audio_record_cancel(void) {
    if (!s_initialized) return;
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    s_recording = false;
    s_record_full = false;
    s_capture_samples = 0;
    clear_detection_locked();
    atomic_fetch_add(&s_capture_generation, 1);
    xSemaphoreGive(s_capture_lock);
}

bool dot_audio_is_recording(void) {
    if (!s_initialized) return false;
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    bool result = s_recording;
    xSemaphoreGive(s_capture_lock);
    return result;
}

bool dot_audio_record_full(void) {
    if (!s_initialized) return false;
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    bool result = s_record_full;
    xSemaphoreGive(s_capture_lock);
    return result;
}

void dot_audio_set_volume(unsigned volume) {
    atomic_store(&s_volume, volume > 100 ? 100 : volume);
    if (!volume) atomic_store(&s_speech_level, 0);
}

unsigned dot_audio_volume(void) { return atomic_load(&s_volume); }
unsigned dot_audio_speech_level(void) {
    return atomic_load(&s_speaking) ? atomic_load(&s_speech_level) : 0;
}
bool dot_audio_speaking(void) { return atomic_load(&s_speaking); }
bool dot_audio_muted(void) { return atomic_load(&s_muted); }

void dot_audio_set_muted(bool muted) {
    if (!s_initialized) { atomic_store(&s_muted, muted); return; }
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    if (muted == atomic_load(&s_muted)) { xSemaphoreGive(s_capture_lock); return; }
    atomic_store(&s_muted, muted);
    if (muted) { s_recording = false; s_record_full = false; s_capture_samples = 0; }
    clear_detection_locked();
    atomic_fetch_add(&s_capture_generation, 1);
    xSemaphoreGive(s_capture_lock);
}

void dot_audio_set_handsfree(bool enabled) {
    if (!s_initialized) { atomic_store(&s_handsfree, enabled); return; }
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    // Idempotent re-application must not discard a ready candidate every frame.
    if (enabled != atomic_load(&s_handsfree)) {
        atomic_store(&s_handsfree, enabled);
        clear_detection_locked();
        s_noise_floor = 120;
        atomic_fetch_add(&s_capture_generation, 1);
    }
    xSemaphoreGive(s_capture_lock);
}

bool dot_audio_detected_ready(void) {
    if (!s_initialized) return false;
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    bool ready = s_detected_ready && !s_recording && !atomic_load(&s_muted);
    xSemaphoreGive(s_capture_lock);
    return ready;
}

esp_err_t dot_audio_take_detected_wav(uint8_t **wav, size_t *length) {
    if (!wav || !length) return ESP_ERR_INVALID_ARG;
    *wav = NULL;
    *length = 0;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    if (!s_detected_ready || s_recording || atomic_load(&s_muted)) {
        xSemaphoreGive(s_capture_lock);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = capture_wav_locked(wav, length);
    xSemaphoreGive(s_capture_lock);
    return err;
}

void dot_audio_cancel_playback(void) {
    atomic_fetch_add(&s_cancel_generation, 1);
}

uint32_t dot_audio_cancel_token(void) {
    return atomic_load(&s_cancel_generation);
}

typedef struct {
    char *response;
    size_t used;
    size_t capacity;
    bool overflow;
    bool speech;
    bool cancellable;
    bool audio_type;
    bool carried;
    uint8_t carry;
    size_t audio_bytes;
    unsigned generation;
    esp_err_t error;
} audio_http_t;

static bool http_cancelled(const audio_http_t *state) {
    return (state->speech || state->cancellable) && cancelled(state->generation);
}

static esp_err_t pcm_feed(audio_http_t *state, const uint8_t *data, size_t length) {
    int64_t progress = esp_timer_get_time();
    size_t offset = 0;
    while (offset < length) {
        if (cancelled(state->generation)) return ESP_ERR_INVALID_STATE;
        if (atomic_load(&s_pcm_failed) || !atomic_load(&s_pcm_playing)) return ESP_FAIL;
        size_t sent = xStreamBufferSend(s_pcm, data + offset, length - offset, pdMS_TO_TICKS(20));
        offset += sent;
        if (sent) progress = esp_timer_get_time();
        else if (esp_timer_get_time() - progress > 2000000) return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t http_event(esp_http_client_event_t *event) {
    audio_http_t *state = event->user_data;
    // IDF 5.2's body parser ignores the callback return code, so preserve the
    // error and refuse all subsequent frames until perform() returns to us.
    if (state->error != ESP_OK) return state->error;
    if (http_cancelled(state)) return state->error = ESP_ERR_INVALID_STATE;
    if (event->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(event->header_key, "Date") == 0)
        dot_clock_observe_http_date(event->header_value);
    if (event->event_id == HTTP_EVENT_ON_HEADER && state->speech &&
        strcasecmp(event->header_key, "Content-Type") == 0) {
        state->audio_type = strncasecmp(event->header_value, "audio/pcm", 9) == 0 ||
            strncasecmp(event->header_value, "application/octet-stream", 24) == 0;
    }
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) return ESP_OK;
    int status = esp_http_client_get_status_code(event->client);
    if (state->speech && status >= 200 && status < 300) {
        if (!state->audio_type) return state->error = ESP_ERR_INVALID_RESPONSE;
        const uint8_t *data = (const uint8_t *)event->data;
        size_t length = (size_t)event->data_len;
        // HTTP/TLS chunk boundaries need not align with 16-bit PCM samples.
        if (state->carried) {
            uint8_t pair[2] = {state->carry, *data++};
            --length;
            state->carried = false;
            state->error = pcm_feed(state, pair, sizeof(pair));
            if (state->error != ESP_OK) return state->error;
            state->audio_bytes += sizeof(pair);
        }
        if (length & 1) {
            state->carry = data[length - 1];
            state->carried = true;
            --length;
        }
        if (length) {
            state->error = pcm_feed(state, data, length);
            if (state->error == ESP_OK) state->audio_bytes += length;
        }
        return state->error;
    }
    if (state->response && !state->overflow) {
        size_t length = (size_t)event->data_len;
        if (length >= state->capacity - state->used) {
            state->overflow = true;
            return state->error = ESP_ERR_INVALID_SIZE;
        }
        memcpy(state->response + state->used, event->data, length);
        state->used += length;
        state->response[state->used] = '\0';
    }
    return ESP_OK;
}

static esp_http_client_handle_t make_client(const dot_config_t *config, const char *url,
        const char *content_type, audio_http_t *state) {
    // IDF's DEBUG HTTP logger prints Authorization headers verbatim.
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);
    esp_http_client_config_t options = {
        .url = url, .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        // Async TLS still uses timeout_ms for its socket-read select. Bound it
        // so a cancelled request waiting for headers returns within one second.
        .timeout_ms = 1000, .buffer_size = 2048, .buffer_size_tx = 1024,
        .event_handler = http_event, .user_data = state,
        // Poll from this worker instead of blocking it while HTTP headers arrive.
        // Cancellation therefore never closes a client that another task is using.
        .is_async = true, .disable_auto_redirect = true,
        .max_authorization_retries = -1,
    };
    esp_http_client_handle_t client = esp_http_client_init(&options);
    if (!client) return NULL;
    char authorization[DOT_KEY_MAX + 8];
    snprintf(authorization, sizeof(authorization), "Bearer %s", config->audio_api_key);
    esp_err_t err = esp_http_client_set_header(client, "Authorization", authorization);
    memset(authorization, 0, sizeof(authorization));
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Content-Type", content_type);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return NULL;
    }
    return client;
}

static esp_err_t perform_request(esp_http_client_handle_t client, audio_http_t *state) {
    int64_t deadline = esp_timer_get_time() + HTTP_DEADLINE_US;
    char *body = NULL;
    int body_length = esp_http_client_get_post_field(client, &body);
    if (!body || body_length <= 0) return ESP_ERR_INVALID_ARG;
    // Manual streaming reads are intentional: IDF 5.2's perform() stops a
    // chunked response on a socket timeout, even when is_async is enabled.
    // fetch_headers/read instead report EAGAIN and retain parser state.
    for (;;) {
        if (http_cancelled(state)) return ESP_ERR_INVALID_STATE;
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        esp_err_t err = esp_http_client_open(client, body_length);
        if (state->error != ESP_OK) return state->error;
        if (err == ESP_OK) break;
        // Connecting is the only safe open retry: no HTTP bytes were sent.
        if (err != ESP_ERR_HTTP_CONNECTING) return err;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    size_t sent = 0;
    while (sent < (size_t)body_length) {
        if (http_cancelled(state)) return ESP_ERR_INVALID_STATE;
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        size_t chunk = (size_t)body_length - sent;
        if (chunk > 2048) chunk = 2048;
        errno = 0;
        int written = esp_http_client_write(client, body + sent, (int)chunk);
        if (written > 0) sent += written;
        else if (written == 0 || errno == EAGAIN || errno == EWOULDBLOCK) vTaskDelay(pdMS_TO_TICKS(10));
        else return ESP_FAIL;
    }
    for (;;) {
        if (http_cancelled(state)) return ESP_ERR_INVALID_STATE;
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        int64_t length = esp_http_client_fetch_headers(client);
        if (state->error != ESP_OK) return state->error;
        if (length >= 0) break;
        if (length != -ESP_ERR_HTTP_EAGAIN) return ESP_ERR_INVALID_RESPONSE;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (http_cancelled(state)) return ESP_ERR_INVALID_STATE;
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        // API errors can echo user input; deliberately log only the HTTP code.
        ESP_LOGW(TAG, "OpenAI audio HTTP status %d", status);
        return ESP_ERR_INVALID_RESPONSE;
    }
    char scratch[2048];
    while (!esp_http_client_is_complete_data_received(client)) {
        if (http_cancelled(state)) return ESP_ERR_INVALID_STATE;
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        // The event callback receives each decoded body segment exactly once;
        // bytes copied into scratch (including fetch_headers' cache) are ignored.
        int received = esp_http_client_read(client, scratch, sizeof(scratch));
        if (state->error != ESP_OK) return state->error;
        if (received == -ESP_ERR_HTTP_EAGAIN) continue;
        if (received < 0 || (received == 0 && !esp_http_client_is_complete_data_received(client)))
            return ESP_ERR_INVALID_RESPONSE;
    }
    return state->overflow ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static bool valid_model(const char *model) {
    if (!model || !*model) return false;
    for (const unsigned char *p = (const unsigned char *)model; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.')) return false;
    }
    return true;
}

esp_err_t dot_openai_transcribe_guarded(const dot_config_t *config, const uint8_t *wav, size_t length,
        char *text, size_t capacity, uint32_t expected_token) {
    if (text && capacity) text[0] = '\0';
    if (!config || !wav || !text || capacity < 2 || length < 44 ||
        length > 44 + RECORD_SAMPLES * sizeof(int16_t) || !config->audio_api_key[0] ||
        !valid_model(config->stt_model)) return ESP_ERR_INVALID_ARG;
    if (cancelled(expected_token)) return ESP_ERR_INVALID_STATE;
    if (!dot_clock_bootstrap_ready()) return ESP_ERR_INVALID_STATE;
    char boundary[48];
    snprintf(boundary, sizeof(boundary), "dot-%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    char prefix[512];
    int prefix_length = snprintf(prefix, sizeof(prefix),
        "--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n", boundary, config->stt_model, boundary, boundary);
    char suffix[64];
    int suffix_length = snprintf(suffix, sizeof(suffix), "\r\n--%s--\r\n", boundary);
    if (prefix_length < 0 || (size_t)prefix_length >= sizeof(prefix) || suffix_length < 0 || (size_t)suffix_length >= sizeof(suffix))
        return ESP_ERR_INVALID_SIZE;
    size_t body_length = (size_t)prefix_length + length + (size_t)suffix_length;
    char *body = heap_caps_malloc(body_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *response = heap_caps_calloc(1, HTTP_RESPONSE_LIMIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body || !response) { free(body); free(response); return ESP_ERR_NO_MEM; }
    memcpy(body, prefix, prefix_length);
    memcpy(body + prefix_length, wav, length);
    memcpy(body + prefix_length + length, suffix, suffix_length);
    audio_http_t state = {.response = response, .capacity = HTTP_RESPONSE_LIMIT,
        .cancellable = true, .generation = expected_token};
    char content_type[96];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_handle_t client = make_client(config, "https://api.openai.com/v1/audio/transcriptions", content_type, &state);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (client) {
        err = esp_http_client_set_post_field(client, body, (int)body_length);
        if (err == ESP_OK) err = perform_request(client, &state);
        esp_http_client_cleanup(client);
    }
    if (cancelled(expected_token)) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        cJSON *json = cJSON_Parse(response);
        cJSON *value = cJSON_GetObjectItemCaseSensitive(json, "text");
        if (!cJSON_IsString(value) || !value->valuestring[0]) err = ESP_ERR_INVALID_RESPONSE;
        else if (strlen(value->valuestring) >= capacity) err = ESP_ERR_INVALID_SIZE;
        else memcpy(text, value->valuestring, strlen(value->valuestring) + 1);
        cJSON_Delete(json);
    }
    free(body);
    free(response);
    return err;
}

esp_err_t dot_openai_transcribe(const dot_config_t *config, const uint8_t *wav, size_t length,
        char *text, size_t capacity) {
    return dot_openai_transcribe_guarded(config, wav, length, text, capacity, dot_audio_cancel_token());
}

esp_err_t dot_openai_speak_guarded(const dot_config_t *config, const char *text, uint32_t expected_token) {
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!config || !text || !*text || strlen(text) > DOT_TEXT_MAX ||
        !config->audio_api_key[0] || !valid_model(config->tts_model) || !config->voice[0]) return ESP_ERR_INVALID_ARG;
    if (cancelled(expected_token)) return ESP_ERR_INVALID_STATE;
    if (!dot_clock_bootstrap_ready()) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_speak_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    unsigned generation = expected_token;
    if (cancelled(generation)) { xSemaphoreGive(s_speak_lock); return ESP_ERR_INVALID_STATE; }
    cJSON *json = cJSON_CreateObject();
    char *body = NULL;
    esp_err_t err = ESP_ERR_NO_MEM;
    if (!json) goto done;
    if (!cJSON_AddStringToObject(json, "model", config->tts_model) ||
        !cJSON_AddStringToObject(json, "voice", config->voice) ||
        !cJSON_AddStringToObject(json, "input", text) ||
        !cJSON_AddStringToObject(json, "response_format", "pcm")) goto done;
    body = cJSON_PrintUnformatted(json);
    if (!body) goto done;
    audio_http_t state = {.speech = true, .cancellable = true, .generation = generation};
    esp_http_client_handle_t client = make_client(config, "https://api.openai.com/v1/audio/speech", "application/json", &state);
    if (!client) goto done;
    err = esp_http_client_set_post_field(client, body, (int)strlen(body));
    if (err != ESP_OK) { esp_http_client_cleanup(client); goto done; }
    if (cancelled(generation)) { esp_http_client_cleanup(client); err = ESP_ERR_INVALID_STATE; goto done; }
    xStreamBufferReset(s_pcm);
    xSemaphoreTake(s_pcm_done, 0);
    s_pcm_generation = generation;
    atomic_store(&s_pcm_failed, false);
    atomic_store(&s_pcm_input_finished, false);
    atomic_store(&s_pcm_playing, true);
    xTaskNotifyGive(s_pcm_task);
    err = perform_request(client, &state);
    esp_http_client_cleanup(client);
    if (err == ESP_OK && state.carried) err = ESP_ERR_INVALID_SIZE;
    if (err != ESP_OK) atomic_store(&s_pcm_failed, true);
    atomic_store(&s_pcm_input_finished, true);
    // At most 64 KiB (1.37 seconds) remain. Cancellation is checked by the PCM task.
    if (xSemaphoreTake(s_pcm_done, pdMS_TO_TICKS(4000)) != pdTRUE) {
        dot_audio_cancel_playback();
        // Do not reuse/reset the stream until its sole consumer relinquishes it.
        xSemaphoreTake(s_pcm_done, portMAX_DELAY);
        err = ESP_ERR_TIMEOUT;
    }
    if (cancelled(generation)) err = ESP_ERR_INVALID_STATE;
    else if (err == ESP_OK && atomic_load(&s_pcm_failed)) err = ESP_FAIL;
    // A successful HTTP response with no PCM frames is not successful playback.
    if (err == ESP_OK && (!state.audio_type || state.audio_bytes == 0)) err = ESP_ERR_INVALID_RESPONSE;
done:
    free(body);
    cJSON_Delete(json);
    xSemaphoreGive(s_speak_lock);
    return err;
}

esp_err_t dot_openai_speak(const dot_config_t *config, const char *text) {
    return dot_openai_speak_guarded(config, text, dot_audio_cancel_token());
}
