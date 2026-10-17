/* Actual production controller, real Windows locks/threads, mocked peripheral
 * calls. Hardware/UI entry points alone are excluded from this host build. */
#define DOT_CONTROLLER_HOST_TEST
#include <assert.h>
#include <stdio.h>
#include "../main/app_main.c"

static job_t s_queue[2];
static unsigned s_queue_count, s_random_counter, s_audio_token;
static LONG s_begin_calls, s_speak_calls, s_transcribe_calls, s_audio_actions, s_publish_calls;
static bool s_fake_recording, s_probe_reserved, s_cancel_in_speak, s_cancel_in_publish;
static bool s_block_cancel, s_cancel_during_end;
static HANDLE s_cancel_entered, s_cancel_continue, s_cancel_attempted, s_cancel_thread;
static unsigned s_lock_cancel_at, s_controller_locks;
static dot_preferences_t s_mock_preferences;
static LONG s_classifier_calls;
static dot_intent_kind_t s_classifier_kind = DOT_INTENT_ADDRESSED;
static bool s_classifier_valid = true, s_cancel_in_classifier, s_seen_followup;
static esp_err_t s_classifier_error = ESP_OK;
static char s_seen_context[DOT_INTENT_CONTEXT_MAX + 1], s_published_text[DOT_TEXT_MAX + 1];
static int64_t s_mock_time = -1;
static bool s_mock_muted, s_preference_write_failed;

size_t strlcpy(char *dest,const char *source,size_t cap)
{size_t n=strlen(source);if(cap){size_t m=n<cap-1?n:cap-1;memcpy(dest,source,m);dest[m]=0;}return n;}
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{SRWLOCK *p=malloc(sizeof(*p));assert(p);InitializeSRWLock(p);return p;}
int xSemaphoreTake(SemaphoreHandle_t h,unsigned t)
{
    (void)t;
    if(h==s_lock&&s_lock_cancel_at&&++s_controller_locks==s_lock_cancel_at){s_lock_cancel_at=0;dot_controller_cancel();}
    AcquireSRWLockExclusive(h);return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t h){ReleaseSRWLockExclusive(h);return pdTRUE;}
int xQueueSend(QueueHandle_t q,const void *item,unsigned t)
{(void)q;(void)t;if(s_queue_count==2)return 0;memcpy(&s_queue[s_queue_count++],item,sizeof(job_t));return pdTRUE;}
static job_t pop_job(void)
{assert(s_queue_count);job_t j=s_queue[0];memmove(s_queue,s_queue+1,(--s_queue_count)*sizeof(job_t));return j;}
void esp_fill_random(void *p,size_t n){unsigned char *q=p;while(n--)*q++=(unsigned char)++s_random_counter;}
const char *esp_err_to_name(int e){return e==ESP_OK?"ESP_OK":"mock_error";}
unsigned esp_get_free_heap_size(void){return 1000000;}
int64_t esp_timer_get_time(void){return s_mock_time >= 0 ? s_mock_time : (int64_t)GetTickCount64()*1000;}
const mbedtls_md_info_t *mbedtls_md_info_from_type(int t)
{static const mbedtls_md_info_t info={0};(void)t;return &info;}
int mbedtls_md(const mbedtls_md_info_t *i,const unsigned char *p,size_t n,unsigned char *out)
{(void)i;uint32_t h=2166136261;while(n--)h=(h^*p++)*16777619;for(unsigned k=0;k<32;k++)out[k]=(unsigned char)(h>>(k%4*8));return 0;}
bool dot_wifi_connected(void){return true;}
int dot_wifi_rssi(void){return -45;}
bool dot_clock_fresh(void){return true;}
bool dot_tunnel_connected(void){return true;}
int dot_tunnel_last_status(void){return 200;}
bool dot_mcp_subscription_ready(void){return true;}
bool dot_config_audio_ready(const dot_config_t *c){return c&&c->audio_api_key[0];}
esp_err_t dot_preferences_get(dot_preferences_t *out)
{ if(!out)return ESP_ERR_INVALID_ARG;*out=s_mock_preferences;return ESP_OK; }
esp_err_t dot_preferences_update(const cJSON *patch,bool physical)
{
    if(s_preference_write_failed)return ESP_FAIL;
    bool changed=false;
    const cJSON *mute=cJSON_GetObjectItemCaseSensitive(patch,"microphone_muted");
    if(mute){assert(physical&&cJSON_IsBool(mute));bool value=cJSON_IsTrue(mute);changed=value!=s_mock_preferences.microphone_muted;s_mock_preferences.microphone_muted=value;}
    const cJSON *volume=cJSON_GetObjectItemCaseSensitive(patch,"volume_percent");
    if(volume){assert(cJSON_IsNumber(volume));changed=changed||volume->valueint!=s_mock_preferences.volume_percent;s_mock_preferences.volume_percent=(uint8_t)volume->valueint;}
    if(changed)++s_mock_preferences.revision;
    return ESP_OK;
}
void dot_audio_set_muted(bool muted) { s_mock_muted=muted; }
bool dot_audio_muted(void) { return s_mock_muted; }
esp_err_t dot_intent_classify(const dot_config_t *config,const char *transcript,const char *context,
    bool followup,uint32_t token,dot_intent_decision_t *decision)
{
    assert(config&&transcript&&decision);(void)token;InterlockedIncrement(&s_classifier_calls);
    s_seen_followup=followup;strlcpy(s_seen_context,context?context:"",sizeof(s_seen_context));
    memset(decision,0,sizeof(*decision));decision->kind=s_classifier_kind;decision->valid=s_classifier_valid;
    decision->addressed=s_classifier_kind==DOT_INTENT_ADDRESSED;
    decision->ambient=s_classifier_kind==DOT_INTENT_AMBIENT;decision->uncertain=s_classifier_kind==DOT_INTENT_UNCERTAIN;
    // Deliberately return a valid classification after cancellation. The
    // controller must independently recheck its generation before publishing.
    if(s_cancel_in_classifier){s_cancel_in_classifier=false;dot_controller_cancel();}
    return s_classifier_error;
}
bool dot_intent_should_accept(const dot_intent_decision_t *d,bool followup)
{ return d&&d->valid&&(d->kind==DOT_INTENT_ADDRESSED||(d->kind==DOT_INTENT_UNCERTAIN&&followup)); }
esp_err_t dot_mcp_publish_utterance(const char *id,const char *text,const char *kind)
{assert(id&&text&&kind);strlcpy(s_published_text,text,sizeof(s_published_text));InterlockedIncrement(&s_publish_calls);if(s_cancel_in_publish){s_cancel_in_publish=false;dot_controller_cancel();return ESP_FAIL;}return ESP_OK;}
uint32_t dot_audio_cancel_token(void){return s_audio_token;}
void dot_audio_cancel_playback(void){++s_audio_token;}
esp_err_t dot_audio_record_begin(void)
{
    InterlockedIncrement(&s_begin_calls);
    if(s_probe_reserved){assert(s_capturing&&!s_pending);assert(dot_controller_submit_text("racing text","serial_test")==ESP_ERR_INVALID_STATE);}
    s_fake_recording=true;return ESP_OK;
}
static DWORD WINAPI cancel_thread(void *p)
{(void)p;if(s_cancel_attempted)SetEvent(s_cancel_attempted);dot_controller_cancel();return 0;}
static DWORD WINAPI capture_thread(void *p){(void)p;record_begin();return 0;}
esp_err_t dot_audio_record_end(uint8_t **wav,size_t *n)
{
    assert(s_fake_recording);s_fake_recording=false;*wav=calloc(1,44);assert(*wav);*n=44;
    if(s_cancel_during_end){s_cancel_during_end=false;s_cancel_thread=CreateThread(NULL,0,cancel_thread,NULL,0,NULL);assert(s_cancel_thread);assert(WaitForSingleObject(s_cancel_attempted,2000)==WAIT_OBJECT_0);}
    return ESP_OK;
}
void dot_audio_record_cancel(void)
{
    if(s_block_cancel){SetEvent(s_cancel_entered);assert(WaitForSingleObject(s_cancel_continue,2000)==WAIT_OBJECT_0);}
    s_fake_recording=false;
}
esp_err_t dot_openai_speak_guarded(const dot_config_t *c,const char *text,uint32_t token)
{
    (void)c;assert(text);InterlockedIncrement(&s_speak_calls);
    if(s_cancel_in_speak){s_cancel_in_speak=false;dot_controller_cancel();}
    if(token!=s_audio_token)return ESP_ERR_INVALID_STATE;
    InterlockedIncrement(&s_audio_actions);return ESP_OK;
}
esp_err_t dot_openai_transcribe_guarded(const dot_config_t *c,const uint8_t *wav,size_t n,char *text,size_t cap,uint32_t token)
{(void)c;assert(wav&&n);InterlockedIncrement(&s_transcribe_calls);if(token!=s_audio_token)return ESP_ERR_INVALID_STATE;strlcpy(text,"captured speech",cap);return ESP_OK;}

static void reset(void)
{
    assert(!s_block_cancel);dot_controller_cancel();
    while(s_queue_count){job_t j=pop_job();free(j.wav);}
    memset(s_done,0,sizeof(s_done));s_done_next=0;
    s_begin_calls=s_speak_calls=s_transcribe_calls=s_audio_actions=s_publish_calls=0;
    s_probe_reserved=s_cancel_in_speak=s_cancel_in_publish=s_cancel_during_end=false;
    s_controller_locks=s_lock_cancel_at=0;
    s_classifier_calls=0;s_classifier_kind=DOT_INTENT_ADDRESSED;s_classifier_valid=true;s_classifier_error=ESP_OK;
    s_cancel_in_classifier=s_seen_followup=false;s_seen_context[0]=s_published_text[0]=0;s_mock_time=-1;
}
static job_t new_speak_job(void)
{
    assert(dot_controller_submit_text("test utterance","serial_test")==ESP_OK);
    job_t j=pop_job();j.kind=JOB_SPEAK;strcpy(j.text,"test reply");return j;
}

static job_t new_auto_job(void)
{
    job_t j={.kind=JOB_TRANSCRIBE,.automatic=true};strcpy(j.input_kind,"speech");
    j.wav=calloc(1,44);assert(j.wav);j.wav_size=44;
    assert(begin_request(&j,false,0)==ESP_OK);
    return pop_job();
}

static void test_automatic_gating(void)
{
    reset();job_t j=new_auto_job();process_job(&j);
    assert(!j.wav&&s_transcribe_calls==1&&s_classifier_calls==1&&s_publish_calls==1);
    assert(s_pending&&!s_seen_followup&&!strcmp(s_published_text,"captured speech"));
    assert(!strcmp(s_context,"captured speech")&&!strcmp(s_state,"waiting_for_dot"));

    reset();s_classifier_kind=DOT_INTENT_AMBIENT;j=new_auto_job();process_job(&j);
    assert(s_classifier_calls==1&&!s_publish_calls&&!s_pending&&!strcmp(s_state,"ambient_ignored"));
    reset();s_classifier_kind=DOT_INTENT_UNCERTAIN;j=new_auto_job();process_job(&j);
    assert(!s_seen_followup&&!s_publish_calls&&!s_pending&&!strcmp(s_state,"ambient_ignored"));

    reset();s_mock_time=1000000;s_followup_until=s_mock_time+45000000;
    strcpy(s_context,"previous accepted utterance");s_classifier_kind=DOT_INTENT_UNCERTAIN;
    j=new_auto_job();process_job(&j);
    assert(s_seen_followup&&s_publish_calls==1&&s_pending&&!strcmp(s_seen_context,"previous accepted utterance"));
    reset();s_mock_time=1000000;s_followup_until=s_mock_time+45000000;
    s_classifier_kind=DOT_INTENT_AMBIENT;j=new_auto_job();process_job(&j);
    assert(s_seen_followup&&!s_publish_calls&&!s_pending); // Ambient stays blocked during follow-up.
    reset();s_mock_time=1000000;s_followup_until=s_mock_time-1;
    s_classifier_kind=DOT_INTENT_UNCERTAIN;j=new_auto_job();process_job(&j);
    assert(!s_seen_followup&&!s_publish_calls&&!s_pending);

    reset();s_classifier_valid=false;j=new_auto_job();process_job(&j);
    assert(!s_publish_calls&&!s_pending);
    reset();s_classifier_error=ESP_FAIL;j=new_auto_job();process_job(&j);
    assert(!s_publish_calls&&!s_pending&&!strcmp(s_state,"error"));
    reset();s_cancel_in_classifier=true;strcpy(s_context,"old conversation");j=new_auto_job();process_job(&j);
    assert(s_classifier_calls==1&&!s_publish_calls&&!s_pending&&!s_followup_until&&!s_context[0]);
    assert(!strcmp(s_state,"cancelled"));

    // Explicit button capture bypasses semantic filtering.
    reset();s_classifier_kind=DOT_INTENT_AMBIENT;j=new_auto_job();j.automatic=false;process_job(&j);
    assert(s_transcribe_calls==1&&!s_classifier_calls&&s_publish_calls==1);

    // A completed reply opens a bounded follow-up window while retaining the
    // accepted utterance as routing context. Cancel clears both atomically.
    reset();s_mock_time=1000000;
    assert(dot_controller_submit_text("What should we do next?","serial_test")==ESP_OK);
    j=pop_job();process_job(&j);assert(!strcmp(s_context,"What should we do next?"));
    bool duplicate=false;assert(dot_controller_accept_reply(j.request_id,"Shall we make some tea?",&duplicate)==ESP_OK&&!duplicate);
    job_t reply=pop_job();process_job(&reply);
    assert(!s_pending&&s_followup_until==s_mock_time+45000000&&!strcmp(s_context,"What should we do next?"));
    s_classifier_kind=DOT_INTENT_UNCERTAIN;s_mock_time+=1000000;
    j=new_auto_job();process_job(&j);assert(s_seen_followup&&s_publish_calls==2);
    assert(!strcmp(s_seen_context,"What should we do next?"));
    dot_controller_cancel();assert(!s_followup_until&&!s_context[0]);
}

static void test_context_utf8_boundary(void)
{
    reset();
    char text[DOT_INTENT_CONTEXT_MAX+16];
    memset(text,'a',sizeof(text));text[sizeof(text)-1]=0;
    lock();save_context_locked(text);unlock();
    assert(strlen(s_context)==DOT_INTENT_CONTEXT_MAX);
    static const unsigned char characters[][4]={{0xc3,0xa9,0,0},{0xe2,0x82,0xac,0},{0xf0,0x9f,0x98,0x80}};
    static const unsigned widths[]={2,3,4};
    for(unsigned kind=0;kind<3;++kind) {
        unsigned width=widths[kind];
        for(unsigned cut=1;cut<width;++cut) {
            memset(text,'a',sizeof(text));text[sizeof(text)-1]=0;
            size_t start=DOT_INTENT_CONTEXT_MAX-cut;
            memcpy(text+start,characters[kind],width);
            lock();save_context_locked(text);unlock();
            assert(strlen(s_context)==start);assert(!memcmp(s_context,text,start));
        }
        memset(text,'a',sizeof(text));text[sizeof(text)-1]=0;
        memcpy(text+DOT_INTENT_CONTEXT_MAX-width,characters[kind],width);
        lock();save_context_locked(text);unlock();
        assert(strlen(s_context)==DOT_INTENT_CONTEXT_MAX);assert(!memcmp(s_context,text,DOT_INTENT_CONTEXT_MAX));
    }
    lock();save_context_locked("");unlock();assert(!s_context[0]);
}

static void test_physical_mute_latch(void)
{
    reset();s_physical_mute_latch=false;s_mute_preferences_initialized=false;s_mock_muted=false;
    s_mock_preferences.microphone_muted=false;s_mock_preferences.revision=1;
    apply_preference_mute(false);assert(!dot_audio_muted());
    s_preference_write_failed=true;
    assert(apply_physical_mute(true)==ESP_FAIL);
    assert(dot_audio_muted()&&s_physical_mute_latch&&!s_mock_preferences.microphone_muted);
    cJSON *status=dot_controller_status();assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(status,"microphone_muted")));cJSON_Delete(status);
    s_preference_write_failed=false;
    cJSON *remote=cJSON_Parse("{\"volume_percent\":25}");assert(remote);
    unsigned old_revision=s_mock_preferences.revision;
    assert(dot_preferences_update(remote,false)==ESP_OK);cJSON_Delete(remote);
    assert(s_mock_preferences.revision>old_revision&&!s_mock_preferences.microphone_muted);
    apply_preference_mute(s_mock_preferences.microphone_muted);
    assert(dot_audio_muted()&&s_physical_mute_latch); // A remote revision cannot unmute.
    s_preference_write_failed=true;assert(apply_physical_mute(false)==ESP_FAIL);
    assert(dot_audio_muted()&&s_physical_mute_latch);
    s_preference_write_failed=false;
    // Stored false is already unchanged; explicit physical unmute must still
    // succeed, clear the live latch, and be the only operation that can do so.
    assert(apply_physical_mute(false)==ESP_OK);assert(!dot_audio_muted()&&!s_physical_mute_latch);
    apply_preference_mute(false);assert(!dot_audio_muted());
    assert(apply_physical_mute(true)==ESP_OK&&s_mock_preferences.microphone_muted&&dot_audio_muted());
    apply_preference_mute(true);assert(dot_audio_muted());
    assert(apply_physical_mute(false)==ESP_OK&&!dot_audio_muted());
}

int main(void)
{
    s_lock=xSemaphoreCreateMutex();s_capture_op_lock=xSemaphoreCreateMutex();s_jobs=(void *)1;
    s_audio_ready=true;strcpy(s_config.audio_api_key,"test-placeholder");
    s_mock_preferences.volume_percent=60;s_mock_preferences.hands_free=true;strcpy(s_mock_preferences.device_name,"Digimon");
    s_cancel_entered=CreateEvent(NULL,TRUE,FALSE,NULL);s_cancel_continue=CreateEvent(NULL,TRUE,FALSE,NULL);
    s_cancel_attempted=CreateEvent(NULL,TRUE,FALSE,NULL);assert(s_cancel_entered&&s_cancel_continue&&s_cancel_attempted);
    reset();s_probe_reserved=true;record_begin();assert(s_capturing&&s_fake_recording&&!strcmp(s_state,"recording"));
    uint32_t old_capture=s_generation;dot_controller_cancel();record_begin();uint32_t new_capture=s_generation;
    job_t stale={.kind=JOB_TRANSCRIBE};assert(begin_request(&stale,true,old_capture)==ESP_ERR_INVALID_STATE);
    assert(s_capturing&&s_generation==new_capture&&!strcmp(s_state,"recording"));

    // An older cancellation is paused inside the peripheral. A new capture
    // must wait until it finishes rather than being stopped by that cancel.
    s_probe_reserved=false;s_block_cancel=true;ResetEvent(s_cancel_entered);ResetEvent(s_cancel_continue);
    HANDLE cancelling=CreateThread(NULL,0,cancel_thread,NULL,0,NULL);assert(cancelling);
    assert(WaitForSingleObject(s_cancel_entered,2000)==WAIT_OBJECT_0);
    LONG before=s_begin_calls;HANDLE beginning=CreateThread(NULL,0,capture_thread,NULL,0,NULL);assert(beginning);
    assert(WaitForSingleObject(beginning,30)==WAIT_TIMEOUT);assert(s_begin_calls==before);
    SetEvent(s_cancel_continue);assert(WaitForSingleObject(cancelling,2000)==WAIT_OBJECT_0);
    assert(WaitForSingleObject(beginning,2000)==WAIT_OBJECT_0);CloseHandle(cancelling);CloseHandle(beginning);s_block_cancel=false;
    assert(s_capturing&&s_fake_recording&&s_begin_calls==before+1&&!strcmp(s_state,"recording"));

    // A cancel arriving while WAV copy is in progress must invalidate the
    // resulting queued job; it must not let record_end resurrect a request.
    ResetEvent(s_cancel_attempted);s_cancel_during_end=true;record_end();
    assert(WaitForSingleObject(s_cancel_thread,2000)==WAIT_OBJECT_0);CloseHandle(s_cancel_thread);
    assert(!s_pending&&!s_capturing&&!s_fake_recording&&!strcmp(s_state,"cancelled"));
    job_t cancelled_capture=pop_job();process_job(&cancelled_capture);assert(!s_transcribe_calls&&!s_publish_calls);

    reset();job_t reply=new_speak_job();s_lock_cancel_at=2;s_controller_locks=0;
    process_job(&reply);assert(!s_speak_calls&&!s_audio_actions&&!strcmp(s_state,"cancelled"));
    reset();reply=new_speak_job();s_cancel_in_speak=true;process_job(&reply);
    assert(s_speak_calls==1&&!s_audio_actions&&!s_pending&&!strcmp(s_state,"cancelled"));

    reset();assert(dot_controller_submit_text("test text","serial_test")==ESP_OK);job_t text=pop_job();
    s_lock_cancel_at=3;s_controller_locks=0;process_job(&text);
    assert(!s_publish_calls&&!strcmp(s_state,"cancelled"));
    reset();assert(dot_controller_submit_text("test text","serial_test")==ESP_OK);text=pop_job();
    s_cancel_in_publish=true;process_job(&text);assert(s_publish_calls==1&&!strcmp(s_state,"cancelled"));

    reset();reply=new_speak_job();process_job(&reply);assert(!s_pending&&s_audio_actions==1&&!strcmp(s_state,"spoken"));
    bool duplicate=false;assert(dot_controller_accept_reply(reply.request_id,reply.text,&duplicate)==ESP_OK&&duplicate);
    assert(dot_controller_accept_reply(reply.request_id,"different reply",&duplicate)==ESP_ERR_INVALID_STATE&&!duplicate);
    assert(dot_controller_submit_text("next request","serial_test")==ESP_OK);uint32_t next=s_generation;
    finish_job(&reply,ESP_FAIL,false);assert(s_pending&&s_generation==next&&!strcmp(s_state,"sending"));
    test_automatic_gating();
    test_context_utf8_boundary();test_physical_mute_latch();
    reset();puts("controller tests passed: capture/cancel races, stale jobs/state, guarded audio, addressed/ambient/uncertain gates, follow-up/context expiry and UTF-8 bounds, classifier cancellation, manual bypass, reply deduplication, physical mute latch across failed persistence and remote revisions");
    return 0;
}
