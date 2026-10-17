#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "dot_config.h"

esp_err_t dot_audio_init(void);
esp_err_t dot_audio_record_begin(void);
/* Caller owns returned WAV and must free it. Capture stops before returning. */
esp_err_t dot_audio_record_end(uint8_t **wav, size_t *length);
void dot_audio_record_cancel(void);
bool dot_audio_is_recording(void);
bool dot_audio_record_full(void);
void dot_audio_set_volume(unsigned volume);
unsigned dot_audio_volume(void);
/* Level follows volume-adjusted PCM sent to I2S, not network receipt. */
unsigned dot_audio_speech_level(void);
bool dot_audio_speaking(void);
void dot_audio_set_muted(bool muted);
bool dot_audio_muted(void);
/* Experimental energy-based speech candidates; semantic filtering is cloud-side. */
void dot_audio_set_handsfree(bool enabled);
bool dot_audio_detected_ready(void);
/* Caller owns the WAV. One candidate is retained until taken or cancelled. */
esp_err_t dot_audio_take_detected_wav(uint8_t **wav, size_t *length);
void dot_audio_cancel_playback(void);
/* Snapshot before checking the controller generation, then pass unchanged. */
uint32_t dot_audio_cancel_token(void);
/* Run on a worker task. Neither function logs credentials or input text. */
esp_err_t dot_openai_transcribe(const dot_config_t *config,
    const uint8_t *wav, size_t length, char *text, size_t capacity);
esp_err_t dot_openai_speak(const dot_config_t *config, const char *text);
esp_err_t dot_openai_transcribe_guarded(const dot_config_t *config,
    const uint8_t *wav, size_t length, char *text, size_t capacity,
    uint32_t expected_token);
esp_err_t dot_openai_speak_guarded(const dot_config_t *config, const char *text,
    uint32_t expected_token);
