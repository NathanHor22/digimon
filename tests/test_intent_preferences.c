/* Production semantic gate + preferences, with fake transport/time/NVS only.
 * This tests acceptance policy and protocol handling, not model accuracy. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/dot_preferences.c"
#include "../main/dot_intent.c"

static unsigned char s_blob[4096];static size_t s_blob_length;
static bool s_nvs_fail,s_truncated,s_endless,s_oversized,s_cancel_on_headers,s_clock_ok=true;
static bool s_actual_muted;
static unsigned s_writes,s_http_calls,s_reads;static uint32_t s_cancel_token;
static int s_status=200;static int64_t s_monotonic=1000,s_header_delay,s_read_delay;
static const char *s_label="addressed";
static const char *s_custom_response;
static char s_last_request[8192];
struct host_client { esp_http_client_config_t cfg; char request[8192],reply[9000];size_t sent,expected,read,length; };
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t h,unsigned long wait) { (void)h;(void)wait;return 1; }
int xSemaphoreGive(SemaphoreHandle_t h) { (void)h;return 1; }
int nvs_open(const char *space,int mode,nvs_handle_t *h) { (void)mode;assert(!strcmp(space,"dot_prefs"));*h=1;return ESP_OK; }
int nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *length)
{ (void)h;assert(!strcmp(key,"profile"));if(!s_blob_length)return ESP_ERR_NVS_NOT_FOUND;assert(*length>=s_blob_length);memcpy(out,s_blob,s_blob_length);*length=s_blob_length;return ESP_OK; }
int nvs_set_blob(nvs_handle_t h,const char *key,const void *value,size_t length)
{ (void)h;(void)key;if(s_nvs_fail)return ESP_FAIL;assert(length<=sizeof(s_blob));memcpy(s_blob,value,length);s_blob_length=length;++s_writes;return ESP_OK; }
int nvs_commit(nvs_handle_t h) { (void)h;return s_nvs_fail?ESP_FAIL:ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
uint32_t dot_audio_cancel_token(void) { return s_cancel_token; }
bool dot_audio_muted(void) { return s_actual_muted; }
bool dot_clock_fresh(void) { return s_clock_ok; }
void dot_clock_observe_http_date(const char *date) { (void)date; }
int64_t esp_timer_get_time(void) { return s_monotonic; }
int esp_crt_bundle_attach(void *value) { (void)value;return ESP_OK; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg)
{
    assert(!strcmp(cfg->url,"https://api.openai.com/v1/responses"));assert(cfg->crt_bundle_attach&&cfg->disable_auto_redirect);
    struct host_client *client=calloc(1,sizeof(*client));assert(client);client->cfg=*cfg;++s_http_calls;return client;
}
int esp_http_client_set_header(esp_http_client_handle_t c,const char *key,const char *value)
{ (void)c;if(!strcmp(key,"Authorization"))assert(!strcmp(value,"Bearer fixture-key"));return ESP_OK; }
int esp_http_client_set_timeout_ms(esp_http_client_handle_t c,int timeout)
{ assert(timeout>0&&timeout<=10000);c->cfg.timeout_ms=timeout;return ESP_OK; }
int esp_http_client_open(esp_http_client_handle_t c,int length) { assert(length>0&&(size_t)length<sizeof(c->request));c->expected=length;return ESP_OK; }
int esp_http_client_write(esp_http_client_handle_t c,const char *bytes,int length)
{ assert(length>0);if(length>7)length=7;memcpy(c->request+c->sent,bytes,length);c->sent+=length;c->request[c->sent]=0;return length; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c)
{
    assert(c->sent==c->expected);strcpy(s_last_request,c->request);
    s_monotonic+=s_header_delay;if(s_cancel_on_headers)++s_cancel_token;
    if(s_custom_response)snprintf(c->reply,sizeof(c->reply),"%s",s_custom_response);
    else snprintf(c->reply,sizeof(c->reply),"{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"status\":\"completed\",\"content\":[{\"type\":\"output_text\",\"text\":\"{\\\"decision\\\":\\\"%s\\\"}\"}]}]}",s_label);
    c->length=strlen(c->reply);
    esp_http_client_event_t event={.event_id=HTTP_EVENT_ON_HEADER,.client=c,.user_data=c->cfg.user_data,.header_key="Date",.header_value="Sat, 17 Oct 2026 12:34:56 GMT"};
    c->cfg.event_handler(&event);
    return s_oversized?INTENT_RESPONSE_MAX+1:s_endless?0:(int64_t)c->length;
}
int esp_http_client_get_status_code(esp_http_client_handle_t c) { (void)c;return s_status; }
int esp_http_client_read(esp_http_client_handle_t c,char *out,int count)
{ assert(count==1);++s_reads;s_monotonic+=s_read_delay;if(s_endless){*out='x';return 1;}if(s_truncated&&c->read+1>=c->length)return 0;if(c->read>=c->length)return 0;*out=c->reply[c->read++];return 1; }
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) { return !s_endless&&c->read>=c->length; }
int esp_http_client_close(esp_http_client_handle_t c) { (void)c;return ESP_OK; }
void esp_http_client_cleanup(esp_http_client_handle_t c) { free(c); }

static esp_err_t patch(const char *json,bool physical)
{ cJSON *p=cJSON_Parse(json);assert(p);int err=dot_preferences_update(p,physical);cJSON_Delete(p);return err; }
static void invalid_response(const char *body)
{ dot_intent_decision_t out={0};assert(intent_parse_response(body,strlen(body),&out)!=ESP_OK);assert(!dot_intent_should_accept(&out,true)); }
int main(void)
{
    assert(dot_preferences_init()==ESP_OK);dot_preferences_t pref;assert(dot_preferences_get(&pref)==ESP_OK);
    assert(pref.volume_percent==60&&!pref.microphone_muted&&pref.hands_free&&!strcmp(pref.device_name,"Digimon"));assert(pref.alias_count==1&&!strcmp(pref.aliases[0],"mommy"));
    assert(patch("{\"volume_percent\":70,\"personality\":\"Concise and candid\",\"aliases\":[\"buddy\",\"mommy\"]}",false)==ESP_OK);
    assert(dot_preferences_get(&pref)==ESP_OK);assert(pref.volume_percent==70&&pref.alias_count==2&&pref.revision==2);
    unsigned writes=s_writes;assert(patch("{\"volume_percent\":70}",false)==ESP_OK);assert(writes==s_writes);
    assert(patch("{\"microphone_muted\":true}",false)==ESP_ERR_NOT_SUPPORTED);assert(patch("{\"microphone_muted\":false}",false)==ESP_ERR_NOT_SUPPORTED);
    assert(patch("{\"volume_percent\":20,\"aliases\":[\"Buddy\",\"buddy\"]}",false)==ESP_ERR_INVALID_ARG);
    assert(dot_preferences_get(&pref)==ESP_OK&&pref.volume_percent==70);
    assert(patch("{\"volume_percent\":-1}",false)==ESP_ERR_INVALID_ARG);assert(patch("{\"volume_percent\":10.5}",false)==ESP_ERR_INVALID_ARG);
    assert(patch("{\"volume_percent\":20,\"volume_percent\":30}",false)==ESP_ERR_INVALID_ARG);
    assert(patch("{\"device_name\":\" bad\"}",false)==ESP_ERR_INVALID_ARG);assert(patch("{\"personality\":\"bad\\ncontrol\"}",false)==ESP_ERR_INVALID_ARG);
    assert(patch("{\"aliases\":[\"1\",\"2\",\"3\",\"4\",\"5\"]}",false)==ESP_ERR_INVALID_ARG);
    s_nvs_fail=true;assert(patch("{\"volume_percent\":30}",false)!=ESP_OK);s_nvs_fail=false;assert(dot_preferences_get(&pref)==ESP_OK&&pref.volume_percent==70);
    assert(dot_preferences_init()==ESP_OK);assert(dot_preferences_get(&pref)==ESP_OK&&pref.volume_percent==70);
    assert(patch("{\"microphone_muted\":true}",true)==ESP_OK);assert(dot_preferences_get(&pref)==ESP_OK&&pref.microphone_muted);
    assert(patch("{\"hands_free\":true}",false)==ESP_OK);assert(dot_preferences_get(&pref)==ESP_OK&&pref.microphone_muted);
    assert(patch("{\"microphone_muted\":false}",true)==ESP_OK);
    dot_config_t config={0};strcpy(config.audio_api_key,"fixture-key");dot_intent_decision_t out;
    assert(dot_intent_classify(&config,"Buddy, can you help?","",false,s_cancel_token,&out)==ESP_OK);
    assert(out.valid&&out.addressed&&dot_intent_should_accept(&out,false));
    cJSON *request=cJSON_Parse(s_last_request);assert(request);assert(intent_string_is(request,"model",DOT_INTENT_MODEL));assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(request,"store")));
    const cJSON *format=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(request,"text"),"format");assert(intent_string_is(format,"type","json_schema"));assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(format,"strict")));
    const cJSON *input=cJSON_GetObjectItemCaseSensitive(request,"input");assert(strstr(input->valuestring,"buddy")&&strstr(input->valuestring,"mommy"));assert(!strstr(input->valuestring,"Concise and candid"));cJSON_Delete(request);
    s_label="ambient";assert(dot_intent_classify(&config,"Mommy is at work","",true,s_cancel_token,&out)==ESP_OK);assert(out.ambient&&!dot_intent_should_accept(&out,true));
    s_label="uncertain";assert(dot_intent_classify(&config,"What about that?","",false,s_cancel_token,&out)==ESP_OK);assert(out.uncertain&&!dot_intent_should_accept(&out,false)&&dot_intent_should_accept(&out,true));
    s_status=401;assert(dot_intent_classify(&config,"hello",NULL,true,s_cancel_token,&out)!=ESP_OK);assert(!out.valid&&!dot_intent_should_accept(&out,true));s_status=200;
    s_truncated=true;assert(dot_intent_classify(&config,"hello",NULL,true,s_cancel_token,&out)!=ESP_OK);assert(!out.valid);s_truncated=false;
    s_oversized=true;s_reads=0;assert(dot_intent_classify(&config,"hello",NULL,true,s_cancel_token,&out)==ESP_ERR_INVALID_SIZE);assert(!s_reads);s_oversized=false;
    s_endless=true;s_reads=0;assert(dot_intent_classify(&config,"hello",NULL,true,s_cancel_token,&out)==ESP_ERR_INVALID_SIZE);assert(s_reads==INTENT_RESPONSE_MAX+1);s_endless=false;
    s_header_delay=INTENT_TIMEOUT_US+1;assert(dot_intent_classify(&config,"hello",NULL,true,s_cancel_token,&out)==ESP_ERR_TIMEOUT);assert(!out.valid);s_header_delay=0;
    s_endless=true;s_read_delay=INT64_C(2000000);assert(dot_intent_classify(&config,"hello",NULL,true,s_cancel_token,&out)==ESP_ERR_TIMEOUT);s_endless=false;s_read_delay=0;
    s_cancel_on_headers=true;assert(dot_intent_classify(&config,"hello",NULL,true,s_cancel_token,&out)!=ESP_OK);assert(!dot_intent_should_accept(&out,true));s_cancel_on_headers=false;
    writes=s_http_calls;assert(dot_intent_classify(&config,"hello",NULL,true,s_cancel_token-1,&out)!=ESP_OK);assert(writes==s_http_calls);
    assert(patch("{\"microphone_muted\":true}",true)==ESP_OK);assert(dot_intent_classify(&config,"hello",NULL,true,s_cancel_token,&out)!=ESP_OK);assert(writes==s_http_calls);
    assert(patch("{\"microphone_muted\":false}",true)==ESP_OK);s_clock_ok=false;assert(dot_intent_classify(&config,"hello",NULL,true,s_cancel_token,&out)!=ESP_OK);assert(writes==s_http_calls);s_clock_ok=true;
    s_actual_muted=true;assert(dot_preferences_get(&pref)==ESP_OK&&!pref.microphone_muted);
    assert(dot_intent_classify(&config,"hello",NULL,true,s_cancel_token,&out)!=ESP_OK);assert(writes==s_http_calls&&!out.valid);
    s_actual_muted=false;
    invalid_response("{\"status\":\"incomplete\",\"output\":[]}");
    invalid_response("{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"status\":\"completed\",\"content\":[{\"type\":\"refusal\",\"refusal\":\"No\"}]}]}");
    invalid_response("{\"status\":\"completed\",\"output\":[]} trailing");
    s_blob[0]=99;assert(dot_preferences_init()!=ESP_OK);assert(dot_preferences_get(&pref)==ESP_OK&&pref.microphone_muted&&!pref.hands_free);
    puts("Intent/preferences tests passed: strict schema request and parsing, ambient/uncertain policy, refusal/incomplete/HTTP failure quietness, bounded I/O, cancellation/mute, atomic validation, persistence/revision, physical-only microphone control, safe corrupt-state recovery.");
    return 0;
}
