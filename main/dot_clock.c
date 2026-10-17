#include "dot_clock.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define MIN_EPOCH INT64_C(1704067200)
#define MAX_EPOCH INT64_C(4102444800)
#define FRESH_US INT64_C(21600000000)
#define CHECKPOINT_US INT64_C(3600000000)
static SemaphoreHandle_t s_clock_lock;
static bool s_verified;
static bool s_has_checkpoint;
static int64_t s_verified_at;
static int64_t s_lower_bound;
static int64_t s_checkpoint_at;

static bool valid_epoch(int64_t epoch)
{ return epoch >= MIN_EPOCH && epoch < MAX_EPOCH; }
static bool fresh_locked(int64_t now)
{ return s_verified && now >= s_verified_at && now - s_verified_at < FRESH_US; }
static int decimal(const char *text, size_t count)
{
    int value = 0;
    for (size_t i = 0; i < count; ++i) {
        if (text[i] < '0' || text[i] > '9') return -1;
        value = value * 10 + text[i] - '0';
    }
    return value;
}

/* Exact IMF-fixdate; no ambient timezone or normalized invalid calendar dates.
 * This is the Date form emitted by OpenAI. UTC range: 2024 through 2099. */
static bool parse_http_date(const char *value, int64_t *epoch)
{
    if (!value || !epoch || strlen(value) != 29 || value[3] != ',' ||
        value[4] != ' ' || value[7] != ' ' || value[11] != ' ' || value[16] != ' ' ||
        value[19] != ':' || value[22] != ':' || memcmp(value + 25, " GMT", 4)) return false;
    static const char months[][4] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    static const char weekdays[][4] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const int month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int month = -1, weekday = -1;
    for (int i = 0; i < 12; ++i) if (!memcmp(value + 8, months[i], 3)) month = i;
    for (int i = 0; i < 7; ++i) if (!memcmp(value, weekdays[i], 3)) weekday = i;
    int day = decimal(value + 5, 2), year = decimal(value + 12, 4);
    int hour = decimal(value + 17, 2), minute = decimal(value + 20, 2), second = decimal(value + 23, 2);
    if (month < 0 || weekday < 0 || year < 2024 || year > 2099 || day < 1 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return false;
    bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (day > month_days[month] + (month == 1 && leap ? 1 : 0)) return false;
    int64_t days = 0;
    for (int y = 1970; y < year; ++y)
        days += 365 + (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0) ? 1 : 0);
    for (int m = 0; m < month; ++m) days += month_days[m] + (m == 1 && leap ? 1 : 0);
    days += day - 1;
    if ((days + 4) % 7 != weekday) return false;
    *epoch = days * 86400 + hour * 3600 + minute * 60 + second;
    return valid_epoch(*epoch);
}

esp_err_t dot_clock_init(int64_t provisioned_epoch)
{
    if (!s_clock_lock) s_clock_lock = xSemaphoreCreateMutex();
    if (!s_clock_lock) return ESP_ERR_NO_MEM;
    int64_t saved = 0;
    nvs_handle_t handle;
    if (nvs_open("dot_clock", NVS_READONLY, &handle) == ESP_OK) {
        (void)nvs_get_i64(handle, "epoch", &saved);
        nvs_close(handle);
    }
    if (!valid_epoch(saved)) saved = 0;
    if (!valid_epoch(provisioned_epoch)) provisioned_epoch = 0;
    int64_t lower = provisioned_epoch > saved ? provisioned_epoch : saved;
    xSemaphoreTake(s_clock_lock, portMAX_DELAY);
    s_verified = false; s_has_checkpoint = false;
    s_verified_at = 0; s_checkpoint_at = 0; s_lower_bound = 0;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (valid_epoch(lower)) {
        struct timeval tv = { .tv_sec = (time_t)lower };
        result = settimeofday(&tv, NULL) == 0 ? ESP_OK : ESP_FAIL;
        if (result == ESP_OK) s_lower_bound = lower;
    }
    xSemaphoreGive(s_clock_lock);
    return result;
}

bool dot_clock_bootstrap_ready(void)
{
    if (!s_clock_lock) return false;
    xSemaphoreTake(s_clock_lock, portMAX_DELAY);
    bool ready = valid_epoch(s_lower_bound);
    xSemaphoreGive(s_clock_lock);
    return ready;
}
bool dot_clock_fresh(void)
{
    if (!s_clock_lock) return false;
    xSemaphoreTake(s_clock_lock, portMAX_DELAY);
    bool fresh = fresh_locked(esp_timer_get_time());
    xSemaphoreGive(s_clock_lock);
    return fresh;
}
void dot_clock_observe_http_date(const char *value)
{
    int64_t epoch;
    if (!s_clock_lock || !parse_http_date(value, &epoch)) return;
    xSemaphoreTake(s_clock_lock, portMAX_DELAY);
    if (valid_epoch(s_lower_bound) && epoch >= s_lower_bound - 300) {
        struct timeval tv = { .tv_sec = (time_t)epoch };
        if (settimeofday(&tv, NULL) == 0) {
            if (epoch > s_lower_bound) s_lower_bound = epoch;
            s_verified_at = esp_timer_get_time(); s_verified = true;
        }
    }
    xSemaphoreGive(s_clock_lock);
}
void dot_clock_checkpoint(void)
{
    if (!s_clock_lock) return;
    xSemaphoreTake(s_clock_lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time(), epoch = (int64_t)time(NULL);
    bool due = fresh_locked(now) && valid_epoch(epoch) &&
        (!s_has_checkpoint || (now >= s_checkpoint_at && now - s_checkpoint_at >= CHECKPOINT_US));
    if (due) {
        nvs_handle_t handle;
        if (nvs_open("dot_clock", NVS_READWRITE, &handle) == ESP_OK) {
            if (nvs_set_i64(handle, "epoch", epoch) == ESP_OK && nvs_commit(handle) == ESP_OK) {
                s_checkpoint_at = now; s_has_checkpoint = true;
                if (epoch > s_lower_bound) s_lower_bound = epoch;
            }
            nvs_close(handle);
        }
    }
    xSemaphoreGive(s_clock_lock);
}