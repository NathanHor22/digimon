#include "dot_ui.h"
#include "lantern_display.h"
#include <stdio.h>
#include <string.h>

#define CHARACTERS 95U /* Every printable ASCII byte, space through tilde. */
#define DELETE_ITEM 95U
#define DONE_ITEM 96U
#define CANCEL_ITEM 97U
#define PICKER_ITEMS 98U

static struct {
    dot_ui_page_t page;
    unsigned selection, character;
    uint8_t volume;
    bool muted, handsfree, volume_pending, mute_pending, handsfree_pending;
    bool scan_pending, scanning, scan_failed, input_full, invalid_password;
    bool secured, wifi_pending, joining_failed;
    dot_ui_network_t networks[DOT_UI_NETWORKS_MAX];
    size_t count;
    char ssid[33], password[65];
    uint32_t revision, rendered_revision;
} s_ui;

static void wipe(void *data, size_t length)
{
    volatile unsigned char *p = data;
    while (length--) *p++ = 0;
}
static void changed(void) { ++s_ui.revision; }
static void start_scan(void)
{
    s_ui.scan_pending = true;
    s_ui.scanning = true;
    s_ui.scan_failed = false;
    changed();
}
static void editor(dot_ui_page_t page)
{
    s_ui.page = page;
    s_ui.character = (unsigned)'a' - 32;
    s_ui.input_full = false;
    s_ui.invalid_password = false;
    changed();
}
void dot_ui_init(void)
{
    wipe(&s_ui, sizeof(s_ui));
    s_ui.page = DOT_UI_FACE;
    s_ui.volume = 65;
    s_ui.handsfree = true;
    s_ui.revision = 1;
}
void dot_ui_set_preferences(uint8_t volume, bool muted, bool handsfree)
{
    if (volume > 100) volume = 100;
    if (s_ui.volume != volume || s_ui.muted != muted || s_ui.handsfree != handsfree) {
        s_ui.volume = volume; s_ui.muted = muted; s_ui.handsfree = handsfree;
        changed();
    }
}
void dot_ui_settings_open(void)
{
    s_ui.page = DOT_UI_SETTINGS; s_ui.selection = 0;
    s_ui.input_full = false; s_ui.invalid_password = false;
    s_ui.wifi_pending = false;
    wipe(s_ui.password, sizeof(s_ui.password));
    changed();
}
bool dot_ui_is_open(void) { return s_ui.page != DOT_UI_FACE; }
bool dot_ui_scan_requested(void)
{
    bool pending = s_ui.scan_pending; s_ui.scan_pending = false; return pending;
}
void dot_ui_set_networks(const dot_ui_network_t *networks, size_t count, bool failed)
{
    s_ui.scanning = false; s_ui.scan_failed = failed; s_ui.count = 0;
    if (networks && !failed) {
        if (count > 64) count = 64;
        for (size_t i = 0; i < count && s_ui.count < DOT_UI_NETWORKS_MAX; ++i) {
            const char *end = memchr(networks[i].ssid, 0, sizeof(networks[i].ssid));
            if (!end || end == networks[i].ssid) continue;
            bool duplicate = false;
            for (size_t n = 0; n < s_ui.count; ++n)
                if (!strcmp(s_ui.networks[n].ssid, networks[i].ssid)) duplicate = true;
            if (duplicate) continue;
            size_t at = s_ui.count++;
            while (at && s_ui.networks[at - 1].rssi < networks[i].rssi) {
                s_ui.networks[at] = s_ui.networks[at - 1]; --at;
            }
            s_ui.networks[at] = networks[i];
        }
    }
    if (s_ui.page == DOT_UI_WIFI) s_ui.selection = 0;
    changed();
}
static void wifi_back(void)
{
    wipe(s_ui.password, sizeof(s_ui.password));
    s_ui.wifi_pending = false;
    s_ui.page = DOT_UI_WIFI; s_ui.selection = 0;
    s_ui.input_full = false; s_ui.invalid_password = false;
    changed();
}
static bool password_valid(void)
{
    size_t length = strlen(s_ui.password);
    if (!length) return !s_ui.secured;
    if (length >= 8 && length <= 63) return true;
    if (length != 64) return false;
    for (const char *p = s_ui.password; *p; ++p)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
            (*p >= 'A' && *p <= 'F'))) return false;
    return true;
}
static void edit_event(dot_ui_event_t event)
{
    if (event == DOT_UI_BACK) { wifi_back(); return; }
    if (event == DOT_UI_UP) s_ui.character = (s_ui.character + PICKER_ITEMS - 1) % PICKER_ITEMS;
    else if (event == DOT_UI_DOWN) s_ui.character = (s_ui.character + 1) % PICKER_ITEMS;
    else if (event == DOT_UI_SELECT) {
        char *input = s_ui.page == DOT_UI_SSID ? s_ui.ssid : s_ui.password;
        size_t limit = s_ui.page == DOT_UI_SSID ? 32 : 64;
        size_t length = strlen(input);
        s_ui.input_full = false; s_ui.invalid_password = false;
        if (s_ui.character < CHARACTERS) {
            if (length < limit) {
                input[length] = (char)(s_ui.character + 32); input[length + 1] = 0;
            } else s_ui.input_full = true;
        } else if (s_ui.character == DELETE_ITEM) {
            if (length) input[length - 1] = 0;
        } else if (s_ui.character == CANCEL_ITEM) { wifi_back(); return; }
        else if (s_ui.character == DONE_ITEM) {
            if (s_ui.page == DOT_UI_SSID) {
                if (*s_ui.ssid) { wipe(s_ui.password, sizeof(s_ui.password)); editor(DOT_UI_PASSWORD); }
                else s_ui.input_full = true;
            } else if (password_valid()) {
                s_ui.wifi_pending = true; s_ui.joining_failed = false;
                s_ui.page = DOT_UI_JOINING;
            } else s_ui.invalid_password = true;
        }
    }
    changed();
}
void dot_ui_event(dot_ui_event_t event)
{
    if (s_ui.page == DOT_UI_FACE) return;
    if (s_ui.page == DOT_UI_PASSWORD || s_ui.page == DOT_UI_SSID) { edit_event(event); return; }
    if (s_ui.page == DOT_UI_VOLUME) {
        if (event == DOT_UI_UP && s_ui.volume < 100) {
            s_ui.volume = s_ui.volume > 95 ? 100 : (uint8_t)(s_ui.volume + 5);
            s_ui.volume_pending = true;
        } else if (event == DOT_UI_DOWN && s_ui.volume > 0) {
            s_ui.volume = s_ui.volume < 5 ? 0 : (uint8_t)(s_ui.volume - 5);
            s_ui.volume_pending = true;
        } else if (event == DOT_UI_SELECT || event == DOT_UI_BACK) {
            s_ui.page = DOT_UI_SETTINGS; s_ui.selection = 1;
        }
        changed(); return;
    }
    if (s_ui.page == DOT_UI_JOINING) {
        if (event == DOT_UI_BACK || event == DOT_UI_SELECT) {
            s_ui.wifi_pending = false; wifi_back();
        }
        return;
    }
    if (event == DOT_UI_BACK) {
        if (s_ui.page == DOT_UI_SETTINGS) s_ui.page = DOT_UI_FACE;
        else { s_ui.page = DOT_UI_SETTINGS; s_ui.selection = 0; }
        wipe(s_ui.password, sizeof(s_ui.password)); changed(); return;
    }
    unsigned count = s_ui.page == DOT_UI_SETTINGS ? 5 : (unsigned)s_ui.count + 3;
    if (event == DOT_UI_UP) s_ui.selection = (s_ui.selection + count - 1) % count;
    else if (event == DOT_UI_DOWN) s_ui.selection = (s_ui.selection + 1) % count;
    else if (event == DOT_UI_SELECT && s_ui.page == DOT_UI_SETTINGS) {
        switch (s_ui.selection) {
            case 0: s_ui.page = DOT_UI_WIFI; s_ui.selection = 0; start_scan(); break;
            case 1: s_ui.page = DOT_UI_VOLUME; break;
            case 2: s_ui.muted = !s_ui.muted; s_ui.mute_pending = true; break;
            case 3: s_ui.handsfree = !s_ui.handsfree; s_ui.handsfree_pending = true; break;
            case 4: s_ui.page = DOT_UI_FACE; break;
        }
    } else if (event == DOT_UI_SELECT && s_ui.page == DOT_UI_WIFI) {
        if (s_ui.selection < s_ui.count) {
            strcpy(s_ui.ssid, s_ui.networks[s_ui.selection].ssid);
            s_ui.secured = s_ui.networks[s_ui.selection].secured;
            wipe(s_ui.password, sizeof(s_ui.password)); editor(DOT_UI_PASSWORD);
        } else if (s_ui.selection == s_ui.count) start_scan();
        else if (s_ui.selection == s_ui.count + 1) {
            wipe(s_ui.ssid, sizeof(s_ui.ssid)); wipe(s_ui.password, sizeof(s_ui.password));
            s_ui.secured = false; editor(DOT_UI_SSID);
        } else { s_ui.page = DOT_UI_SETTINGS; s_ui.selection = 0; }
    }
    changed();
}
bool dot_ui_take_wifi(char ssid[33], char password[65])
{
    if (!s_ui.wifi_pending || !ssid || !password) return false;
    memcpy(ssid, s_ui.ssid, sizeof(s_ui.ssid));
    memcpy(password, s_ui.password, sizeof(s_ui.password));
    wipe(s_ui.password, sizeof(s_ui.password)); s_ui.wifi_pending = false;
    return true;
}
void dot_ui_wifi_result(bool connected)
{
    if (s_ui.page != DOT_UI_JOINING) return;
    s_ui.joining_failed = !connected;
    if (connected) s_ui.page = DOT_UI_FACE;
    changed();
}
bool dot_ui_take_volume(uint8_t *volume)
{
    if (!s_ui.volume_pending || !volume) return false;
    *volume = s_ui.volume; s_ui.volume_pending = false; return true;
}
bool dot_ui_take_mute(bool *muted)
{
    if (!s_ui.mute_pending || !muted) return false;
    *muted = s_ui.muted; s_ui.mute_pending = false; return true;
}
bool dot_ui_take_handsfree(bool *handsfree)
{
    if (!s_ui.handsfree_pending || !handsfree) return false;
    *handsfree = s_ui.handsfree; s_ui.handsfree_pending = false; return true;
}
void dot_ui_get_view(dot_ui_view_t *view)
{
    if (!view) return;
    *view = (dot_ui_view_t) {
        .page = s_ui.page, .selection = s_ui.selection, .character_index = s_ui.character,
        .input_length = (unsigned)strlen(s_ui.page == DOT_UI_SSID ? s_ui.ssid : s_ui.password),
        .input_limit = s_ui.page == DOT_UI_SSID ? 32 : 64,
        .volume = s_ui.volume, .muted = s_ui.muted, .handsfree = s_ui.handsfree,
        .scanning = s_ui.scanning, .input_full = s_ui.input_full,
    };
}
static void render_editor(void)
{
    bool secret = s_ui.page == DOT_UI_PASSWORD;
    const char *input = secret ? s_ui.password : s_ui.ssid;
    unsigned length = (unsigned)strlen(input);
    char preview[33] = {0}, selected[32];
    unsigned shown = length > 30 ? 30 : length;
    if (secret) memset(preview, '*', shown);
    else memcpy(preview, input + length - shown, shown);
    if (s_ui.character == DELETE_ITEM) strcpy(selected, "DELETE");
    else if (s_ui.character == DONE_ITEM) strcpy(selected, secret ? "JOIN" : "NEXT");
    else if (s_ui.character == CANCEL_ITEM) strcpy(selected, "CANCEL");
    else if (!s_ui.character) strcpy(selected, "SPACE");
    else snprintf(selected, sizeof(selected), "%c", (char)(s_ui.character + 32));
    const char *hint = s_ui.invalid_password ? "Use 8-63 chars or 64 hex" :
        s_ui.input_full ? (length ? "Input full - delete to edit" : "Enter a network name") :
        secret ? "Scroll for DELETE / JOIN" : "Scroll for DELETE / NEXT";
    lantern_display_show_input(secret ? "Wi-Fi password" : "Hidden network",
        secret ? s_ui.ssid : "Network name", preview, selected, length,
        secret ? 64 : 32, hint);
}
void dot_ui_render(const dot_ui_snapshot_t *snapshot)
{
    if (!snapshot) return;
    if (s_ui.page == DOT_UI_FACE) {
        unsigned phase = snapshot->now_ms % 5200U;
        unsigned eyes = 100;
        if (phase >= 5020 && phase < 5080) eyes = 100 - (phase - 5020) * 100 / 60;
        else if (phase >= 5080 && phase < 5120) eyes = 0;
        else if (phase >= 5120) eyes = (phase - 5120) * 100 / 80;
        unsigned level = snapshot->speech_level > 100 ? 100 : snapshot->speech_level;
        lantern_face_t face = {
            .eye_open = (uint8_t)eyes,
            .mouth_open = snapshot->speaking && level > 3 ? (uint8_t)(((level + 4) / 5) * 5) : 0,
            .listening = snapshot->listening && !snapshot->muted,
            .thinking = snapshot->thinking,
            .muted = snapshot->muted,
            .wifi_connected = snapshot->wifi_connected,
            .configured = snapshot->configured,
        };
        lantern_display_show_face(&face);
        return;
    }
    if (s_ui.rendered_revision == s_ui.revision) return;
    s_ui.rendered_revision = s_ui.revision;
    if (s_ui.page == DOT_UI_SSID || s_ui.page == DOT_UI_PASSWORD) { render_editor(); return; }
    if (s_ui.page == DOT_UI_VOLUME) {
        lantern_display_show_volume(s_ui.volume);
        return;
    }
    if (s_ui.page == DOT_UI_JOINING) {
        const char *items[] = {s_ui.joining_failed ? "Try Wi-Fi again" : "Joining Wi-Fi...", "Back"};
        lantern_display_show_compact_menu(s_ui.ssid, items, 2, s_ui.joining_failed ? 1 : 0,
            s_ui.joining_failed ? "Check password / centre: back" : "Centre: back");
        return;
    }
    if (s_ui.page == DOT_UI_SETTINGS) {
        char mute[32], handsfree[32];
        snprintf(mute, sizeof(mute), "Microphone: %s", s_ui.muted ? "muted" : "on");
        snprintf(handsfree, sizeof(handsfree), "Hands-free: %s", s_ui.handsfree ? "on" : "off");
        const char *items[] = {"Wi-Fi", "Volume", mute, handsfree, "Back"};
        lantern_display_show_compact_menu("Settings", items, 5, s_ui.selection, "UP/DOWN  centre: select");
        return;
    }
    char labels[5][48];
    const char *items[5];
    unsigned total = (unsigned)s_ui.count + 3;
    unsigned first = s_ui.selection >= 5 ? s_ui.selection - 4 : 0;
    unsigned visible = total - first; if (visible > 5) visible = 5;
    for (unsigned i = 0; i < visible; ++i) {
        unsigned item = first + i;
        if (item < s_ui.count) snprintf(labels[i], sizeof(labels[i]), "%s%s",
            s_ui.networks[item].secured ? "* " : "", s_ui.networks[item].ssid);
        else if (item == s_ui.count) strcpy(labels[i], s_ui.scanning ? "Scanning..." : "Scan again");
        else if (item == s_ui.count + 1) strcpy(labels[i], "Hidden / manual network");
        else strcpy(labels[i], "Back");
        items[i] = labels[i];
    }
    lantern_display_show_compact_menu("Wi-Fi", items, visible, s_ui.selection - first,
        s_ui.scan_failed ? "Scan failed - try again" :
        s_ui.scanning ? "Scanning / centre: choose" : "UP/DOWN  centre: choose");
}
