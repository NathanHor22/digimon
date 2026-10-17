#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t dot_clock_init(int64_t provisioned_epoch);
bool dot_clock_bootstrap_ready(void);
bool dot_clock_fresh(void);
/* Only pass Date from a successfully verified TLS connection to api.openai.com. */
void dot_clock_observe_http_date(const char *value);
void dot_clock_checkpoint(void);

