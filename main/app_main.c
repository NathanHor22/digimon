#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef DOT_CONTROLLER_HOST_TEST
#include "controller_host.h"
#else
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#endif
#include "dot_audio.h"
#include "dot_clock.h"
#include "dot_config.h"
#include "dot_controller.h"
#include "dot_mcp.h"
#include "dot_tunnel.h"
#include "dot_wifi.h"
#include "dot_intent.h"
#include "dot_preferences.h"
#ifndef DOT_CONTROLLER_HOST_TEST
#include <stdatomic.h>
#include "dot_buttons.h"
#include "dot_ui.h"
#include "lantern_board.h"
#include "lantern_display.h"
#endif

#define VERSION "0.2.0"
#define WAIT_US INT64_C(120000000)
static const char *TAG = "dot";
static dot_config_t s_config;
static SemaphoreHandle_t s_lock;
/* Capture operations always take this before s_lock. Audio capture functions
 * never call back into the controller, so this ordering has no inverse edge. */
static SemaphoreHandle_t s_capture_op_lock;
static QueueHandle_t s_jobs;
static bool s_pending, s_capturing, s_queued, s_audio_ready;
static uint32_t s_generation;
static int64_t s_started;
static int64_t s_followup_until;
static char s_context[DOT_INTENT_CONTEXT_MAX + 1];
static char s_request[DOT_REQUEST_ID_MAX + 1];
static char s_state[32] = "booting";
static char s_error[64];
static unsigned char s_reply_digest[32];
/* Physical mute is sticky for this boot even if its flash write fails. Only a
 * successful, explicit physical unmute may clear it after initialization. */
static bool s_physical_mute_latch, s_mute_preferences_initialized;
typedef struct { char id[DOT_REQUEST_ID_MAX + 1]; unsigned char digest[32]; } done_t;
static done_t s_done[4];
static unsigned s_done_next;
typedef enum { JOB_TEXT, JOB_TRANSCRIBE, JOB_SPEAK } job_kind_t;
typedef struct {
    job_kind_t kind;
    uint32_t generation;
    char request_id[DOT_REQUEST_ID_MAX + 1];
    char text[DOT_TEXT_MAX + 1];
    char input_kind[16];
    uint8_t *wav;
    size_t wav_size;
    bool automatic;
} job_t;

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }
static void set_state_locked(const char *state, esp_err_t err)
{
    strlcpy(s_state, state, sizeof(s_state));
    strlcpy(s_error, err == ESP_OK ? "" : esp_err_to_name(err), sizeof(s_error));
}
static bool current(uint32_t generation)
{
    lock(); bool result = s_pending && s_generation == generation; unlock();
    return result;
}
static bool set_job_state(const job_t *job, const char *state, esp_err_t err)
{
    lock();
    bool active = s_pending && s_generation == job->generation;
    if (active) set_state_locked(state, err);
    unlock();
    return active;
}
static void uuid(char out[DOT_REQUEST_ID_MAX + 1])
{
    unsigned char b[16]; esp_fill_random(b, sizeof(b));
    b[6] = (b[6] & 15) | 64; b[8] = (b[8] & 63) | 128;
    snprintf(out, DOT_REQUEST_ID_MAX + 1,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
}
static void digest(const char *text, unsigned char out[32])
{
    (void)mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
        (const unsigned char *)text, strlen(text), out);
}
static bool transport_ready(void)
{
    return dot_wifi_connected() && dot_clock_fresh() && dot_tunnel_connected() &&
        dot_mcp_subscription_ready();
}
static void save_context_locked(const char *text)
{
    size_t length = strlen(text);
    if (length > DOT_INTENT_CONTEXT_MAX) length = DOT_INTENT_CONTEXT_MAX;
    /* Do not leave a partial UTF-8 character at the context boundary. */
    while (length && ((unsigned char)text[length] & 0xc0) == 0x80) --length;
    memcpy(s_context, text, length); s_context[length] = 0;
}
cJSON *dot_controller_status(void)
{
    cJSON *result = cJSON_CreateObject();
    if (!result) return NULL;
    lock();
    cJSON_AddStringToObject(result, "state", s_state);
    cJSON_AddStringToObject(result, "last_request_id", s_request);
    cJSON_AddStringToObject(result, "last_error", s_error);
    cJSON_AddBoolToObject(result, "pending", s_pending);
    cJSON_AddBoolToObject(result, "reply_queued", s_queued);
    unlock();
    cJSON_AddStringToObject(result, "device_id", s_config.device_id);
    cJSON_AddStringToObject(result, "board_id", "lantern-original");
    cJSON_AddStringToObject(result, "firmware_version", VERSION);
    cJSON_AddBoolToObject(result, "wifi_connected", dot_wifi_connected());
    cJSON_AddBoolToObject(result, "clock_fresh", dot_clock_fresh());
    cJSON_AddBoolToObject(result, "tunnel_connected", dot_tunnel_connected());
    cJSON_AddNumberToObject(result, "tunnel_http_status", dot_tunnel_last_status());
    cJSON_AddBoolToObject(result, "subscription_ready", dot_mcp_subscription_ready());
    cJSON_AddBoolToObject(result, "audio_ready", s_audio_ready && dot_config_audio_ready(&s_config));
    dot_preferences_t preferences;
    if (dot_preferences_get(&preferences) == ESP_OK) {
        cJSON_AddBoolToObject(result, "microphone_muted", dot_audio_muted());
        cJSON_AddBoolToObject(result, "hands_free", preferences.hands_free);
        cJSON_AddNumberToObject(result, "volume", preferences.volume_percent);
        cJSON_AddStringToObject(result, "name", preferences.device_name);
    }
    cJSON_AddNumberToObject(result, "free_heap", esp_get_free_heap_size());
    return result;
}
bool dot_controller_is_pending(const char *request_id)
{
    lock(); bool pending = s_pending && request_id && !strcmp(request_id, s_request); unlock();
    return pending;
}
static esp_err_t begin_request(job_t *job, bool from_recording, uint32_t capture_generation)
{
    if (!transport_ready()) return ESP_ERR_INVALID_STATE;
    lock();
    if (s_pending || (from_recording ? (!s_capturing || s_generation != capture_generation) : s_capturing)) {
        unlock(); return ESP_ERR_INVALID_STATE;
    }
    s_pending = true; s_capturing = false; s_queued = false;
    ++s_generation; s_started = esp_timer_get_time();
    uuid(s_request); strlcpy(job->request_id, s_request, sizeof(job->request_id));
    job->generation = s_generation;
    set_state_locked(job->kind == JOB_TRANSCRIBE ? "transcribing" : "sending", ESP_OK);
    if (xQueueSend(s_jobs, job, 0) != pdTRUE) {
        s_pending = false; set_state_locked("error", ESP_ERR_NO_MEM); unlock();
        return ESP_ERR_NO_MEM;
    }
    unlock(); return ESP_OK;
}
esp_err_t dot_controller_submit_text(const char *text, const char *input_kind)
{
    if (!text || !*text || strlen(text) > DOT_TEXT_MAX || !input_kind ||
        (strcmp(input_kind, "serial_test") && strcmp(input_kind, "speech")))
        return ESP_ERR_INVALID_ARG;
    job_t *job = calloc(1, sizeof(*job));
    if (!job) return ESP_ERR_NO_MEM;
    job->kind = JOB_TEXT;
    strlcpy(job->text, text, sizeof(job->text));
    strlcpy(job->input_kind, input_kind, sizeof(job->input_kind));
    esp_err_t err = begin_request(job, false, 0); free(job); return err;
}
esp_err_t dot_controller_accept_reply(const char *request_id, const char *text, bool *duplicate)
{
    if (duplicate) *duplicate = false;
    if (!request_id || !*request_id || strlen(request_id) > DOT_REQUEST_ID_MAX ||
        !text || !*text || strlen(text) > DOT_TEXT_MAX) return ESP_ERR_INVALID_ARG;
    unsigned char hash[32]; digest(text, hash);
    lock();
    for (unsigned i = 0; i < 4; ++i) {
        if (!strcmp(s_done[i].id, request_id)) {
            bool same = memcmp(s_done[i].digest, hash, sizeof(hash)) == 0;
            if (duplicate) *duplicate = same;
            unlock(); return same ? ESP_OK : ESP_ERR_INVALID_STATE;
        }
    }
    if (!s_pending || strcmp(s_request, request_id)) { unlock(); return ESP_ERR_INVALID_STATE; }
    if (s_queued) {
        bool same = memcmp(s_reply_digest, hash, sizeof(hash)) == 0;
        if (duplicate) *duplicate = same;
        unlock(); return same ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    job_t *job = calloc(1, sizeof(*job));
    if (!job) { unlock(); return ESP_ERR_NO_MEM; }
    job->kind = JOB_SPEAK; job->generation = s_generation;
    strlcpy(job->request_id, request_id, sizeof(job->request_id));
    strlcpy(job->text, text, sizeof(job->text));
    if (xQueueSend(s_jobs, job, 0) != pdTRUE) {
        free(job); unlock(); return ESP_ERR_NO_MEM;
    }
    free(job); s_queued = true;
    memcpy(s_reply_digest, hash, sizeof(hash)); set_state_locked("reply_queued", ESP_OK);
    unlock(); return ESP_OK;
}
static void cancel_locked(void)
{
    ++s_generation; s_pending = false; s_capturing = false; s_queued = false;
    /* Token and controller generation change in the same critical section. */
    dot_audio_cancel_playback();
    s_followup_until = 0;
    memset(s_context, 0, sizeof(s_context));
    set_state_locked("cancelled", ESP_OK);
}
void dot_controller_cancel(void)
{
    xSemaphoreTake(s_capture_op_lock, portMAX_DELAY);
    lock();
    cancel_locked();
    unlock();
    /* New capture cannot start until this older cancellation is finished. */
    dot_audio_record_cancel();
    xSemaphoreGive(s_capture_op_lock);
}
static esp_err_t physical_preference(const char *key, int value, bool boolean)
{
    cJSON *patch = cJSON_CreateObject();
    esp_err_t err = ESP_ERR_NO_MEM;
    if (patch) {
        bool added = boolean ? cJSON_AddBoolToObject(patch, key, value != 0) != NULL :
                               cJSON_AddNumberToObject(patch, key, value) != NULL;
        if (added) err = dot_preferences_update(patch, true);
        cJSON_Delete(patch);
    }
    if (err != ESP_OK) { lock(); set_state_locked("settings_error", err); unlock(); }
    return err;
}
static esp_err_t apply_physical_mute(bool muted)
{
    if (muted) {
        s_physical_mute_latch = true;
        dot_audio_set_muted(true);
        dot_controller_cancel();
    }
    esp_err_t err = physical_preference("microphone_muted", muted, true);
    if (!muted && err == ESP_OK) {
        s_physical_mute_latch = false;
        dot_audio_set_muted(false);
    } else if (!muted) {
        s_physical_mute_latch = true;
        dot_audio_set_muted(true);
        dot_controller_cancel();
        lock(); set_state_locked("settings_error", err); unlock();
    }
    return err;
}
static void apply_preference_mute(bool stored_muted)
{
    if (!s_mute_preferences_initialized) {
        s_mute_preferences_initialized = true;
        s_physical_mute_latch = s_physical_mute_latch || stored_muted;
        dot_audio_set_muted(s_physical_mute_latch);
        if (s_physical_mute_latch) dot_controller_cancel();
    } else if (stored_muted) {
        s_physical_mute_latch = true;
        dot_audio_set_muted(true);
        dot_controller_cancel();
    }
    /* An unrelated remote preference revision can never apply unmute. */
}
static void finish_job_locked(const job_t *job, esp_err_t err, bool completed)
{
    if (s_pending && s_generation == job->generation) {
        if (completed) {
            done_t *done = &s_done[s_done_next++ % 4];
            strlcpy(done->id, job->request_id, sizeof(done->id)); digest(job->text, done->digest);
            s_followup_until = esp_timer_get_time() + INT64_C(45000000);
        }
        s_pending = false; s_queued = false;
        set_state_locked(err == ESP_OK ? (completed ? "spoken" : "text_received") : "error", err);
    }
}
static void finish_job(const job_t *job, esp_err_t err, bool completed)
{
    lock(); finish_job_locked(job, err, completed); unlock();
}
static void process_job(job_t *job)
{
    /* Snapshot first: a cancel between current() and the audio call must
     * invalidate this token rather than letting the call take a fresh one. */
    uint32_t audio_token = dot_audio_cancel_token();
    if (!current(job->generation)) { free(job->wav); job->wav = NULL; return; }
    esp_err_t err = ESP_OK;
    if (job->kind == JOB_SPEAK) {
        if (s_audio_ready && dot_config_audio_ready(&s_config)) {
            if (!set_job_state(job, "speaking", ESP_OK)) return;
            err = dot_openai_speak_guarded(&s_config, job->text, audio_token);
            finish_job(job, err, err == ESP_OK);
        } else {
            /* Text-only first connection test: output is intentionally local,
             * only this owner's dot can provide the correlated reply. */
            cJSON *response = cJSON_CreateObject();
            cJSON_AddStringToObject(response, "dot", "reply");
            cJSON_AddStringToObject(response, "request_id", job->request_id);
            cJSON_AddStringToObject(response, "text", job->text);
            char *line = cJSON_PrintUnformatted(response);
            cJSON_Delete(response);
            lock();
            if (s_pending && s_generation == job->generation) {
                if (line) {
                    printf("%s\n", line);
                    finish_job_locked(job, ESP_OK, true);
                    set_state_locked("text_received", ESP_OK);
                } else finish_job_locked(job, ESP_ERR_NO_MEM, false);
            }
            unlock();
            free(line);
        }
        return;
    }
    if (job->kind == JOB_TRANSCRIBE) {
        err = dot_openai_transcribe_guarded(&s_config, job->wav, job->wav_size,
            job->text, sizeof(job->text), audio_token);
        free(job->wav); job->wav = NULL;
    }
    if (!current(job->generation)) return;
    if (err != ESP_OK) { finish_job(job, err, false); return; }
    if (job->automatic) {
        if (!set_job_state(job, "checking_address", ESP_OK)) return;
        char context[DOT_INTENT_CONTEXT_MAX + 1];
        lock();
        bool followup = esp_timer_get_time() < s_followup_until;
        strlcpy(context, s_context, sizeof(context));
        unlock();
        dot_intent_decision_t decision;
        err = dot_intent_classify(&s_config, job->text, context, followup, audio_token, &decision);
        memset(context, 0, sizeof(context));
        if (!current(job->generation)) return;
        if (err != ESP_OK) { finish_job(job, err, false); return; }
        if (!dot_intent_should_accept(&decision, followup)) {
            lock();
            if (s_pending && s_generation == job->generation) {
                s_pending = false; s_queued = false;
                set_state_locked("ambient_ignored", ESP_OK);
            }
            unlock();
            return;
        }
    }
    if (!set_job_state(job, "waiting_for_dot", ESP_OK)) return;
    lock();
    if (s_pending && s_generation == job->generation)
        save_context_locked(job->text);
    unlock();
    err = dot_mcp_publish_utterance(job->request_id, job->text, job->input_kind);
    if (err != ESP_OK && current(job->generation)) {
        /* Delivery may have been accepted before a network timeout. Retain
         * this request; an explicit cancel prevents accidental duplicate work. */
        (void)set_job_state(job, "delivery_uncertain", err);
    }
}

#ifndef DOT_CONTROLLER_HOST_TEST
static void worker_task(void *arg)
{
    (void)arg;
    job_t *job = calloc(1, sizeof(*job));
    if (!job) { vTaskDelete(NULL); return; }
    for (;;) {
        if (xQueueReceive(s_jobs, job, portMAX_DELAY) == pdTRUE) process_job(job);
    }
}

static void serial_result(const char *op, esp_err_t err)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return;
    cJSON_AddStringToObject(json, "dot", op);
    cJSON_AddBoolToObject(json, "ok", err == ESP_OK);
    if (err != ESP_OK) cJSON_AddStringToObject(json, "error", esp_err_to_name(err));
    char *line = cJSON_PrintUnformatted(json);
    if (line) { printf("%s\n", line); free(line); }
    cJSON_Delete(json);
}
static bool set_string(const cJSON *object, const char *key, char *dest, size_t cap, bool header)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!value) return true;
    if (!cJSON_IsString(value) || !value->valuestring || strlen(value->valuestring) >= cap) return false;
    for (const unsigned char *p = (const unsigned char *)value->valuestring; *p; ++p) {
        if (*p < 32 || *p == 127 || (header && (*p <= 32 || *p > 126))) return false;
    }
    strlcpy(dest, value->valuestring, cap); return true;
}
static bool unique_fields(const cJSON *obj)
{
    if (!cJSON_IsObject(obj)) return false;
    const cJSON *a, *b;
    cJSON_ArrayForEach(a, obj) {
        if (!a->string) return false;
        for (b = a->next; b; b = b->next) if (!strcmp(a->string, b->string)) return false;
    }
    return true;
}
static void serial_command(char *line)
{
    const char *end = NULL;
    cJSON *request = cJSON_ParseWithLengthOpts(line, strlen(line) + 1, &end, true);
    if (!unique_fields(request)) { cJSON_Delete(request); serial_result("invalid", ESP_ERR_INVALID_ARG); return; }
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(request, "op");
    if (!cJSON_IsString(op)) { cJSON_Delete(request); serial_result("invalid", ESP_ERR_INVALID_ARG); return; }
    if (!strcmp(op->valuestring, "status")) {
        cJSON *status = dot_controller_status();
        cJSON_AddStringToObject(status, "dot", "status");
        char *text = cJSON_PrintUnformatted(status);
        if (text) { printf("%s\n", text); free(text); } cJSON_Delete(status);
    } else if (!strcmp(op->valuestring, "configure")) {
        const cJSON *cfg = cJSON_GetObjectItemCaseSensitive(request, "config");
        dot_config_t *candidate = malloc(sizeof(*candidate));
        if (!candidate) { serial_result("configure", ESP_ERR_NO_MEM); goto cleanup; }
        *candidate = s_config;
        bool valid = unique_fields(cfg);
#define FIELD(name, header) valid = valid && set_string(cfg, #name, candidate->name, sizeof(candidate->name), header)
        FIELD(device_id, true); FIELD(wifi_ssid, false); FIELD(wifi_password, false);
        FIELD(audio_api_key, true); FIELD(tunnel_api_key, true); FIELD(tunnel_id, true);
        FIELD(mcp_bearer, true); FIELD(stt_model, true); FIELD(tts_model, true); FIELD(voice, true);
#undef FIELD
        const cJSON *scope_auth = cJSON_GetObjectItemCaseSensitive(cfg, "tunnel_scope_auth");
        if (scope_auth) {
            valid = valid && cJSON_IsBool(scope_auth);
            candidate->tunnel_scope_auth = cJSON_IsTrue(scope_auth);
        }
        const cJSON *epoch = cJSON_GetObjectItemCaseSensitive(cfg, "epoch");
        valid = valid && cJSON_IsNumber(epoch) && epoch->valuedouble >= 1704067200.0 &&
            epoch->valuedouble < 4102444800.0;
        if (valid) candidate->provisioned_epoch = (int64_t)epoch->valuedouble;
        if (valid && candidate->mcp_bearer[0] && strlen(candidate->mcp_bearer) < 32) valid = false;
        esp_err_t err = valid ? dot_config_save(candidate) : ESP_ERR_INVALID_ARG;
        memset(candidate, 0, sizeof(*candidate)); free(candidate);
        serial_result("configure", err);
        if (err == ESP_OK) { cJSON_Delete(request); memset(line, 0, strlen(line)); vTaskDelay(pdMS_TO_TICKS(400)); esp_restart(); }
    } else if (!strcmp(op->valuestring, "say")) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(request, "text");
        esp_err_t err = cJSON_IsString(text) ? dot_controller_submit_text(text->valuestring, "serial_test") : ESP_ERR_INVALID_ARG;
        serial_result("say", err);
    } else if (!strcmp(op->valuestring, "cancel")) {
        dot_controller_cancel(); serial_result("cancel", ESP_OK);
    } else if (!strcmp(op->valuestring, "preferences")) {
        const cJSON *patch = cJSON_GetObjectItemCaseSensitive(request, "preferences");
        /* USB can tune preferences, but microphone mute remains a physical
         * control. In particular, no remote/serial request may unmute it. */
        serial_result("preferences", dot_preferences_update(patch, false));
    } else if (!strcmp(op->valuestring, "reboot")) {
        serial_result("reboot", ESP_OK); vTaskDelay(pdMS_TO_TICKS(200)); esp_restart();
    } else serial_result("invalid", ESP_ERR_NOT_SUPPORTED);
cleanup:
    cJSON_Delete(request);
}
static void serial_task(void *arg)
{
    (void)arg;
    char *line = calloc(1, 4096);
    if (!line) { vTaskDelete(NULL); return; }
    size_t count = 0; bool overflow = false;
    for (;;) {
        unsigned char ch;
        int read = uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(100));
        if (read != 1) continue;
        if (ch == '\n') {
            if (overflow) serial_result("invalid", ESP_ERR_INVALID_SIZE);
            else if (count) { line[count] = 0; serial_command(line); }
            memset(line, 0, 4096); count = 0; overflow = false;
        } else if (ch != '\r') {
            if (ch == 0 || count >= 4095) overflow = true;
            else if (!overflow) line[count++] = (char)ch;
        }
    }
}
#endif
static void record_begin(void)
{
    xSemaphoreTake(s_capture_op_lock, portMAX_DELAY);
    bool ready = transport_ready() && s_audio_ready && dot_config_audio_ready(&s_config);
    lock();
    if (s_pending || s_capturing) {
        cancel_locked(); unlock();
        dot_audio_record_cancel();
        xSemaphoreGive(s_capture_op_lock);
        return;
    }
    if (!ready) {
        set_state_locked("not_ready", ESP_ERR_INVALID_STATE); unlock();
        xSemaphoreGive(s_capture_op_lock);
        return;
    }
    uint32_t generation = ++s_generation;
    s_capturing = true;
    set_state_locked("starting_capture", ESP_OK);
    unlock();
    esp_err_t err = dot_audio_record_begin();
    lock();
    bool owns_capture = s_capturing && !s_pending && s_generation == generation;
    if (owns_capture) {
        s_capturing = err == ESP_OK;
        set_state_locked(err == ESP_OK ? "recording" : "error", err);
    }
    unlock();
    if (!owns_capture && err == ESP_OK) dot_audio_record_cancel();
    xSemaphoreGive(s_capture_op_lock);
}
static void record_end(void)
{
    xSemaphoreTake(s_capture_op_lock, portMAX_DELAY);
    lock();
    bool capturing = s_capturing && !s_pending;
    uint32_t generation = s_generation;
    unlock();
    if (!capturing) { xSemaphoreGive(s_capture_op_lock); return; }
    job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        dot_audio_record_cancel();
        lock();
        if (s_capturing && !s_pending && s_generation == generation) {
            s_capturing = false;
            set_state_locked("error", ESP_ERR_NO_MEM);
        }
        unlock();
        xSemaphoreGive(s_capture_op_lock);
        return;
    }
    job->kind = JOB_TRANSCRIBE; strcpy(job->input_kind, "speech");
    esp_err_t err = dot_audio_record_end(&job->wav, &job->wav_size);
    if (err == ESP_OK) err = begin_request(job, true, generation);
    if (err != ESP_OK) {
        free(job->wav);
        dot_audio_record_cancel();
        lock();
        if (s_capturing && !s_pending && s_generation == generation) {
            s_capturing = false;
            set_state_locked("error", err);
        }
        unlock();
    }
    free(job);
    xSemaphoreGive(s_capture_op_lock);
}
#ifndef DOT_CONTROLLER_HOST_TEST
static atomic_int s_scan_state;
static dot_wifi_network_t s_scan_results[DOT_UI_NETWORKS_MAX];
static size_t s_scan_count;

static void scan_task(void *arg)
{
    (void)arg;
    s_scan_count = DOT_UI_NETWORKS_MAX;
    esp_err_t err = dot_wifi_scan(s_scan_results, &s_scan_count);
    atomic_store(&s_scan_state, err == ESP_OK ? 2 : 3);
    vTaskDelete(NULL);
}
static void take_automatic(void)
{
    if (!dot_audio_detected_ready()) return;
    xSemaphoreTake(s_capture_op_lock, portMAX_DELAY);
    lock(); bool idle = !s_pending && !s_capturing; unlock();
    if (idle) {
        job_t *job = calloc(1, sizeof(*job));
        if (job) {
            job->kind = JOB_TRANSCRIBE; job->automatic = true;
            strcpy(job->input_kind, "speech");
            esp_err_t err = dot_audio_take_detected_wav(&job->wav, &job->wav_size);
            if (err == ESP_OK) err = begin_request(job, false, 0);
            if (err != ESP_OK) free(job->wav);
            free(job);
        }
    }
    xSemaphoreGive(s_capture_op_lock);
}
static void ui_work(void)
{
    if (dot_ui_scan_requested() && atomic_load(&s_scan_state) == 0) {
        atomic_store(&s_scan_state, 1);
        if (xTaskCreate(scan_task, "digimon_scan", 6144, NULL, 3, NULL) != pdPASS)
            atomic_store(&s_scan_state, 3);
    }
    int scan = atomic_load(&s_scan_state);
    if (scan >= 2) {
        dot_ui_network_t results[DOT_UI_NETWORKS_MAX];
        size_t count = scan == 2 ? s_scan_count : 0;
        for (size_t i = 0; i < count; ++i) {
            memcpy(results[i].ssid, s_scan_results[i].ssid, sizeof(results[i].ssid));
            results[i].rssi = s_scan_results[i].rssi;
            results[i].secured = s_scan_results[i].secured;
        }
        dot_ui_set_networks(results, count, scan != 2);
        atomic_store(&s_scan_state, 0);
    }
    uint8_t volume; bool flag;
    if (dot_ui_take_volume(&volume)) physical_preference("volume_percent", volume, false);
    if (dot_ui_take_mute(&flag)) {
        (void)apply_physical_mute(flag);
    }
    if (dot_ui_take_handsfree(&flag)) physical_preference("hands_free", flag, true);
    char ssid[33], password[65];
    if (dot_ui_take_wifi(ssid, password)) {
        dot_config_t *candidate = malloc(sizeof(*candidate));
        esp_err_t err = ESP_ERR_NO_MEM;
        if (candidate) {
            *candidate = s_config;
            strlcpy(candidate->wifi_ssid, ssid, sizeof(candidate->wifi_ssid));
            strlcpy(candidate->wifi_password, password, sizeof(candidate->wifi_password));
            err = dot_config_save(candidate);
            memset(candidate, 0, sizeof(*candidate)); free(candidate);
        }
        memset(password, 0, sizeof(password));
        memset(ssid, 0, sizeof(ssid));
        if (err == ESP_OK) {
            dot_controller_cancel(); vTaskDelay(pdMS_TO_TICKS(100)); esp_restart();
        } else {
            dot_ui_wifi_result(false);
            lock(); set_state_locked("wifi_settings_error", err); unlock();
        }
    }
}
static void button_actions(unsigned actions, dot_preferences_t *preferences)
{
    if (actions & DOT_HOLD_SIDES) {
        bool mute = !preferences->microphone_muted;
        (void)apply_physical_mute(mute);
        return;
    }
    if (actions & DOT_HOLD_CENTRE) {
        dot_controller_cancel();
        if (dot_ui_is_open()) dot_ui_event(DOT_UI_BACK);
        else dot_ui_settings_open();
        return;
    }
    if (dot_ui_is_open()) {
        if (actions & DOT_PRESS_UP) dot_ui_event(DOT_UI_UP);
        if (actions & DOT_PRESS_DOWN) dot_ui_event(DOT_UI_DOWN);
        if (actions & DOT_PRESS_CENTRE) dot_ui_event(DOT_UI_SELECT);
        return;
    }
    if (actions & (DOT_PRESS_UP | DOT_PRESS_DOWN)) {
        int volume = preferences->volume_percent + ((actions & DOT_PRESS_UP) ? 5 : -5);
        if (volume < 0) volume = 0;
        if (volume > 100) volume = 100;
        physical_preference("volume_percent", volume, false);
    }
    if (actions & DOT_PRESS_CENTRE) {
        lock(); bool capturing = s_capturing, pending = s_pending; unlock();
        if (capturing) record_end();
        else if (pending) dot_controller_cancel();
        else if (!preferences->microphone_muted) record_begin();
    }
}
void app_main(void)
{
    gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT); gpio_set_level(GPIO_NUM_2, 1);
    gpio_config_t buttons = { .pin_bit_mask = (1ULL << 0) | (1ULL << 10) | (1ULL << 39) | (1ULL << 9),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    ESP_ERROR_CHECK(gpio_config(&buttons));
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);
    s_lock = xSemaphoreCreateMutex(); s_capture_op_lock = xSemaphoreCreateMutex();
    s_jobs = xQueueCreate(2, sizeof(job_t));
    ESP_ERROR_CHECK(s_lock && s_capture_op_lock && s_jobs ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(dot_config_init(&s_config));
    esp_err_t err = dot_preferences_init();
    if (err != ESP_OK) ESP_LOGE(TAG, "Preferences use safe defaults: %s", esp_err_to_name(err));
    uart_config_t uart = { .baud_rate = 115200, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE };
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uart));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 4096, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(lantern_display_init()); dot_ui_init();
    s_audio_ready = dot_audio_init() == ESP_OK;
    (void)dot_clock_init(s_config.provisioned_epoch);
    ESP_ERROR_CHECK(dot_mcp_init(&s_config));
    ESP_ERROR_CHECK(xTaskCreate(serial_task, "dot_serial", 8192, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(worker_task, "dot_worker", 16384, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    err = dot_wifi_start(&s_config); /* Empty SSID still permits scans. */
    if (err != ESP_OK) ESP_LOGE(TAG, "Wi-Fi init: %s", esp_err_to_name(err));
    ESP_LOGI(TAG, "Digimon %s: face, three-button setup, experimental hands-free", VERSION);
    dot_buttons_t buttons_state = {0};
    bool tunnel_started = false;
    uint32_t applied_revision = 0, display_at = 0, checkpoint_at = 0;
    dot_preferences_t preferences = { .volume_percent = 60, .microphone_muted = true };
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (dot_preferences_get(&preferences) == ESP_OK && preferences.revision != applied_revision) {
            dot_audio_set_volume(preferences.volume_percent);
            apply_preference_mute(preferences.microphone_muted);
            applied_revision = preferences.revision;
        }
        preferences.microphone_muted = dot_audio_muted();
        /* Refresh the menu from actual mute state even when a failed physical
         * save did not change the persisted preference revision. */
        dot_ui_set_preferences(preferences.volume_percent, preferences.microphone_muted, preferences.hands_free);
        uint8_t raw = (gpio_get_level(GPIO_NUM_0) == 0 ? DOT_BUTTON_CENTRE : 0) |
            (gpio_get_level(GPIO_NUM_10) == 0 ? DOT_BUTTON_UP : 0) |
            (gpio_get_level(GPIO_NUM_39) == 0 ? DOT_BUTTON_DOWN : 0);
        button_actions(dot_buttons_poll(&buttons_state, raw, now), &preferences);
        ui_work();
        preferences.microphone_muted = dot_audio_muted();
        if (!tunnel_started && dot_wifi_connected() && dot_clock_bootstrap_ready() && dot_config_tunnel_ready(&s_config)) {
            err = dot_tunnel_start(&s_config); tunnel_started = err == ESP_OK;
            if (err != ESP_OK) vTaskDelay(pdMS_TO_TICKS(1000));
        }
        lock(); bool idle = !s_pending && !s_capturing; unlock();
        bool enabled = idle && preferences.hands_free && !preferences.microphone_muted &&
            !dot_ui_is_open() && s_audio_ready && dot_config_audio_ready(&s_config) && transport_ready();
        dot_audio_set_handsfree(enabled);
        if (enabled) take_automatic();
        if (dot_audio_record_full()) record_end();
        lock();
        if (s_pending && !s_queued && esp_timer_get_time() - s_started > WAIT_US)
            set_state_locked("still_waiting", ESP_ERR_TIMEOUT);
        bool busy = s_pending;
        unlock();
        if ((uint32_t)(now - display_at) >= 50) {
            display_at = now;
            dot_ui_snapshot_t snapshot = { .listening = enabled || dot_audio_is_recording(),
                .speaking = dot_audio_speaking(), .thinking = busy && !dot_audio_speaking(),
                .muted = dot_audio_muted(), .wifi_connected = dot_wifi_connected(),
                .configured = dot_config_audio_ready(&s_config) && dot_config_tunnel_ready(&s_config),
                .speech_level = (uint8_t)dot_audio_speech_level(), .now_ms = now };
            dot_ui_render(&snapshot);
        }
        if ((uint32_t)(now - checkpoint_at) >= 1000) { checkpoint_at = now; dot_clock_checkpoint(); }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
#endif
