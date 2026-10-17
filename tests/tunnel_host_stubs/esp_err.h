#pragma once
/* Protocol-only host build: no ESP implementation or network stack is mocked.
 * The production header merely needs the ESP-IDF error type for declarations. */
typedef int esp_err_t;
