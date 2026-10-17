/* Native Windows harness exercising the real dot_mcp.c. Only platform/network
 * functions are mocked. SHA-256/HMAC is provided by Windows BCrypt; cJSON is
 * the upstream ESP-IDF-pinned implementation, not a replacement parser. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/dot_mcp.c"
#include "../main/dot_preferences.c"

static unsigned s_calls, s_reply_calls, s_random;
static int s_mock_status = 200, s_mock_error;
static bool s_wrong_challenge, s_private_dns, s_pending = true;
static bool s_clock_fresh = true;
static bool s_actual_muted;
static bool s_endless_body, s_oversized_body, s_truncated_body;
static int64_t s_time_offset_us, s_header_delay_us, s_read_delay_us;
static unsigned s_partial_write_max, s_reads;
static unsigned s_nvs_failure;
static unsigned char s_blob[4096];
static size_t s_blob_length;
static unsigned char s_preferences_blob[4096];
static size_t s_preferences_blob_length;
static char s_last_body[8192], s_last_signature[128], s_last_timestamp[32], s_last_id[96];
static cJSON *s_fixtures;
struct host_client {
    esp_http_client_config_t config;
    char body[8192], response[2048];
    size_t body_length, sent, response_length, read;
};

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t x, unsigned long y) { (void)x; (void)y; return 1; }
int xSemaphoreGive(SemaphoreHandle_t x) { (void)x; return 1; }
void vTaskDelay(unsigned x) { (void)x; }
void esp_fill_random(void *out, size_t size) { unsigned char *p = out; while (size--) *p++ = ++s_random; }
int64_t esp_timer_get_time(void) { return (int64_t)GetTickCount64() * 1000 + s_time_offset_us; }
const char *esp_err_to_name(int x) { (void)x; return "mock_error"; }
int esp_crt_bundle_attach(void *x) { (void)x; return 0; }
int nvs_open(const char *x, int y, nvs_handle_t *h) { (void)y; *h = !strcmp(x,"dot_prefs")?2:1; return 0; }
int nvs_set_blob(nvs_handle_t h, const char *k, const void *b, size_t n)
{ (void)k; if (s_nvs_failure) return ESP_FAIL; assert(n <= sizeof(s_blob)); if(h==2){memcpy(s_preferences_blob,b,n);s_preferences_blob_length=n;}else{memcpy(s_blob,b,n);s_blob_length=n;}return 0; }
int nvs_get_blob(nvs_handle_t h, const char *k, void *b, size_t *n)
{ (void)k;size_t length=h==2?s_preferences_blob_length:s_blob_length; if(!length)return ESP_ERR_NVS_NOT_FOUND;if(*n<length)return ESP_FAIL;memcpy(b,h==2?s_preferences_blob:s_blob,length);*n=length;return 0; }
int nvs_commit(nvs_handle_t h) { (void)h; return s_nvs_failure ? ESP_FAIL : 0; }
void nvs_close(nvs_handle_t h) { (void)h; }
uint32_t ntohl(uint32_t n) { return ((n&255)<<24)|((n&65280)<<8)|((n>>8)&65280)|(n>>24); }
int getaddrinfo(const char *host, const char *service, const struct addrinfo *hint, struct addrinfo **out)
{
    (void)host; (void)service; (void)hint;
    static struct sockaddr_in address;
    static struct addrinfo info;
    address.sin_addr.s_addr = ntohl(s_private_dns ? 0x7f000001 : 0x5db8d822);
    info.ai_family = AF_INET; info.ai_addrlen = sizeof(address); info.ai_addr = (struct sockaddr *)&address;
    *out = &info; return 0;
}
void freeaddrinfo(struct addrinfo *x) { (void)x; }
const char *inet_ntop(int family, const void *address, char *dest, size_t n)
{ (void)family; uint32_t v=ntohl(((const struct in_addr *)address)->s_addr); snprintf(dest,n,"%u.%u.%u.%u",v>>24,(v>>16)&255,(v>>8)&255,v&255); return dest; }

const mbedtls_md_info_t *mbedtls_md_info_from_type(int t)
{ static const mbedtls_md_info_t info={0}; return t==MBEDTLS_MD_SHA256?&info:NULL; }
void mbedtls_md_init(mbedtls_md_context_t *c) { memset(c,0,sizeof(*c)); }
int mbedtls_md_setup(mbedtls_md_context_t *c, const mbedtls_md_info_t *i, int h)
{
    (void)i; c->hmac=h!=0;
    return BCryptOpenAlgorithmProvider((BCRYPT_ALG_HANDLE *)&c->algorithm,BCRYPT_SHA256_ALGORITHM,NULL,h?BCRYPT_ALG_HANDLE_HMAC_FLAG:0)<0?-1:0;
}
static int hash_start(mbedtls_md_context_t *c,const unsigned char *key,size_t len)
{ return BCryptCreateHash(c->algorithm,(BCRYPT_HASH_HANDLE *)&c->handle,NULL,0,(PUCHAR)key,(ULONG)len,0)<0?-1:0; }
int mbedtls_md_starts(mbedtls_md_context_t *c) { return hash_start(c,NULL,0); }
int mbedtls_md_update(mbedtls_md_context_t *c,const unsigned char *p,size_t n)
{ return BCryptHashData(c->handle,(PUCHAR)p,(ULONG)n,0)<0?-1:0; }
int mbedtls_md_finish(mbedtls_md_context_t *c,unsigned char *out)
{ return BCryptFinishHash(c->handle,out,32,0)<0?-1:0; }
void mbedtls_md_free(mbedtls_md_context_t *c)
{ if(c->handle)BCryptDestroyHash(c->handle); if(c->algorithm)BCryptCloseAlgorithmProvider(c->algorithm,0); }
int mbedtls_md_hmac_starts(mbedtls_md_context_t *c,const unsigned char *k,size_t n) { return hash_start(c,k,n); }
int mbedtls_md_hmac_update(mbedtls_md_context_t *c,const unsigned char *p,size_t n) { return mbedtls_md_update(c,p,n); }
int mbedtls_md_hmac_finish(mbedtls_md_context_t *c,unsigned char *o) { return mbedtls_md_finish(c,o); }
static const char base64chars[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
int mbedtls_base64_encode(unsigned char *out,size_t cap,size_t *used,const unsigned char *in,size_t n)
{
    *used=4*((n+2)/3); if(cap<=*used)return -1;
    size_t j=0; for(size_t i=0;i<n;i+=3) {
        unsigned v=(unsigned)in[i]<<16; if(i+1<n)v|=(unsigned)in[i+1]<<8; if(i+2<n)v|=in[i+2];
        out[j++]=base64chars[(v>>18)&63]; out[j++]=base64chars[(v>>12)&63];
        out[j++]=i+1<n?base64chars[(v>>6)&63]:'='; out[j++]=i+2<n?base64chars[v&63]:'=';
    } out[j]=0; return 0;
}
int mbedtls_base64_decode(unsigned char *out,size_t cap,size_t *used,const unsigned char *in,size_t n)
{
    if(n%4)return -1; *used=0;
    for(size_t i=0;i<n;i+=4) { unsigned v=0,padding=0;
        for(size_t j=0;j<4;++j) { if(in[i+j]=='='){++padding;v<<=6;} else {
            const char *p=strchr(base64chars,in[i+j]); if(!p||padding)return -1; v=(v<<6)|(unsigned)(p-base64chars); } }
        if(padding>2||(*used+3-padding)>cap)return -1;
        out[(*used)++]=(v>>16)&255; if(padding<2)out[(*used)++]=(v>>8)&255; if(!padding)out[(*used)++]=v&255;
    } return 0;
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg)
{
    assert(cfg->disable_auto_redirect); assert(cfg->crt_bundle_attach);
    assert(!strcmp(cfg->common_name,"receiver.example.com"));
    assert(strstr(cfg->url,"https://93.184.216.34/")==cfg->url);
    struct host_client *c=calloc(1,sizeof(*c)); c->config=*cfg; return c;
}
int esp_http_client_set_header(esp_http_client_handle_t c,const char *k,const char *v)
{
    (void)c;
    if(!strcmp(k,"Host"))assert(!strcmp(v,"receiver.example.com"));
    if(!strcmp(k,"webhook-id"))snprintf(s_last_id,sizeof(s_last_id),"%s",v);
    if(!strcmp(k,"webhook-timestamp"))snprintf(s_last_timestamp,sizeof(s_last_timestamp),"%s",v);
    if(!strcmp(k,"webhook-signature"))snprintf(s_last_signature,sizeof(s_last_signature),"%s",v);
    return 0;
}
int esp_http_client_set_post_field(esp_http_client_handle_t c,const char *b,int n)
{ (void)c;(void)b;(void)n;assert(!"set_post_field is not used by bounded streaming client");return ESP_FAIL; }
int esp_http_client_perform(esp_http_client_handle_t c)
{ (void)c;assert(!"perform ignores event callback aborts and must not be used");return ESP_FAIL; }
int esp_http_client_set_timeout_ms(esp_http_client_handle_t c,int ms)
{ assert(ms>0&&ms<=WEBHOOK_TIMEOUT_MS);c->config.timeout_ms=ms;return ESP_OK; }
int esp_http_client_open(esp_http_client_handle_t c,int length)
{ assert(length>0&&(size_t)length<sizeof(c->body));c->body_length=(size_t)length;return ESP_OK; }
int esp_http_client_write(esp_http_client_handle_t c,const char *body,int length)
{
    assert(length>0&&c->sent+(size_t)length<=c->body_length);
    if(s_partial_write_max&&(unsigned)length>s_partial_write_max)length=(int)s_partial_write_max;
    memcpy(c->body+c->sent,body,(size_t)length);c->sent+=(size_t)length;c->body[c->sent]=0;return length;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c)
{
    assert(c->sent==c->body_length);
    ++s_calls; snprintf(s_last_body,sizeof(s_last_body),"%s",c->body);
    s_time_offset_us+=s_header_delay_us;
    if(s_mock_error)return -1;
    cJSON *request=cJSON_Parse(c->body); const cJSON *challenge=get(request,"challenge");
    cJSON *reply=cJSON_CreateObject();
    if(challenge)cJSON_AddStringToObject(reply,"challenge",s_wrong_challenge?"wrong":challenge->valuestring);
    char *body=cJSON_PrintUnformatted(reply);
    snprintf(c->response,sizeof(c->response),"%s",body);c->response_length=strlen(body);
    cJSON_free(body);cJSON_Delete(reply);cJSON_Delete(request);
    esp_http_client_event_t event={.event_id=0,.user_data=c->config.user_data,.client=c};
    c->config.event_handler(&event);
    return s_oversized_body?WEBHOOK_REPLY_MAX+1:s_endless_body?0:(int64_t)c->response_length;
}
int esp_http_client_read(esp_http_client_handle_t c,char *out,int length)
{
    assert(length==1);++s_reads;s_time_offset_us+=s_read_delay_us;
    if(s_endless_body){*out='x';return 1;}
    if(s_truncated_body&&c->read+1>=c->response_length)return 0;
    if(c->read>=c->response_length)return 0;
    *out=c->response[c->read++];return 1;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c)
{ return !s_endless_body&&c->read>=c->response_length; }
int esp_http_client_close(esp_http_client_handle_t c) { (void)c;return ESP_OK; }
int esp_http_client_get_status_code(esp_http_client_handle_t c) { (void)c; return s_mock_status; }
void esp_http_client_cleanup(esp_http_client_handle_t c) { free(c); }

cJSON *dot_controller_status(void) { return cJSON_Parse("{\"device_id\":\"esp32-test\"}"); }
esp_err_t dot_controller_accept_reply(const char *id,const char *text,bool *duplicate)
{ (void)text; ++s_reply_calls; *duplicate=false; return !strcmp(id,"pending")?ESP_OK:ESP_ERR_INVALID_STATE; }
esp_err_t dot_controller_submit_text(const char *text,const char *kind) { (void)text;(void)kind; return 0; }
void dot_controller_cancel(void) { s_pending=false; }
bool dot_controller_is_pending(const char *id) { (void)id; return s_pending; }
bool dot_clock_fresh(void) { return s_clock_fresh; }
bool dot_audio_muted(void) { return s_actual_muted; }

static cJSON *invoke(const char *json,bool auth)
{
    cJSON *r=cJSON_Parse(json); assert(r);
    cJSON *params=cJSON_GetObjectItemCaseSensitive(r,"params");
    if(!params)params=cJSON_AddObjectToObject(r,"params");
    if(!get(params,"_meta")) {
        cJSON *meta=cJSON_AddObjectToObject(params,"_meta");
        cJSON_AddStringToObject(meta,"io.modelcontextprotocol/protocolVersion",PROTOCOL_VERSION);
        cJSON_AddObjectToObject(meta,"io.modelcontextprotocol/clientCapabilities");
    }
    cJSON *answer=dot_mcp_dispatch(r,auth);
    if(answer&&get(answer,"result")) {
        assert(strval(get(get(answer,"result"),"resultType"),8,8));
        assert(!strcmp(get(get(answer,"result"),"resultType")->valuestring,"complete"));
        assert(get(get(get(answer,"result"),"_meta"),"io.modelcontextprotocol/serverInfo"));
        const char *method=get(r,"method")->valuestring,*request_schema=NULL,*response_schema=NULL;
        if(!strcmp(method,"server/discover")){request_schema="DiscoverRequest";response_schema="DiscoverResultResponse";}
        if(!strcmp(method,"tools/list")){request_schema="ListToolsRequest";response_schema="ListToolsResultResponse";}
        if(!strcmp(method,"tools/call")){request_schema="CallToolRequest";response_schema="CallToolResultResponse";}
        if(request_schema) {
            cJSON *fixture=cJSON_CreateObject();cJSON_AddStringToObject(fixture,"schema",request_schema);
            cJSON_AddItemToObject(fixture,"value",cJSON_Duplicate(r,true));cJSON_AddItemToArray(s_fixtures,fixture);
            fixture=cJSON_CreateObject();cJSON_AddStringToObject(fixture,"schema",response_schema);
            cJSON_AddItemToObject(fixture,"value",cJSON_Duplicate(answer,true));cJSON_AddItemToArray(s_fixtures,fixture);
        }
    }
    cJSON_Delete(r); return answer;
}
static void expect_error(const char *json,bool auth,int code)
{ cJSON *r=invoke(json,auth);assert(r);assert(get(get(r,"error"),"code")); assert(get(get(r,"error"),"code")->valueint==code);cJSON_Delete(r); }
static const char *secret1="whsec_AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";
static const char *secret2="whsec_AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHwA=";
static cJSON *subscribe(const char *secret,const char *extra,const char *url)
{
    char request[2048];
    snprintf(request,sizeof(request),"{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"events/subscribe\",\"params\":{\"name\":\"device.utterance\",\"arguments\":{\"device_id\":\"esp32-test\"},\"delivery\":{\"mode\":\"webhook\",\"url\":\"%s\",\"secret\":\"%s\"}%s}}",url,secret,extra);
    return invoke(request,true);
}
static void no_error(cJSON *r) { assert(r); if(get(r,"error")){ char *s=cJSON_Print(r);fprintf(stderr,"Unexpected error: %s\n",s);cJSON_free(s);} assert(!get(r,"error"));cJSON_Delete(r); }

int main(int argc,char **argv)
{
    s_fixtures=cJSON_CreateArray();assert(s_fixtures);
    assert(dot_preferences_init()==ESP_OK);
    dot_config_t config={0}; strcpy(config.device_id,"esp32-test");strcpy(config.mcp_bearer,"unit-test-owner");
    assert(dot_mcp_init(&config)==ESP_OK);
    cJSON *missing_meta=cJSON_Parse("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"server/discover\",\"params\":{}}");
    cJSON *missing_reply=dot_mcp_dispatch(missing_meta,true);assert(get(get(missing_reply,"error"),"code")->valueint==-32602);cJSON_Delete(missing_reply);cJSON_Delete(missing_meta);
    expect_error("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"server/discover\",\"params\":{\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"wrong\",\"io.modelcontextprotocol/clientCapabilities\":{}}}}",true,-32022);
    char sig[49]; assert(signature(secret1,"evt_test","1760000000","{\"hello\":\"world\"}",sig));
    assert(!strcmp(sig,"v1,+cRtzrsu3Tm1qG5jUwnB6JKIw4Ivv/aP0XcMqKUbLEc="));
    expect_error("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"device_status\"}",false,-32001);
    expect_error("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"unknown\"}",true,-32601);
    expect_error("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"method\":\"tools/call\"}",true,-32600);
    expect_error("{\"jsonrpc\":\"2.0\",\"id\":1.2,\"method\":\"ping\"}",true,-32600);
    expect_error("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"reply_to_device\",\"arguments\":{\"request_id\":\"pending\",\"text\":\"hello\",\"extra\":true}}}",true,-32602);
    assert(!invoke("{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"params\":{\"name\":\"reply_to_device\",\"arguments\":{\"request_id\":\"pending\",\"text\":\"hello\"}}}",true));
    assert(!s_reply_calls);
    no_error(invoke("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"server/discover\"}",true));
    no_error(invoke("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}",true));
    no_error(invoke("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"get_device_preferences\",\"arguments\":{}}}",true));
    no_error(invoke("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"update_device_preferences\",\"arguments\":{\"volume_percent\":55}}}",true));
    s_actual_muted=true;
    cJSON *privacy=invoke("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"get_device_preferences\",\"arguments\":{}}}",true);
    const cJSON *privacy_data=get(get(privacy,"result"),"structuredContent");
    assert(cJSON_IsTrue(get(privacy_data,"microphone_muted"))&&cJSON_IsFalse(get(privacy_data,"stored_microphone_muted")));cJSON_Delete(privacy);
    privacy=invoke("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"update_device_preferences\",\"arguments\":{\"volume_percent\":50}}}",true);
    privacy_data=get(get(privacy,"result"),"structuredContent");
    assert(cJSON_IsTrue(get(privacy_data,"microphone_muted"))&&cJSON_IsFalse(get(privacy_data,"stored_microphone_muted"))&&s_actual_muted);cJSON_Delete(privacy);
    s_actual_muted=false;
    expect_error("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"update_device_preferences\",\"arguments\":{\"volume_percent\":101}}}",true,-32602);
    expect_error("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"update_device_preferences\",\"arguments\":{\"microphone_muted\":false}}}",true,-32602);
    no_error(invoke("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"events/list\"}",true));
    no_error(invoke("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"reply_to_device\",\"arguments\":{\"request_id\":\"pending\",\"text\":\"hello\"}}}",true));
    assert(s_reply_calls==1);
    assert(!dot_mcp_subscription_ready());
    s_wrong_challenge=true; cJSON *r=subscribe(secret1,"","https://receiver.example.com/callback");
    assert(get(get(r,"error"),"code")->valueint==-32015);cJSON_Delete(r);assert(!dot_mcp_subscription_ready());
    s_wrong_challenge=false;s_private_dns=true;r=subscribe(secret1,"","https://receiver.example.com/callback");
    assert(get(r,"error"));cJSON_Delete(r);assert(!dot_mcp_subscription_ready());s_private_dns=false;
    s_nvs_failure=1;r=subscribe(secret1,"","https://receiver.example.com/callback");
    assert(get(r,"error"));cJSON_Delete(r);assert(!dot_mcp_subscription_ready());s_nvs_failure=0;
    expect_error("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"events/subscribe\",\"params\":{\"name\":\"device.utterance\",\"arguments\":{\"device_id\":\"someone-elses-device\"},\"delivery\":{\"mode\":\"webhook\",\"url\":\"https://receiver.example.com/callback\",\"secret\":\"whsec_AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=\"}}}",true,-32602);
    s_clock_fresh=false;r=subscribe(secret1,"","https://receiver.example.com/callback");
    assert(get(r,"error"));cJSON_Delete(r);assert(!dot_mcp_subscription_ready());s_clock_fresh=true;
    no_error(subscribe(secret1,",\"ttlMs\":2000","https://receiver.example.com/callback"));
    assert(dot_mcp_subscription_ready());assert(s_sub.expires_ms-now_ms()<=2000);
    webhook_response_t response;int status;
    s_partial_write_max=3;assert(post_signed(&s_sub,"evt_partial","{\"data\":\"exact bytes\"}",&response,&status)==ESP_OK);
    assert(!strcmp(s_last_body,"{\"data\":\"exact bytes\"}"));s_partial_write_max=0;
    s_oversized_body=true;s_reads=0;assert(post_signed(&s_sub,"evt_large","{}",&response,&status)==ESP_ERR_INVALID_SIZE);assert(!s_reads);s_oversized_body=false;
    s_endless_body=true;s_reads=0;assert(post_signed(&s_sub,"evt_endless","{}",&response,&status)==ESP_ERR_INVALID_SIZE);assert(s_reads==WEBHOOK_REPLY_MAX+1);s_endless_body=false;
    s_truncated_body=true;assert(post_signed(&s_sub,"evt_short","{}",&response,&status)!=ESP_OK);s_truncated_body=false;
    s_header_delay_us=WEBHOOK_TIMEOUT_MS*INT64_C(1000)+1;assert(post_signed(&s_sub,"evt_headers_timeout","{}",&response,&status)==ESP_ERR_TIMEOUT);s_header_delay_us=0;s_time_offset_us=0;
    s_endless_body=true;s_read_delay_us=INT64_C(2000000);s_reads=0;assert(post_signed(&s_sub,"evt_body_timeout","{}",&response,&status)==ESP_ERR_TIMEOUT);assert(s_reads<=5);s_endless_body=false;s_read_delay_us=0;s_time_offset_us=0;
    no_error(subscribe(secret1,",\"ttlMs\":1","https://receiver.example.com/callback"));assert(s_sub.expires_ms-now_ms()<=1000);
    unsigned calls=s_calls; no_error(subscribe(secret1,"","https://receiver.example.com/callback"));assert(s_calls==calls);
    char identity[69];strcpy(identity,s_sub.id);
    no_error(subscribe(secret2,"","https://receiver.example.com/callback"));assert(!strcmp(identity,s_sub.id));
    assert(!strcmp(s_sub.previous_secret,secret1));assert(strchr(s_last_signature,' '));
    assert(dot_mcp_publish_utterance("pending","hello","text")==ESP_OK);
    assert(!strncmp(s_last_id,"evt_",4));assert(strstr(s_last_body,"\"text\":\"hello\""));
    assert(strchr(s_last_signature,' '));
    char signature_check[49]; assert(signature(secret2,s_last_id,s_last_timestamp,s_last_body,signature_check));
    assert(!strncmp(s_last_signature,signature_check,strlen(signature_check)));
    s_sub.previous_secret_until=now_ms()/1000-1;
    assert(dot_mcp_publish_utterance("pending","hello","text")==ESP_OK);assert(!strchr(s_last_signature,' '));
    char stable_id[96];strcpy(stable_id,s_last_id);
    s_mock_status=503; calls=s_calls;assert(dot_mcp_publish_utterance("pending","hello","text")!=ESP_OK);
    assert(s_calls==calls+3);assert(!strcmp(stable_id,s_last_id));s_mock_status=200;
    s_sub.expires_ms=now_ms()-1; assert(!dot_mcp_subscription_ready());
    calls=s_calls;assert(dot_mcp_publish_utterance("pending","hello","text")!=ESP_OK);assert(s_calls==calls);
    assert(dot_mcp_init(&config)==ESP_OK);assert(dot_mcp_subscription_ready());
    s_clock_fresh=false;calls=s_calls;assert(dot_mcp_publish_utterance("pending","hello","text")!=ESP_OK);assert(s_calls==calls);s_clock_fresh=true;
    strcpy(config.mcp_bearer,"rotated-owner");assert(dot_mcp_init(&config)==ESP_OK);assert(!dot_mcp_subscription_ready());
    strcpy(config.mcp_bearer,"unit-test-owner");assert(dot_mcp_init(&config)==ESP_OK);assert(dot_mcp_subscription_ready());
    no_error(invoke("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"events/unsubscribe\",\"params\":{\"name\":\"device.utterance\",\"arguments\":{\"device_id\":\"esp32-test\"},\"delivery\":{\"mode\":\"webhook\",\"url\":\"https://receiver.example.com/callback\"}}}",true));
    assert(!dot_mcp_subscription_ready());assert(dot_mcp_init(&config)==ESP_OK);assert(!dot_mcp_subscription_ready());
    config.mcp_bearer[0]=0;strcpy(config.tunnel_id,"tnl_personal-test");config.tunnel_scope_auth=false;
    assert(dot_mcp_init(&config)==ESP_OK);
    expect_error("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}",true,-32001);
    config.tunnel_scope_auth=true;assert(dot_mcp_init(&config)==ESP_OK);
    no_error(subscribe(secret1,"","https://receiver.example.com/callback"));
    assert(dot_mcp_subscription_ready());assert(dot_mcp_init(&config)==ESP_OK);assert(dot_mcp_subscription_ready());
    char scoped_owner[65];strcpy(scoped_owner,s_owner);strcpy(config.mcp_bearer,"stale-bearer-from-previous-mode");
    assert(dot_mcp_init(&config)==ESP_OK);assert(!strcmp(scoped_owner,s_owner));assert(dot_mcp_subscription_ready());
    strcpy(config.tunnel_id,"tnl_another-personal-test");assert(dot_mcp_init(&config)==ESP_OK);assert(!dot_mcp_subscription_ready());
    strcpy(config.tunnel_id,"tnl_personal-test");config.tunnel_scope_auth=false;assert(dot_mcp_init(&config)==ESP_OK);assert(!dot_mcp_subscription_ready());
    const char *bad[]={"http://receiver.example.com/callback","https://receiver.example.com:444/callback","https://user@receiver.example.com/callback","https://receiver.example.com/#secret","https://receiver.example.com\\evil/callback"};
    destination_t dest;for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i)assert(!parse_callback(bad[i],&dest));
    assert(!public_ipv4(ntohl(0x7f000001)));assert(!public_ipv4(ntohl(0x0a000001)));assert(!public_ipv4(ntohl(0xa9fe0101)));assert(!public_ipv4(ntohl(0xc0a80101)));assert(public_ipv4(ntohl(0x5db8d822)));
    if(argc>1) {
        char *fixtures=cJSON_Print(s_fixtures);assert(fixtures);FILE *file=fopen(argv[1],"wb");assert(file);
        assert(fwrite(fixtures,1,strlen(fixtures),file)==strlen(fixtures));assert(!fclose(file));cJSON_free(fixtures);
    }
    cJSON_Delete(s_fixtures);
    puts("MCP tests passed: JSON-RPC, auth, filters, verification, pinned TLS, HMAC, TTL, refresh, key rotation, retries, stable IDs, persistence, revocation, unsubscribe, URL/IP rejection.");
    return 0;
}
