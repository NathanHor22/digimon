#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "dot_ui.h"
#include "lantern_display.h"

static lantern_face_t last_face;
static char last_preview[65], last_choice[48];
static unsigned face_frames;
void lantern_display_show_face(const lantern_face_t *face) { last_face = *face; ++face_frames; }
void lantern_display_show_compact_menu(const char *title, const char *const *items,
    size_t count, size_t selected, const char *footer)
{
    assert(title && footer && items && count && count <= 5 && selected < count);
    for (size_t i = 0; i < count; ++i) assert(items[i] && strlen(items[i]) < 48);
}
void lantern_display_show_input(const char *title, const char *context, const char *preview,
    const char *selected, unsigned length, unsigned limit, const char *hint)
{
    assert(title && context && hint && length <= limit && limit <= 64);
    snprintf(last_preview, sizeof(last_preview), "%s", preview);
    snprintf(last_choice, sizeof(last_choice), "%s", selected);
}
void lantern_display_show_volume(uint8_t volume) { assert(volume <= 100); }
static dot_ui_view_t view(void) { dot_ui_view_t v; dot_ui_get_view(&v); return v; }
static void choose(unsigned index)
{
    for (unsigned step = 0; view().character_index != index && step < 100; ++step)
        dot_ui_event(DOT_UI_DOWN);
    assert(view().character_index == index);
    dot_ui_event(DOT_UI_SELECT);
}
static void character(char ch) { choose((unsigned char)ch - 32); }
static void open_wifi(void)
{
    dot_ui_settings_open(); dot_ui_event(DOT_UI_SELECT);
    assert(view().page == DOT_UI_WIFI);
    assert(dot_ui_scan_requested()); assert(!dot_ui_scan_requested());
}
static void test_scan_and_secret_round_trip(void)
{
    dot_ui_init(); open_wifi();
    dot_ui_network_t networks[] = {
        {.ssid="weak",.rssi=-90,.secured=false},
        {.ssid="strong",.rssi=-30,.secured=true},
        {.ssid="strong",.rssi=-35,.secured=true},
        {.ssid="",.rssi=-1,.secured=false}
    };
    dot_ui_set_networks(networks, 4, false);
    assert(!view().scanning);
    dot_ui_event(DOT_UI_SELECT);
    assert(view().page == DOT_UI_PASSWORD);
    const char *password = "aA0 !~\\?";
    assert(strlen(password) == 8);
    for (const char *p = password; *p; ++p) character(*p);
    assert(view().input_length == 8);
    dot_ui_snapshot_t frame = {0};
    dot_ui_render(&frame);
    assert(!strcmp(last_preview, "********"));
    assert(!strstr(last_preview, password));
    choose(96); /* JOIN */
    assert(view().page == DOT_UI_JOINING);
    char ssid[33], returned[65];
    assert(!dot_ui_take_wifi(NULL, returned));
    assert(dot_ui_take_wifi(ssid, returned));
    assert(!strcmp(ssid, "strong") && !strcmp(returned, password));
    assert(!dot_ui_take_wifi(ssid, returned));
    assert(view().input_length == 0); /* Pending secret wiped after transfer. */
    dot_ui_wifi_result(true);
    assert(!dot_ui_is_open());
}
static void test_all_printable_ascii_and_cancel(void)
{
    dot_ui_init(); open_wifi();
    dot_ui_network_t network = {.ssid="network",.rssi=-40,.secured=true};
    dot_ui_set_networks(&network, 1, false);
    dot_ui_event(DOT_UI_SELECT);
    dot_ui_snapshot_t frame = {0};
    for (unsigned code = 32; code <= 126; ++code) {
        character((char)code);
        assert(view().input_length == 1);
        dot_ui_render(&frame);
        assert(!strcmp(last_preview, "*"));
        if (code == 32) assert(!strcmp(last_choice, "SPACE"));
        else assert(last_choice[0] == (char)code && !last_choice[1]);
        choose(95); /* DELETE */
        assert(view().input_length == 0);
    }
    character('s'); character('e'); character('c');
    dot_ui_event(DOT_UI_BACK);
    assert(view().page == DOT_UI_WIFI && view().input_length == 0);
    dot_ui_event(DOT_UI_SELECT);
    assert(view().page == DOT_UI_PASSWORD && view().input_length == 0);
    char ssid[33], pass[65];
    assert(!dot_ui_take_wifi(ssid, pass));
}
static void test_input_limits_and_hidden_network(void)
{
    dot_ui_init(); open_wifi(); dot_ui_set_networks(NULL, 0, false);
    dot_ui_event(DOT_UI_DOWN); dot_ui_event(DOT_UI_SELECT);
    assert(view().page == DOT_UI_SSID && view().input_limit == 32);
    for (unsigned i = 0; i < 33; ++i) character('a');
    assert(view().input_length == 32 && view().input_full);
    choose(95); assert(view().input_length == 31);
    choose(96); assert(view().page == DOT_UI_PASSWORD);
    for (unsigned i = 0; i < 65; ++i) character('a');
    assert(view().input_length == 64 && view().input_full);
    choose(96); assert(view().page == DOT_UI_JOINING); /* 64 hex PSK accepted. */
    char ssid[33], password[65];
    assert(dot_ui_take_wifi(ssid, password));
    assert(strlen(ssid) == 31 && strlen(password) == 64);
    dot_ui_wifi_result(false);
    assert(view().page == DOT_UI_JOINING);
    dot_ui_event(DOT_UI_BACK); assert(view().page == DOT_UI_WIFI);
    dot_ui_wifi_result(true); assert(view().page == DOT_UI_WIFI); /* Stale result cannot close menu. */
}
static void test_password_validation_and_open_network(void)
{
    dot_ui_init(); open_wifi();
    dot_ui_network_t network = {.ssid="secured",.rssi=-40,.secured=true};
    dot_ui_set_networks(&network, 1, false); dot_ui_event(DOT_UI_SELECT);
    for (unsigned i = 0; i < 7; ++i) character('g');
    choose(96); assert(view().page == DOT_UI_PASSWORD);
    for (unsigned i = 7; i < 64; ++i) character('g');
    choose(96); assert(view().page == DOT_UI_PASSWORD); /* Nonhex 64 rejected. */
    choose(95); choose(96); assert(view().page == DOT_UI_JOINING); /* 63-char passphrase. */
    dot_ui_init(); open_wifi();
    network.secured = false; dot_ui_set_networks(&network, 1, false);
    dot_ui_event(DOT_UI_SELECT); choose(96);
    char ssid[33], password[65];
    assert(dot_ui_take_wifi(ssid, password) && !*password);
}
static void test_preferences_and_navigation_bounds(void)
{
    dot_ui_init(); dot_ui_set_preferences(255, false, true);
    assert(view().volume == 100);
    dot_ui_settings_open();
    dot_ui_event(DOT_UI_UP); assert(view().selection == 4);
    dot_ui_event(DOT_UI_DOWN); assert(view().selection == 0);
    dot_ui_event(DOT_UI_DOWN); dot_ui_event(DOT_UI_SELECT);
    assert(view().page == DOT_UI_VOLUME);
    for (unsigned i = 0; i < 50; ++i) dot_ui_event(DOT_UI_DOWN);
    assert(view().volume == 0);
    uint8_t volume; assert(dot_ui_take_volume(&volume) && volume == 0);
    assert(!dot_ui_take_volume(&volume));
    for (unsigned i = 0; i < 50; ++i) dot_ui_event(DOT_UI_UP);
    assert(view().volume == 100);
    dot_ui_event(DOT_UI_BACK); dot_ui_event(DOT_UI_DOWN); dot_ui_event(DOT_UI_SELECT);
    bool mute; assert(dot_ui_take_mute(&mute) && mute);
    assert(!dot_ui_take_mute(&mute));
    dot_ui_event(DOT_UI_DOWN); dot_ui_event(DOT_UI_SELECT);
    bool handsfree; assert(dot_ui_take_handsfree(&handsfree) && !handsfree);
    assert(!dot_ui_take_handsfree(&handsfree));
    dot_ui_event(DOT_UI_BACK); assert(!dot_ui_is_open());
}
static void test_scan_bound_and_face_animation(void)
{
    dot_ui_init(); open_wifi();
    dot_ui_network_t many[16] = {0};
    for (unsigned i = 0; i < 16; ++i) {
        snprintf(many[i].ssid, sizeof(many[i].ssid), "network-%u", i); many[i].rssi = -(int)i;
    }
    dot_ui_set_networks(many, 16, false);
    dot_ui_event(DOT_UI_UP); assert(view().selection == DOT_UI_NETWORKS_MAX + 2);
    dot_ui_event(DOT_UI_BACK); dot_ui_event(DOT_UI_BACK);
    dot_ui_snapshot_t frame = {.configured=true,.wifi_connected=true,.now_ms=1000,.speech_level=100};
    dot_ui_render(&frame);
    assert(last_face.eye_open == 100 && last_face.mouth_open == 0);
    frame.speaking = true; frame.speech_level = 10; dot_ui_render(&frame);
    unsigned quiet_mouth = last_face.mouth_open;
    frame.speech_level = 80; dot_ui_render(&frame);
    assert(last_face.mouth_open > quiet_mouth);
    frame.speech_level = 0; dot_ui_render(&frame); assert(!last_face.mouth_open);
    frame.listening = true; frame.muted = true; dot_ui_render(&frame);
    assert(last_face.muted && !last_face.listening);
    frame.now_ms = 5080; dot_ui_render(&frame); assert(!last_face.eye_open);
    frame.now_ms = 5160; dot_ui_render(&frame); assert(last_face.eye_open == 50);
    frame.now_ms = 5200; dot_ui_render(&frame); assert(last_face.eye_open == 100);
    assert(face_frames >= 8);
}
int main(void)
{
    test_scan_and_secret_round_trip();
    test_all_printable_ascii_and_cancel();
    test_input_limits_and_hidden_network();
    test_password_validation_and_open_network();
    test_preferences_and_navigation_bounds();
    test_scan_bound_and_face_animation();
    puts("PASS: production UI character coverage/bounds, secret handling, Wi-Fi selection, volume/mute/hands-free, deterministic blinking and speech amplitude");
    return 0;
}
