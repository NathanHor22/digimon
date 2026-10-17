/* Run the production audio/WAV/HTTP code with mocked ESP-IDF peripherals.
 * No credentials, network, serial device, or external service are used. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/dot_audio.c"

struct host_audio_client { esp_http_client_config_t options; const char *body; int length; };
static struct host_audio_client s_client;
static uint8_t s_stream[PCM_BUFFER_BYTES], s_speaker_bytes[4096], s_request[500000];
static size_t s_stream_length, s_speaker_length, s_request_length;
static const uint8_t *s_response;
static size_t s_response_length, s_response_offset;
static const char *s_content_type;
static int s_status, s_connecting, s_header_again, s_read_again;
static bool s_cancel_header;
static int64_t s_time, s_time_step = 1000;
static unsigned s_open_count;
static unsigned s_client_count;
bool dot_clock_bootstrap_ready(void) { return true; }
void dot_clock_observe_http_date(const char *value) { (void)value; }

void *heap_caps_malloc(size_t n, unsigned f) { (void)f; return malloc(n); }
void *heap_caps_calloc(size_t n, size_t m, unsigned f) { (void)f; return calloc(n,m); }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t h, unsigned t) { (void)h; (void)t; return 1; }
int xSemaphoreGive(SemaphoreHandle_t h) { (void)h; return 1; }
StreamBufferHandle_t xStreamBufferCreateStatic(size_t n,size_t t,uint8_t *b,StaticStreamBuffer_t *s)
{ (void)n; (void)t; (void)b; (void)s; return (void *)1; }
size_t xStreamBufferBytesAvailable(StreamBufferHandle_t h) { (void)h; return s_stream_length; }
size_t xStreamBufferSend(StreamBufferHandle_t h,const void *p,size_t n,unsigned t)
{ (void)h; (void)t; assert((n&1)==0); assert(s_stream_length+n<=sizeof(s_stream)); memcpy(s_stream+s_stream_length,p,n); s_stream_length+=n; return n; }
size_t xStreamBufferReceive(StreamBufferHandle_t h,void *p,size_t n,unsigned t)
{ (void)h; (void)t; if(n>s_stream_length)n=s_stream_length; memcpy(p,s_stream,n); memmove(s_stream,s_stream+n,s_stream_length-n); s_stream_length-=n; return n; }
int xStreamBufferReset(StreamBufferHandle_t h) { (void)h; s_stream_length=0; return 1; }
void vTaskDelay(unsigned t) { s_time+=(int64_t)t*1000; }
unsigned ulTaskNotifyTake(int a,unsigned b) { (void)a; (void)b; return 1; }
int xTaskNotifyGive(TaskHandle_t h) { (void)h; return 1; }
int xTaskCreatePinnedToCore(void (*f)(void *),const char *n,unsigned s,void *a,unsigned p,TaskHandle_t *h,unsigned c)
{ (void)f;(void)n;(void)s;(void)a;(void)p;(void)c; if(h)*h=(void *)1; return pdPASS; }
uint32_t esp_random(void) { return 0x12345678; }
int64_t esp_timer_get_time(void) { s_time+=s_time_step; return s_time; }
void esp_log_level_set(const char *t,int l) { (void)t;(void)l; }
int esp_crt_bundle_attach(void *x) { (void)x; return 0; }
int i2s_new_channel(const i2s_chan_config_t *c,i2s_chan_handle_t *t,i2s_chan_handle_t *r)
{(void)c; if(t)*t=(void *)1;if(r)*r=(void *)1;return ESP_OK;}
int i2s_channel_init_std_mode(i2s_chan_handle_t h,const i2s_std_config_t *c){(void)h;(void)c;return ESP_OK;}
int i2s_channel_enable(i2s_chan_handle_t h){(void)h;return ESP_OK;}
int i2s_channel_disable(i2s_chan_handle_t h){(void)h;return ESP_OK;}
int i2s_channel_read(i2s_chan_handle_t h,void *p,size_t n,size_t *r,unsigned t)
{(void)h;(void)p;(void)n;(void)t;*r=0;return ESP_FAIL;}
int i2s_channel_write(i2s_chan_handle_t h,const void *p,size_t n,size_t *w,unsigned t)
{(void)h;(void)t; if(n>4)n=4; assert(s_speaker_length+n<=sizeof(s_speaker_bytes));memcpy(s_speaker_bytes+s_speaker_length,p,n);s_speaker_length+=n;*w=n;return ESP_OK;}
int i2s_channel_preload_data(i2s_chan_handle_t h,const void *p,size_t n,size_t *w)
{(void)h;(void)p;*w=n;return ESP_OK;}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *o)
{ s_client_count++;s_client.options=*o;s_client.body=NULL;s_client.length=0; assert(o->crt_bundle_attach==esp_crt_bundle_attach);assert(o->is_async);assert(o->disable_auto_redirect);assert(o->timeout_ms<=1000);return &s_client; }
int esp_http_client_set_header(esp_http_client_handle_t h,const char *k,const char *v)
{(void)h;assert(k&&v);return ESP_OK;}
int esp_http_client_set_post_field(esp_http_client_handle_t h,const char *p,int n){h->body=p;h->length=n;return ESP_OK;}
int esp_http_client_get_post_field(esp_http_client_handle_t h,char **p){*p=(char *)h->body;return h->length;}
int esp_http_client_get_status_code(esp_http_client_handle_t h){(void)h;return s_status;}
int esp_http_client_open(esp_http_client_handle_t h,int n)
{(void)h;assert(n>0);s_open_count++;if(s_connecting-- >0)return ESP_ERR_HTTP_CONNECTING;return ESP_OK;}
int esp_http_client_write(esp_http_client_handle_t h,const char *p,int n)
{(void)h; if(n>37)n=37;assert(s_request_length+n<sizeof(s_request));memcpy(s_request+s_request_length,p,n);s_request_length+=n;s_request[s_request_length]=0;return n;}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h)
{
    if(s_cancel_header){dot_audio_cancel_playback();return -ESP_ERR_HTTP_EAGAIN;}
    if(s_header_again-- >0)return -ESP_ERR_HTTP_EAGAIN;
    esp_http_client_event_t e={.event_id=HTTP_EVENT_ON_HEADER,.user_data=h->options.user_data,
        .header_key="Content-Type",.header_value=(char *)s_content_type,.client=h};
    h->options.event_handler(&e);return s_response_length;
}
int esp_http_client_read(esp_http_client_handle_t h,char *p,int n)
{
    (void)p;(void)n;
    if(s_read_again-- >0)return -ESP_ERR_HTTP_EAGAIN;
    // Deliberately split PCM and JSON at odd TLS boundaries.
    size_t count=(s_response_offset%7)+1;
    if(count>s_response_length-s_response_offset)count=s_response_length-s_response_offset;
    esp_http_client_event_t e={.event_id=HTTP_EVENT_ON_DATA,.user_data=h->options.user_data,
        .data=(char *)s_response+s_response_offset,.data_len=(int)count,.client=h};
    h->options.event_handler(&e);s_response_offset+=count;return (int)count;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t h)
{(void)h;return s_response_offset==s_response_length;}
void esp_http_client_cleanup(esp_http_client_handle_t h){(void)h;}

static void reset_http(const void *data,size_t length,const char *type)
{
    s_response=data;s_response_length=length;s_response_offset=0;s_content_type=type;
    s_status=200;s_connecting=2;s_header_again=2;s_read_again=2;s_cancel_header=false;
    s_request_length=s_stream_length=s_open_count=s_client_count=0;s_time=0;s_time_step=1000;
    atomic_store(&s_pcm_failed,false);atomic_store(&s_pcm_playing,true);
}

static void feed_frame(const int16_t *samples, size_t count) {
    capture_frame(samples, count, atomic_load(&s_capture_generation));
}

static void feed_repeated(const int16_t *samples, unsigned frames) {
    while (frames--) feed_frame(samples, MIC_FRAME);
}

static void test_handsfree(void) {
    int16_t quiet[MIC_FRAME], voice[MIC_FRAME];
    for (size_t i = 0; i < MIC_FRAME; ++i) { quiet[i] = 100; voice[i] = 2000; }
    atomic_store(&s_pcm_playing, false); atomic_store(&s_speaking, false);
    dot_audio_set_handsfree(true);
    feed_repeated(quiet, 20); feed_repeated(voice, 10); feed_repeated(quiet, 34);
    assert(!dot_audio_detected_ready()); feed_repeated(quiet, 1);
    assert(dot_audio_detected_ready() && !dot_audio_is_recording() && !dot_audio_record_full());
    size_t stopped = s_capture_samples; feed_repeated(voice, 20); assert(s_capture_samples == stopped);
    dot_audio_set_handsfree(true); dot_audio_set_muted(false); assert(dot_audio_detected_ready());
    uint8_t *wav = NULL; size_t bytes = 0;
    assert(dot_audio_take_detected_wav(&wav, &bytes) == ESP_OK);
    assert(bytes == 44 + (PREROLL_SAMPLES + 10 * MIC_FRAME + 35 * MIC_FRAME) * 2);
    int16_t sample = 0; memcpy(&sample, wav + 44, 2); assert(sample == 100);
    memcpy(&sample, wav + 44 + PREROLL_SAMPLES * 2, 2); assert(sample == 2000);
    assert(!dot_audio_detected_ready()); free(wav);
    feed_repeated(quiet, 20); feed_repeated(voice, 1); feed_repeated(quiet, 35);
    assert(!dot_audio_detected_ready() && !s_candidate); // Reject a 20 ms click.

    feed_repeated(quiet, 20); feed_repeated(voice, 760);
    assert(dot_audio_detected_ready() && s_capture_samples == RECORD_SAMPLES);
    assert(dot_audio_take_detected_wav(&wav, &bytes) == ESP_OK && bytes == 44 + RECORD_SAMPLES * 2); free(wav);

    feed_repeated(quiet, 20); feed_repeated(voice, 10); assert(s_candidate);
    assert(dot_audio_record_begin() == ESP_OK && !s_candidate && !s_preroll_count);
    feed_repeated(voice, 5); dot_audio_set_handsfree(false);
    assert(dot_audio_record_end(&wav, &bytes) == ESP_OK && bytes == 44 + 5 * MIC_FRAME * 2); free(wav);

    dot_audio_set_handsfree(true); feed_repeated(quiet, 20); feed_repeated(voice, 10);
    dot_audio_set_muted(true);
    assert(dot_audio_muted() && !dot_audio_detected_ready() && !s_candidate && !s_preroll_count);
    assert(dot_audio_record_begin() == ESP_ERR_INVALID_STATE);
    feed_repeated(voice, 20); assert(!s_candidate && !s_capture_samples);
    dot_audio_set_muted(false); feed_repeated(quiet, 20); feed_repeated(voice, 10); feed_repeated(quiet, 35);
    assert(dot_audio_detected_ready()); dot_audio_set_muted(true); assert(!dot_audio_detected_ready());
    assert(dot_audio_take_detected_wav(&wav, &bytes) == ESP_ERR_INVALID_STATE && !wav && !bytes);
    dot_audio_set_muted(false);

    feed_repeated(quiet, 20); feed_repeated(voice, 10); assert(s_candidate);
    atomic_store(&s_pcm_playing, true); feed_repeated(voice, 20);
    assert(!s_candidate && !s_preroll_count && !dot_audio_detected_ready());
    atomic_store(&s_pcm_playing, false);
    unsigned old = atomic_load(&s_capture_generation); dot_audio_record_cancel();
    capture_frame(voice, MIC_FRAME, old); assert(!s_candidate && !s_preroll_count);
    old = atomic_load(&s_capture_generation); assert(dot_audio_record_begin() == ESP_OK);
    capture_frame(voice, MIC_FRAME, old); assert(!s_capture_samples);
    feed_frame(voice, MIC_FRAME); assert(s_capture_samples == MIC_FRAME);
    dot_audio_record_cancel(); dot_audio_set_handsfree(false);
}

int main(void)
{
    assert(dot_audio_init()==ESP_OK);
    assert(dot_audio_record_begin()==ESP_OK);
    assert(dot_audio_record_begin()==ESP_ERR_INVALID_STATE);
    int16_t frame[MIC_FRAME];for(size_t i=0;i<MIC_FRAME;i++)frame[i]=(int16_t)(i*97-15000);
    for(unsigned i=0;i<DOT_RECORD_SECONDS*DOT_SAMPLE_RATE/MIC_FRAME+10;i++)feed_frame(frame,MIC_FRAME);
    assert(dot_audio_record_full());assert(s_capture_samples==RECORD_SAMPLES);
    uint8_t *wav=NULL;size_t length=0;
    assert(dot_audio_record_end(&wav,&length)==ESP_OK);
    assert(length==44+RECORD_SAMPLES*2);assert(!memcmp(wav,"RIFF",4));assert(!memcmp(wav+8,"WAVEfmt ",8));
    assert(wav[20]==1&&wav[22]==1&&wav[34]==16);
    uint32_t data_bytes;memcpy(&data_bytes,wav+40,4);assert(data_bytes==RECORD_SAMPLES*2);
    assert(!memcmp(wav+44,frame,sizeof(frame)));assert(!dot_audio_is_recording());assert(!dot_audio_record_full());
    assert(dot_audio_record_begin()==ESP_OK);feed_frame(frame,10);dot_audio_record_cancel();
    uint8_t *empty=(void *)1;size_t empty_size=99;assert(dot_audio_record_end(&empty,&empty_size)==ESP_ERR_INVALID_SIZE);assert(!empty&&!empty_size);
    test_handsfree();

    const uint8_t pcm[]={0,128,255,127,0,0,1,0,255,255,3,1,7,9,13,19};
    dot_config_t config={0};strcpy(config.audio_api_key,"host-test-placeholder");strcpy(config.stt_model,"gpt-4o-mini-transcribe");
    strcpy(config.tts_model,"gpt-4o-mini-tts");strcpy(config.voice,"coral");
    uint32_t stale_token=dot_audio_cancel_token();dot_audio_cancel_playback();
    reset_http(pcm,sizeof(pcm),"audio/pcm");
    assert(dot_openai_speak_guarded(&config,"Cancelled reply",stale_token)==ESP_ERR_INVALID_STATE);
    char stale_text[64]="old text";
    assert(dot_openai_transcribe_guarded(&config,wav,length,stale_text,sizeof(stale_text),stale_token)==ESP_ERR_INVALID_STATE);
    assert(!stale_text[0]&&!s_client_count&&!s_open_count&&!s_request_length&&!s_stream_length);
    reset_http(pcm,sizeof(pcm),"audio/pcm");
    audio_http_t state={.speech=true,.generation=atomic_load(&s_cancel_generation)};
    esp_http_client_handle_t client=make_client(&config,"https://api.openai.com/v1/audio/speech","application/json",&state);
    esp_http_client_set_post_field(client,"test request",12);
    assert(perform_request(client,&state)==ESP_OK);assert(s_open_count==3);assert(s_request_length==12);
    assert(!memcmp(s_request,"test request",12));assert(state.audio_bytes==sizeof(pcm));assert(!state.carried);
    assert(s_stream_length==sizeof(pcm)&&!memcmp(s_stream,pcm,sizeof(pcm)));
    s_speaker_length=0;assert(speaker_write(pcm,10,state.generation)==ESP_OK);assert(s_speaker_length==20);
    const int32_t expected[]={INT32_C(-1073741824),INT32_C(1073709056),0,32768,-32768};
    assert(!memcmp(s_speaker_bytes,expected,sizeof(expected)));
    const uint8_t level_pcm[]={0x70,0x17,0x90,0xe8}; // +/-6000 PCM16.
    atomic_store(&s_speaking,true); assert(dot_audio_speaking());
    dot_audio_set_volume(50); assert(dot_audio_volume()==50); s_speaker_length=0;
    assert(speaker_write(level_pcm,sizeof(level_pcm),state.generation)==ESP_OK);
    const int32_t half_gain[]={98304000,-98304000}; assert(!memcmp(s_speaker_bytes,half_gain,sizeof(half_gain)));
    assert(dot_audio_speech_level()==50);
    dot_audio_set_volume(0); s_speaker_length=0; assert(speaker_write(level_pcm,sizeof(level_pcm),state.generation)==ESP_OK);
    const int32_t silence[]={0,0}; assert(!memcmp(s_speaker_bytes,silence,sizeof(silence))); assert(!dot_audio_speech_level());
    dot_audio_set_volume(200); assert(dot_audio_volume()==100); s_speaker_length=0;
    assert(speaker_write(level_pcm,sizeof(level_pcm),state.generation)==ESP_OK && dot_audio_speech_level()==100);
    atomic_store(&s_speaking,false); assert(!dot_audio_speaking() && !dot_audio_speech_level());

    reset_http(pcm,sizeof(pcm),"audio/pcm");state=(audio_http_t){.speech=true,.generation=atomic_load(&s_cancel_generation)};
    client=make_client(&config,"https://api.openai.com/v1/audio/speech","application/json",&state);esp_http_client_set_post_field(client,"request",7);
    s_cancel_header=true;assert(perform_request(client,&state)==ESP_ERR_INVALID_STATE);assert(!s_stream_length);
    assert(speaker_write(pcm,10,state.generation)==ESP_ERR_INVALID_STATE);

    reset_http(pcm,sizeof(pcm),"application/json");state=(audio_http_t){.speech=true,.generation=atomic_load(&s_cancel_generation)};
    client=make_client(&config,"https://api.openai.com/v1/audio/speech","application/json",&state);esp_http_client_set_post_field(client,"request",7);
    assert(perform_request(client,&state)==ESP_ERR_INVALID_RESPONSE);assert(!s_stream_length);

    const char json[]="{\"text\":\"Turn the light on.\"}";char transcript[64];
    reset_http(json,strlen(json),"application/json");assert(dot_openai_transcribe(&config,wav,length,transcript,sizeof(transcript))==ESP_OK);
    assert(!strcmp(transcript,"Turn the light on."));assert(s_request_length>length);
    assert(!memcmp(s_request,"--dot-",6));assert(strstr((char *)s_request,"name=\"model\"\r\n\r\ngpt-4o-mini-transcribe\r\n"));
    assert(strstr((char *)s_request,"filename=\"speech.wav\""));
    reset_http(json,strlen(json),"application/json");assert(dot_openai_transcribe(&config,wav,length,transcript,4)==ESP_ERR_INVALID_SIZE);assert(!transcript[0]);
    reset_http(json,strlen(json),"application/json");s_status=429;assert(dot_openai_transcribe(&config,wav,length,transcript,sizeof(transcript))==ESP_ERR_INVALID_RESPONSE);assert(!transcript[0]);
    reset_http(json,strlen(json),"application/json");s_cancel_header=true;
    assert(dot_openai_transcribe_guarded(&config,wav,length,transcript,sizeof(transcript),dot_audio_cancel_token())==ESP_ERR_INVALID_STATE);assert(!transcript[0]);
    const char bad_json[]="{invalid}";reset_http(bad_json,strlen(bad_json),"application/json");
    assert(dot_openai_transcribe(&config,wav,length,transcript,sizeof(transcript))==ESP_ERR_INVALID_RESPONSE);
    char tiny[4]={0};state=(audio_http_t){.response=tiny,.capacity=sizeof(tiny)};
    esp_http_client_event_t overflow={.event_id=HTTP_EVENT_ON_DATA,.data="12345",.data_len=5,.user_data=&state,.client=&s_client};
    assert(http_event(&overflow)==ESP_ERR_INVALID_SIZE&&state.overflow&&!tiny[0]);
    free(wav);puts("audio tests passed: WAV cap, hands-free preroll/minimum/silence/cap, mute/manual/epoch isolation, PCM volume/amplitude, multipart/JSON, partial writes, odd PCM splits, guarded cancellation, MIME/errors/bounds");
    return 0;
}
