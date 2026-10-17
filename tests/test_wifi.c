#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/dot_wifi.c"
const char WIFI_EVENT[]="wifi",IP_EVENT[]="ip";
static unsigned s_bits,s_connects,s_stops,s_deinits,s_netif_destroys;
static bool s_fail_config,s_fail_task;
static bool s_radio_held,s_fail_scan,s_scan_busy_once,s_fail_ap_count,s_fail_ap_records;
static int s_disconnect_error=ESP_OK;
static unsigned s_scans,s_disconnects,s_clears,s_configs;
static wifi_ap_record_t s_scan_records[70];
static uint16_t s_scan_record_count;
static esp_event_handler_t s_wifi_handler,s_ip_handler;
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &s_radio_held; }
int xSemaphoreTake(SemaphoreHandle_t mutex,unsigned long ticks)
{ assert(mutex==&s_radio_held);if(s_radio_held){assert(!ticks);return 0;}s_radio_held=true;return pdTRUE; }
int xSemaphoreGive(SemaphoreHandle_t mutex) { assert(mutex==&s_radio_held&&s_radio_held);s_radio_held=false;return pdTRUE; }
EventGroupHandle_t xEventGroupCreate(void) { return &s_bits; }
unsigned xEventGroupClearBits(EventGroupHandle_t group,unsigned bits) { assert(group==&s_bits);return s_bits&=~bits; }
unsigned xEventGroupSetBits(EventGroupHandle_t group,unsigned bits) { assert(group==&s_bits);return s_bits|=bits; }
unsigned xEventGroupGetBits(EventGroupHandle_t group) { assert(group==&s_bits);return s_bits; }
void vTaskDelay(unsigned ms) { (void)ms; }
int esp_netif_init(void) { return ESP_OK; }
int esp_event_loop_create_default(void) { return ESP_OK; }
esp_netif_t *esp_netif_create_default_wifi_sta(void) { static esp_netif_t netif;return &netif; }
void esp_netif_destroy_default_wifi(void *netif) { assert(netif);++s_netif_destroys; }
int esp_event_handler_register(esp_event_base_t base,int id,esp_event_handler_t handler,void *arg)
{ (void)arg;assert(id==ESP_EVENT_ANY_ID);if(base==WIFI_EVENT){assert(!s_wifi_handler);s_wifi_handler=handler;}else{assert(!s_ip_handler);s_ip_handler=handler;}return ESP_OK; }
int esp_event_handler_unregister(esp_event_base_t base,int id,esp_event_handler_t handler)
{ assert(id==ESP_EVENT_ANY_ID);if(base==WIFI_EVENT){assert(s_wifi_handler==handler);s_wifi_handler=NULL;}else{assert(s_ip_handler==handler);s_ip_handler=NULL;}return ESP_OK; }
int esp_wifi_init(const wifi_init_config_t *config) { assert(config);return ESP_OK; }
int esp_wifi_deinit(void) { ++s_deinits;return ESP_OK; }
int esp_wifi_connect(void) { ++s_connects;return ESP_OK; }
int esp_wifi_disconnect(void)
{ ++s_disconnects;if(s_disconnect_error!=ESP_OK)return s_disconnect_error;if(s_wifi_handler)s_wifi_handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL);return ESP_OK; }
int esp_wifi_scan_start(const wifi_scan_config_t *config,bool block)
{
    assert(!config&&block&&atomic_load(&s_scanning)&&s_radio_held);++s_scans;
    unsigned before=s_connects;reconnect_once();wifi_event(NULL,WIFI_EVENT,WIFI_EVENT_STA_START,NULL);
    assert(s_connects==before); // Both periodic/start callbacks are suppressed.
    if(s_scan_busy_once){s_scan_busy_once=false;return ESP_ERR_WIFI_STATE;}
    return s_fail_scan?ESP_FAIL:ESP_OK;
}
int esp_wifi_scan_get_ap_num(uint16_t *count)
{ if(s_fail_ap_count)return ESP_FAIL;*count=s_scan_record_count;return ESP_OK; }
int esp_wifi_scan_get_ap_records(uint16_t *count,wifi_ap_record_t *records)
{ assert(*count<=64);if(s_fail_ap_records)return ESP_FAIL;if(*count>s_scan_record_count)*count=s_scan_record_count;memcpy(records,s_scan_records,*count*sizeof(*records));return ESP_OK; }
int esp_wifi_clear_ap_list(void) { ++s_clears;return ESP_OK; }
int esp_wifi_set_storage(int mode) { assert(mode==WIFI_STORAGE_RAM);return ESP_OK; }
int esp_wifi_set_mode(int mode) { assert(mode==WIFI_MODE_STA);return ESP_OK; }
int esp_wifi_set_config(int interface,const wifi_config_t *wifi)
{ ++s_configs;assert(interface==WIFI_IF_STA);assert(wifi->sta.pmf_cfg.capable&&!wifi->sta.pmf_cfg.required);assert(!memcmp(wifi->sta.ssid,"test-wifi",9));return s_fail_config?ESP_FAIL:ESP_OK; }
int esp_wifi_start(void) { assert(s_wifi_handler);s_wifi_handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_START,NULL);return ESP_OK; }
int esp_wifi_stop(void) { ++s_stops;if(s_wifi_handler)s_wifi_handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_STOP,NULL);return ESP_OK; }
int esp_wifi_set_ps(int ps) { assert(ps==WIFI_PS_NONE);return ESP_OK; }
int esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap) { ap->rssi=-42;return ESP_OK; }
int xTaskCreate(void (*task)(void *),const char *name,unsigned stack,void *arg,unsigned priority,void *handle)
{ (void)arg;(void)priority;(void)handle;assert(task&&name&&stack>=3072);return s_fail_task?0:pdPASS; }
bool dot_config_wifi_ready(const dot_config_t *config) { return config&&config->wifi_ssid[0]; }

int main(void)
{
    dot_config_t config={0};strcpy(config.wifi_ssid,"test-wifi");strcpy(config.wifi_password,"test-password");
    dot_wifi_network_t networks[8];size_t capacity=8;
    assert(dot_wifi_scan(networks,&capacity)==ESP_ERR_INVALID_STATE&&!capacity);
    assert(dot_wifi_start(NULL)==ESP_ERR_INVALID_ARG);
    s_fail_config=true;assert(dot_wifi_start(&config)==ESP_FAIL);assert(!s_started&&!dot_wifi_connected());
    assert(s_deinits==1&&s_netif_destroys==1&&!s_wifi_handler&&!s_ip_handler);
    s_fail_config=false;s_fail_task=true;assert(dot_wifi_start(&config)==ESP_ERR_NO_MEM);
    assert(s_stops==1&&s_deinits==2&&s_netif_destroys==2&&!s_wifi_handler&&!s_ip_handler);
    assert(!dot_wifi_connected());
    s_fail_task=false;assert(dot_wifi_start(&config)==ESP_OK);assert(s_started&&s_connects==2);
    assert(!dot_wifi_connected());assert(dot_wifi_rssi()==-127);
    s_ip_handler(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,NULL);assert(dot_wifi_connected());assert(dot_wifi_rssi()==-42);
    s_ip_handler(NULL,IP_EVENT,IP_EVENT_STA_LOST_IP,NULL);assert(!dot_wifi_connected());
    s_ip_handler(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,NULL);assert(dot_wifi_connected());
    s_wifi_handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL);assert(!dot_wifi_connected());
    assert(dot_wifi_start(&config)==ESP_ERR_INVALID_STATE);

    s_scan_record_count=6;
    strcpy((char *)s_scan_records[0].ssid,"duplicate");s_scan_records[0].rssi=-70;s_scan_records[0].authmode=0;
    strcpy((char *)s_scan_records[1].ssid,"open-network");s_scan_records[1].rssi=-40;s_scan_records[1].authmode=0;
    strcpy((char *)s_scan_records[2].ssid,"duplicate");s_scan_records[2].rssi=-20;s_scan_records[2].authmode=3;
    memset(s_scan_records[3].ssid,'A',32);s_scan_records[3].ssid[32]=0;s_scan_records[3].rssi=-30;s_scan_records[3].authmode=3;
    s_scan_records[4].ssid[0]=0;s_scan_records[4].rssi=-10; // Hidden SSID skipped.
    strcpy((char *)s_scan_records[5].ssid,"weak");s_scan_records[5].rssi=-90;s_scan_records[5].authmode=3;
    struct { unsigned before;dot_wifi_network_t out[2];unsigned after; } bounded={.before=0x12345678,.after=0x87654321};
    capacity=2;unsigned connects=s_connects;assert(dot_wifi_scan(bounded.out,&capacity)==ESP_OK&&capacity==2);
    assert(bounded.before==0x12345678&&bounded.after==0x87654321);
    assert(!strcmp(bounded.out[0].ssid,"duplicate")&&bounded.out[0].rssi==-20&&bounded.out[0].secured);
    assert(strlen(bounded.out[1].ssid)==32&&bounded.out[1].ssid[32]==0);
    assert(s_connects==connects+1&&!atomic_load(&s_scanning)&&!s_radio_held);
    capacity=8;assert(dot_wifi_scan(networks,&capacity)==ESP_OK&&capacity==4);
    assert(!strcmp(networks[2].ssid,"open-network")&&!networks[2].secured);
    capacity=9;unsigned scans=s_scans;assert(dot_wifi_scan(networks,&capacity)==ESP_ERR_INVALID_ARG&&!capacity&&s_scans==scans);
    assert(dot_wifi_scan(networks,NULL)==ESP_ERR_INVALID_ARG);
    capacity=8;atomic_store(&s_scanning,true);assert(dot_wifi_scan(networks,&capacity)==ESP_ERR_INVALID_STATE&&!capacity);atomic_store(&s_scanning,false);
    s_fail_scan=true;capacity=8;connects=s_connects;unsigned clears=s_clears;
    assert(dot_wifi_scan(networks,&capacity)==ESP_FAIL&&!capacity&&!atomic_load(&s_scanning)&&!s_radio_held);
    assert(s_connects==connects+1&&s_clears==clears+1);s_fail_scan=false;
    s_scan_busy_once=true;capacity=8;scans=s_scans;assert(dot_wifi_scan(networks,&capacity)==ESP_OK&&s_scans==scans+2);
    s_fail_ap_count=true;capacity=8;assert(dot_wifi_scan(networks,&capacity)==ESP_FAIL&&!capacity&&!atomic_load(&s_scanning));s_fail_ap_count=false;
    s_fail_ap_records=true;capacity=8;assert(dot_wifi_scan(networks,&capacity)==ESP_FAIL&&!capacity&&!atomic_load(&s_scanning));s_fail_ap_records=false;
    s_disconnect_error=ESP_FAIL;capacity=8;assert(dot_wifi_scan(networks,&capacity)==ESP_FAIL&&!capacity&&!atomic_load(&s_scanning));s_disconnect_error=ESP_OK;

    // Simulate a reboot into an unconfigured device; the driver may scan but
    // must neither load a new station config nor attempt any connection.
    esp_wifi_stop();esp_event_handler_unregister(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event);
    esp_event_handler_unregister(IP_EVENT,ESP_EVENT_ANY_ID,wifi_event);esp_wifi_deinit();s_started=false;
    memset(&config,0,sizeof(config));connects=s_connects;unsigned configs=s_configs;
    assert(dot_wifi_start(&config)==ESP_OK&&s_connects==connects&&s_configs==configs);
    reconnect_once();assert(s_connects==connects);
    s_scan_record_count=0;capacity=8;assert(dot_wifi_scan(networks,&capacity)==ESP_OK&&!capacity&&s_connects==connects);
    assert(!atomic_load(&s_scanning)&&!s_radio_held);
    puts("Wi-Fi tests passed: startup cleanup/retry, blank-SSID scan-only startup, reconnect isolation, sorted/deduplicated bounded scans, failures resume, IP events, RSSI.");
    return 0;
}
