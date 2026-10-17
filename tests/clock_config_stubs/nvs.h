#pragma once
#include "host.h"
#define ESP_ERR_NVS_NOT_FOUND 0x1102
int nvs_get_i64(nvs_handle_t, const char *, int64_t *);
int nvs_set_i64(nvs_handle_t, const char *, int64_t);
