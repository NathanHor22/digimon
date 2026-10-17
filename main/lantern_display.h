#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define LANTERN_DISPLAY_MENU_ITEMS 5
#define LANTERN_DISPLAY_MENU_TEXT 48

typedef enum {
  LANTERN_CONNECTIVITY_OFFLINE = 0,
  LANTERN_CONNECTIVITY_RECONNECTING,
  LANTERN_CONNECTIVITY_WIFI_ONLY,
  LANTERN_CONNECTIVITY_WEAK,
  LANTERN_CONNECTIVITY_ONLINE,
  LANTERN_CONNECTIVITY_SETUP,
} lantern_connectivity_t;

typedef enum {
  LANTERN_SCREEN_BOOTING,
  LANTERN_SCREEN_SETUP,
  LANTERN_SCREEN_CONNECTING,
  LANTERN_SCREEN_READY,
  LANTERN_SCREEN_LISTENING,
  LANTERN_SCREEN_UNDERSTANDING,
  LANTERN_SCREEN_CONSENT,
  LANTERN_SCREEN_RECORDING,
  LANTERN_SCREEN_SAVING,
  LANTERN_SCREEN_COMPLETE,
  LANTERN_SCREEN_STATUS,
  LANTERN_SCREEN_ERROR,
} lantern_screen_t;

typedef enum {
  LANTERN_POWER_UNKNOWN = 0,
  LANTERN_POWER_BATTERY,
  LANTERN_POWER_CHARGING,
} lantern_power_state_t;

typedef struct {
  uint8_t eye_open;   /* 0..100 */
  uint8_t mouth_open; /* 0..100, played speech amplitude */
  bool listening, thinking, muted, wifi_connected, configured;
} lantern_face_t;

esp_err_t lantern_display_init(void);
/* Version 0.2 compact 240x240 views. Copies all data; does not block on DMA. */
void lantern_display_show_face(const lantern_face_t *face);
void lantern_display_show_compact_menu(const char *title, const char *const *items,
  size_t count, size_t selected, const char *footer);
void lantern_display_show_input(const char *title, const char *context,
  const char *preview, const char *selected, unsigned length, unsigned limit,
  const char *hint);
void lantern_display_show_volume(uint8_t volume);
// Non-blocking, task-context only; copies detail and coalesces pending updates.
void lantern_display_show(lantern_screen_t screen, const char *detail);
// Touch-first views use fixed-size copies so rendering never retains caller memory.
void lantern_display_show_splash(void);
void lantern_display_show_home(const char *footer);
void lantern_display_show_menu(const char *title, const char *const *items,
                               size_t item_count, const char *footer);
void lantern_display_show_keyboard(const char *ssid, const char *password,
                                   bool uppercase);
void lantern_display_show_detail(const char *title, const char *body,
                                 const char *footer);
void lantern_display_set_charge_eta(int minutes);
void lantern_display_set_awake(bool awake);
// Updates the persistent top status bar without changing the active screen.
// battery_level is -1 when measurement is unavailable; wifi_rssi is dBm.
void lantern_display_set_status(
  int battery_level,
  lantern_power_state_t power_state,
  lantern_connectivity_t connectivity,
  int wifi_rssi,
  bool paired);
