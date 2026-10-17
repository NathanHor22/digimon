#pragma once
#include "host.h"
#define pdPASS 1
#define pdTRUE 1
#define ESP_EVENT_ANY_ID -1
#define WIFI_EVENT_STA_START 1
#define WIFI_EVENT_STA_DISCONNECTED 2
#define WIFI_EVENT_STA_STOP 3
#define IP_EVENT_STA_GOT_IP 4
#define IP_EVENT_STA_LOST_IP 5
#define WIFI_STORAGE_RAM 1
#define WIFI_MODE_STA 1
#define WIFI_IF_STA 1
#define WIFI_PS_NONE 0
#define WIFI_AUTH_OPEN 0
#define ESP_ERR_WIFI_NOT_CONNECT 0x300f
#define ESP_ERR_WIFI_STATE 0x3006
typedef const char *esp_event_base_t;
extern const char WIFI_EVENT[], IP_EVENT[];
typedef void (*esp_event_handler_t)(void *,esp_event_base_t,int32_t,void *);
typedef void *EventGroupHandle_t;
typedef struct { int unused; } esp_netif_t;
typedef struct { int unused; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
typedef struct { struct { unsigned char ssid[32],password[64]; struct { bool capable,required; } pmf_cfg; } sta; } wifi_config_t;
typedef struct { unsigned char ssid[33]; int rssi, authmode; } wifi_ap_record_t;
typedef struct { bool show_hidden; } wifi_scan_config_t;
EventGroupHandle_t xEventGroupCreate(void);
unsigned xEventGroupClearBits(EventGroupHandle_t,unsigned);
unsigned xEventGroupSetBits(EventGroupHandle_t,unsigned);
unsigned xEventGroupGetBits(EventGroupHandle_t);
int esp_netif_init(void);
int esp_event_loop_create_default(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
void esp_netif_destroy_default_wifi(void *);
int esp_event_handler_register(esp_event_base_t,int,esp_event_handler_t,void *);
int esp_event_handler_unregister(esp_event_base_t,int,esp_event_handler_t);
int esp_wifi_init(const wifi_init_config_t *);
int esp_wifi_deinit(void);
int esp_wifi_connect(void);
int esp_wifi_disconnect(void);
int esp_wifi_scan_start(const wifi_scan_config_t *,bool);
int esp_wifi_scan_get_ap_num(uint16_t *);
int esp_wifi_scan_get_ap_records(uint16_t *,wifi_ap_record_t *);
int esp_wifi_clear_ap_list(void);
int esp_wifi_set_storage(int);
int esp_wifi_set_mode(int);
int esp_wifi_set_config(int,const wifi_config_t *);
int esp_wifi_start(void);
int esp_wifi_stop(void);
int esp_wifi_set_ps(int);
int esp_wifi_sta_get_ap_info(wifi_ap_record_t *);
int xTaskCreate(void (*)(void *),const char *,unsigned,void *,unsigned,void *);
