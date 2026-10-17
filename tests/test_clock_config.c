/* Real clock/config source with deterministic host time and NVS adapters. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "host.h"
static time_t host_clock_time(time_t *);
#define time host_clock_time
#define settimeofday host_settimeofday
#include "../main/dot_clock.c"
#undef time
#undef settimeofday
#include "../main/dot_config.c"

static int64_t s_host_us, s_saved_clock;
static time_t s_host_epoch;
static bool s_saved_available, s_settime_failed, s_commit_failed;
static unsigned s_clock_writes;
static unsigned char s_config_blob[4096];
static size_t s_config_blob_size;
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t h, unsigned long wait) { (void)h; (void)wait; return 1; }
int xSemaphoreGive(SemaphoreHandle_t h) { (void)h; return 1; }
int64_t esp_timer_get_time(void) { return s_host_us; }
static time_t host_clock_time(time_t *out) { if(out)*out=s_host_epoch;return s_host_epoch; }
int host_settimeofday(const struct timeval *tv,const void *zone)
{ (void)zone;if(s_settime_failed)return -1;s_host_epoch=tv->tv_sec;return 0; }
int nvs_flash_init(void) { return ESP_OK; }
int nvs_open(const char *space,int mode,nvs_handle_t *h)
{ (void)mode;*h=!strcmp(space,"dot_clock")?1:2;return ESP_OK; }
int nvs_get_i64(nvs_handle_t h,const char *key,int64_t *out)
{ (void)key;assert(h==1);if(!s_saved_available)return ESP_ERR_NVS_NOT_FOUND;*out=s_saved_clock;return ESP_OK; }
int nvs_set_i64(nvs_handle_t h,const char *key,int64_t epoch)
{ (void)key;assert(h==1);++s_clock_writes;s_saved_clock=epoch;s_saved_available=true;return ESP_OK; }
int nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *size)
{ (void)key;assert(h==2);if(!s_config_blob_size)return ESP_ERR_NVS_NOT_FOUND;if(*size<s_config_blob_size)return ESP_FAIL;memcpy(out,s_config_blob,s_config_blob_size);*size=s_config_blob_size;return ESP_OK; }
int nvs_set_blob(nvs_handle_t h,const char *key,const void *value,size_t size)
{ (void)key;assert(h==2&&size<=sizeof(s_config_blob));memcpy(s_config_blob,value,size);s_config_blob_size=size;return ESP_OK; }
int nvs_commit(nvs_handle_t h) { (void)h;return s_commit_failed?ESP_FAIL:ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

int main(void)
{
    int64_t epoch;
    assert(parse_http_date("Sat, 17 Oct 2026 12:34:56 GMT",&epoch));assert(epoch==INT64_C(1792240496));
    assert(parse_http_date("Thu, 29 Feb 2024 00:00:00 GMT",&epoch));assert(epoch==INT64_C(1709164800));
    const char *bad[]={"XXX, 17 Oct 2026 12:34:56 GMT","Sun, 17 Oct 2026 12:34:56 GMT", "Sat, 17 J 2026 12:34:56 GMT", "Sat, 31 Feb 2026 12:34:56 GMT", "Sat, 17 Oct 2026 24:34:56 GMT", "Sat, 17 Oct 2026 12:34:60 GMT", "Sat, 17 Oct 2026 12:34:56 UTC", "Sat, 17 Oct 2026 12:34:56 GMT extra", "Sat, 17 oct 2026 12:34:56 GMT", "Thu, 29 Feb 2025 00:00:00 GMT"};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i)assert(!parse_http_date(bad[i],&epoch));
    assert(dot_clock_init(0)==ESP_ERR_INVALID_STATE);assert(!dot_clock_bootstrap_ready());assert(!dot_clock_fresh());
    assert(dot_clock_init(MAX_EPOCH)==ESP_ERR_INVALID_STATE);assert(!dot_clock_bootstrap_ready());
    s_saved_available=true;s_saved_clock=MAX_EPOCH+100;
    assert(dot_clock_init(INT64_C(1792240000))==ESP_OK);assert(dot_clock_bootstrap_ready());assert(!dot_clock_fresh());
    dot_clock_observe_http_date("Sat, 17 Oct 2026 12:34:56 GMT");assert(dot_clock_fresh());assert(s_host_epoch==INT64_C(1792240496));
    s_host_us=FRESH_US-1;assert(dot_clock_fresh());s_host_us=FRESH_US;assert(!dot_clock_fresh());
    s_host_us=10;dot_clock_observe_http_date("Sat, 17 Oct 2026 12:34:56 GMT");assert(dot_clock_fresh());
    s_host_us=9;assert(!dot_clock_fresh());s_host_us=10;
    dot_clock_checkpoint();assert(s_clock_writes==1);dot_clock_checkpoint();assert(s_clock_writes==1);
    s_host_us+=CHECKPOINT_US;s_host_epoch+=3600;dot_clock_checkpoint();assert(s_clock_writes==2);
    assert(dot_clock_init(INT64_C(1792240000))==ESP_OK);assert(!dot_clock_fresh());assert(s_host_epoch==s_saved_clock);
    dot_clock_observe_http_date("Sat, 17 Oct 2026 12:34:56 GMT");assert(!dot_clock_fresh());
    s_settime_failed=true;assert(dot_clock_init(INT64_C(1792240000))==ESP_FAIL);assert(!dot_clock_bootstrap_ready());s_settime_failed=false;

    dot_config_t config;assert(dot_config_init(&config)==ESP_OK);assert(valid_config(&config));
    assert(!dot_config_audio_ready(&config)&&!dot_config_wifi_ready(&config)&&!dot_config_tunnel_ready(&config));
    strcpy(config.wifi_ssid,"test-wifi");strcpy(config.wifi_password,"eight-ok");assert(dot_config_wifi_ready(&config));
    strcpy(config.audio_api_key,"test-key");assert(dot_config_audio_ready(&config));
    strcpy(config.tunnel_api_key,"test-tunnel-key");strcpy(config.tunnel_id,"tnl_personal");config.tunnel_scope_auth=true;
    assert(dot_config_tunnel_ready(&config));assert(dot_config_save(&config)==ESP_OK);
    dot_config_t loaded;assert(dot_config_init(&loaded)==ESP_OK);assert(!memcmp(&loaded,&config,sizeof(config)));
    dot_config_t invalid=config;memset(invalid.audio_api_key,'x',sizeof(invalid.audio_api_key));assert(dot_config_save(&invalid)==ESP_ERR_INVALID_ARG);assert(!dot_config_audio_ready(&invalid));
    invalid=config;strcpy(invalid.tunnel_api_key,"key\r\nAuthorization: other");assert(!valid_config(&invalid));
    invalid=config;strcpy(invalid.wifi_password,"short");assert(!valid_config(&invalid));
    invalid=config;memset(invalid.wifi_password,'A',64);invalid.wifi_password[64]=0;assert(valid_config(&invalid));invalid.wifi_password[1]='z';assert(!valid_config(&invalid));
    invalid=config;invalid.wifi_password[0]=0;assert(valid_config(&invalid));
    invalid=config;invalid.provisioned_epoch=MAX_EPOCH;assert(!valid_config(&invalid));
    invalid=config;strcpy(invalid.device_id,"id with spaces");assert(!valid_config(&invalid));
    invalid=config;((unsigned char *)&invalid.tunnel_scope_auth)[0]=2;assert(!valid_config(&invalid));
    s_config_blob[0]=99;assert(dot_config_init(&loaded)==ESP_ERR_INVALID_STATE);
    puts("Clock/config tests passed: strict UTC dates, calendar/weekday validation, freshness, rollback/re-init, checkpoint rate, invalid checkpoint recovery, persistence, token/header validation, Wi-Fi PSK, authflag and bounds.");
    return 0;
}
