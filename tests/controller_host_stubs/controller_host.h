#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../audio_host_stubs/audio_host.h"
typedef void *QueueHandle_t;
int xQueueSend(QueueHandle_t, const void *, unsigned);
size_t strlcpy(char *, const char *, size_t);
void esp_fill_random(void *, size_t);
const char *esp_err_to_name(int);
unsigned esp_get_free_heap_size(void);
typedef struct { int unused; } mbedtls_md_info_t;
#define MBEDTLS_MD_SHA256 1
const mbedtls_md_info_t *mbedtls_md_info_from_type(int);
int mbedtls_md(const mbedtls_md_info_t *, const unsigned char *, size_t, unsigned char *);
