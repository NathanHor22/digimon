#include "lantern_display.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lantern_board.h"

static const char *TAG = "lantern_display";
#if LANTERN_DISPLAY_DRIVER_ST7789
static esp_lcd_panel_handle_t s_panel;
#endif
static esp_lcd_panel_io_handle_t s_panel_io;
static uint16_t *s_pixels;
static uint16_t *s_transfer_pixels;
static SemaphoreHandle_t s_transfer_done;
static QueueHandle_t s_updates;

typedef struct {
  enum { DISPLAY_STATE, DISPLAY_SPLASH, DISPLAY_HOME, DISPLAY_MENU, DISPLAY_KEYBOARD, DISPLAY_DETAIL,
    DISPLAY_FACE, DISPLAY_COMPACT_MENU, DISPLAY_INPUT, DISPLAY_VOLUME } view;
  lantern_screen_t screen;
  char detail[96];
  char title[48];
  char items[LANTERN_DISPLAY_MENU_ITEMS][LANTERN_DISPLAY_MENU_TEXT];
  size_t item_count;
  char input[65];
  bool uppercase;
  int charge_eta_minutes;
  uint32_t revision;
  int battery_level;
  lantern_power_state_t power_state;
  int wifi_rssi;
  lantern_connectivity_t connectivity;
  bool paired;
  lantern_face_t face;
  unsigned selected, input_length, input_limit, volume;
} display_update_t;

static portMUX_TYPE s_update_lock = portMUX_INITIALIZER_UNLOCKED;
static display_update_t s_requested = {
  .screen = LANTERN_SCREEN_BOOTING,
  .view = DISPLAY_STATE,
  .charge_eta_minutes = -1,
  .battery_level = -1,
  .power_state = LANTERN_POWER_UNKNOWN,
  .wifi_rssi = -127,
};

// Only this task owns the framebuffer and DMA staging buffer. A one-item
// mailbox keeps the latest state instead of replaying stale progress screens.
static void render_screen(const display_update_t *update);

static void display_task(void *argument) {
  (void)argument;
  display_update_t update;
  while (true) {
    if (xQueueReceive(s_updates, &update, portMAX_DELAY) == pdTRUE) {
      render_screen(&update);
    }
  }
}

#define TRANSFER_ROWS 16

#if LANTERN_DISPLAY_DRIVER_ILI9341
static esp_err_t ili9341_command(uint8_t command, const uint8_t *parameters, size_t length) {
  return esp_lcd_panel_io_tx_param(s_panel_io, command, parameters, length);
}

static esp_err_t ili9341_init(void) {
  static const uint8_t power_b[] = {0x00, 0xc1, 0x30};
  static const uint8_t power_seq[] = {0x64, 0x03, 0x12, 0x81};
  static const uint8_t timing_a[] = {0x85, 0x00, 0x78};
  static const uint8_t power_a[] = {0x39, 0x2c, 0x00, 0x34, 0x02};
  static const uint8_t pump[] = {0x20};
  static const uint8_t timing_b[] = {0x00, 0x00};
  static const uint8_t power_1[] = {0x23};
  static const uint8_t power_2[] = {0x10};
  static const uint8_t vcom_1[] = {0x3e, 0x28};
  static const uint8_t vcom_2[] = {0x86};
  static const uint8_t memory_access[] = {0x48};
  static const uint8_t pixel_format[] = {0x55};
  static const uint8_t frame_rate[] = {0x00, 0x18};
  static const uint8_t display_function[] = {0x08, 0x82, 0x27};
  static const uint8_t gamma_disable[] = {0x00};
  static const uint8_t gamma_curve[] = {0x01};
  static const uint8_t gamma_positive[] = {
    0x0f, 0x31, 0x2b, 0x0c, 0x0e, 0x08, 0x4e, 0xf1,
    0x37, 0x07, 0x10, 0x03, 0x0e, 0x09, 0x00,
  };
  static const uint8_t gamma_negative[] = {
    0x00, 0x0e, 0x14, 0x03, 0x11, 0x07, 0x31, 0xc1,
    0x48, 0x08, 0x0f, 0x0c, 0x31, 0x36, 0x0f,
  };

  ESP_RETURN_ON_ERROR(ili9341_command(0x01, NULL, 0), TAG, "ILI9341 reset failed");
  vTaskDelay(pdMS_TO_TICKS(150));
  ESP_RETURN_ON_ERROR(ili9341_command(0xcf, power_b, sizeof(power_b)), TAG, "ILI9341 power B failed");
  ESP_RETURN_ON_ERROR(ili9341_command(0xed, power_seq, sizeof(power_seq)), TAG, "ILI9341 power sequence failed");
  ESP_RETURN_ON_ERROR(ili9341_command(0xe8, timing_a, sizeof(timing_a)), TAG, "ILI9341 timing A failed");
  ESP_RETURN_ON_ERROR(ili9341_command(0xcb, power_a, sizeof(power_a)), TAG, "ILI9341 power A failed");
  ESP_RETURN_ON_ERROR(ili9341_command(0xf7, pump, sizeof(pump)), TAG, "ILI9341 pump failed");
  ESP_RETURN_ON_ERROR(ili9341_command(0xea, timing_b, sizeof(timing_b)), TAG, "ILI9341 timing B failed");
  ESP_RETURN_ON_ERROR(ili9341_command(0xc0, power_1, sizeof(power_1)), TAG, "ILI9341 power 1 failed");
  ESP_RETURN_ON_ERROR(ili9341_command(0xc1, power_2, sizeof(power_2)), TAG, "ILI9341 power 2 failed");
  ESP_RETURN_ON_ERROR(ili9341_command(0xc5, vcom_1, sizeof(vcom_1)), TAG, "ILI9341 VCOM 1 failed");
  ESP_RETURN_ON_ERROR(ili9341_command(0xc7, vcom_2, sizeof(vcom_2)), TAG, "ILI9341 VCOM 2 failed");
  ESP_RETURN_ON_ERROR(ili9341_command(0x36, memory_access, sizeof(memory_access)), TAG, "ILI9341 orientation failed");
  ESP_RETURN_ON_ERROR(ili9341_command(0x3a, pixel_format, sizeof(pixel_format)), TAG, "ILI9341 pixel format failed");
  ESP_RETURN_ON_ERROR(ili9341_command(0xb1, frame_rate, sizeof(frame_rate)), TAG, "ILI9341 frame rate failed");
  ESP_RETURN_ON_ERROR(ili9341_command(0xb6, display_function, sizeof(display_function)), TAG, "ILI9341 function failed");
  ESP_RETURN_ON_ERROR(ili9341_command(0xf2, gamma_disable, sizeof(gamma_disable)), TAG, "ILI9341 gamma mode failed");
  ESP_RETURN_ON_ERROR(ili9341_command(0x26, gamma_curve, sizeof(gamma_curve)), TAG, "ILI9341 gamma curve failed");
  ESP_RETURN_ON_ERROR(ili9341_command(0xe0, gamma_positive, sizeof(gamma_positive)), TAG, "ILI9341 positive gamma failed");
  ESP_RETURN_ON_ERROR(ili9341_command(0xe1, gamma_negative, sizeof(gamma_negative)), TAG, "ILI9341 negative gamma failed");
  ESP_RETURN_ON_ERROR(ili9341_command(0x11, NULL, 0), TAG, "ILI9341 sleep exit failed");
  vTaskDelay(pdMS_TO_TICKS(120));
  // The ES3C28P IPS panel is wired for inverted colors. This matches the
  // manufacturer's BSP (`BSP_LCD_INVERTED = true`).
  ESP_RETURN_ON_ERROR(ili9341_command(0x21, NULL, 0), TAG, "ILI9341 inversion failed");
  ESP_RETURN_ON_ERROR(ili9341_command(0x29, NULL, 0), TAG, "ILI9341 display on failed");
  vTaskDelay(pdMS_TO_TICKS(20));
  return ESP_OK;
}

static esp_err_t ili9341_draw_bitmap(int y, int rows, const uint16_t *pixels) {
  const uint8_t columns[] = {0x00, 0x00, 0x00, LANTERN_DISPLAY_WIDTH - 1};
  const uint8_t pages[] = {
    (uint8_t)(y >> 8), (uint8_t)y,
    (uint8_t)((y + rows - 1) >> 8), (uint8_t)(y + rows - 1),
  };
  ESP_RETURN_ON_ERROR(ili9341_command(0x2a, columns, sizeof(columns)), TAG, "ILI9341 column failed");
  ESP_RETURN_ON_ERROR(ili9341_command(0x2b, pages, sizeof(pages)), TAG, "ILI9341 page failed");
  return esp_lcd_panel_io_tx_color(
    s_panel_io, 0x2c, pixels, (size_t)rows * LANTERN_DISPLAY_WIDTH * sizeof(uint16_t));
}
#endif

static bool transfer_finished(esp_lcd_panel_io_handle_t panel_io,
                              esp_lcd_panel_io_event_data_t *event,
                              void *context) {
  (void)panel_io;
  (void)event;
  (void)context;
  BaseType_t task_woken = pdFALSE;
  xSemaphoreGiveFromISR(s_transfer_done, &task_woken);
  return task_woken == pdTRUE;
}

static uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue) {
  return (uint16_t)(((red & 0xf8) << 8) | ((green & 0xfc) << 3) | (blue >> 3));
}

static void pixel(int x, int y, uint16_t color) {
  if (!s_pixels || x < 0 || y < 0 || x >= LANTERN_DISPLAY_WIDTH || y >= LANTERN_DISPLAY_HEIGHT) return;
  s_pixels[y * LANTERN_DISPLAY_WIDTH + x] = color;
}

static void fill(uint16_t color) {
  if (!s_pixels) return;
  for (size_t index = 0; index < LANTERN_DISPLAY_WIDTH * LANTERN_DISPLAY_HEIGHT; ++index) {
    s_pixels[index] = color;
  }
}

static void rect(int x, int y, int width, int height, uint16_t color) {
  for (int row = y; row < y + height; ++row) {
    for (int column = x; column < x + width; ++column) pixel(column, row, color);
  }
}

static void outline_rect(int x, int y, int width, int height, int thickness, uint16_t color) {
  rect(x, y, width, thickness, color);
  rect(x, y + height - thickness, width, thickness, color);
  rect(x, y + thickness, thickness, height - thickness * 2, color);
  rect(x + width - thickness, y + thickness, thickness, height - thickness * 2, color);
}

static void ring(int center_x, int center_y, int outer_radius, int thickness, uint16_t color) {
  const int outer = outer_radius * outer_radius;
  const int inner_radius = outer_radius - thickness;
  const int inner = inner_radius * inner_radius;
  for (int y = -outer_radius; y <= outer_radius; ++y) {
    for (int x = -outer_radius; x <= outer_radius; ++x) {
      int distance = x * x + y * y;
      if (distance <= outer && distance >= inner) pixel(center_x + x, center_y + y, color);
    }
  }
}

static void quipus_mark(int center_x, int top, uint16_t color) {
  // Static connected threads. Draw only when the screen state changes.
  const int ends_x[] = {82, 76, 64, 46, 24};
  const int ends_y[] = {0, 22, 44, 62, 78};
  for (int side = -1; side <= 1; side += 2) {
    for (int thread = 0; thread < 5; ++thread) {
      for (int step = 0; step <= 160; ++step) {
        float t = step / 160.0f, u = 1.0f - t;
        int x = center_x + side * (int)(3*u*u*t*18 + 3*u*t*t*(ends_x[thread]-20) + t*t*t*ends_x[thread]);
        int y = top + (int)(u*u*u*80 + 3*u*u*t*52 + 3*u*t*t*ends_y[thread] + t*t*t*ends_y[thread]);
        rect(x, y, 2, 2, color);
      }
    }
  }
  ring(center_x, top + 83, 5, 2, color);
  rect(center_x, top + 87, 2, 10, color);
}

static const uint8_t *glyph(char raw) {
  static const uint8_t space[5] = {0, 0, 0, 0, 0};
  static const uint8_t dash[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
  static const uint8_t dot[5] = {0, 0x60, 0x60, 0, 0};
  static const uint8_t colon[5] = {0, 0x36, 0x36, 0, 0};
  static const uint8_t slash[5] = {0x20, 0x10, 0x08, 0x04, 0x02};
  static const uint8_t percent[5] = {0x63, 0x13, 0x08, 0x64, 0x63};
  static const uint8_t underscore[5] = {0x40, 0x40, 0x40, 0x40, 0x40};
  static const uint8_t bang[5] = {0x00, 0x00, 0x5f, 0x00, 0x00};
  static const uint8_t at[5] = {0x3e, 0x41, 0x5d, 0x55, 0x1e};
  static const uint8_t star[5] = {0x14, 0x08, 0x3e, 0x08, 0x14};
  static const uint8_t digits[10][5] = {
    {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0x00, 0x42, 0x7f, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4b, 0x31},
    {0x18, 0x14, 0x12, 0x7f, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3c, 0x4a, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1e},
  };
  static const uint8_t letters[26][5] = {
    {0x7e, 0x11, 0x11, 0x11, 0x7e}, {0x7f, 0x49, 0x49, 0x49, 0x36},
    {0x3e, 0x41, 0x41, 0x41, 0x22}, {0x7f, 0x41, 0x41, 0x22, 0x1c},
    {0x7f, 0x49, 0x49, 0x49, 0x41}, {0x7f, 0x09, 0x09, 0x09, 0x01},
    {0x3e, 0x41, 0x49, 0x49, 0x7a}, {0x7f, 0x08, 0x08, 0x08, 0x7f},
    {0x00, 0x41, 0x7f, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3f, 0x01},
    {0x7f, 0x08, 0x14, 0x22, 0x41}, {0x7f, 0x40, 0x40, 0x40, 0x40},
    {0x7f, 0x02, 0x0c, 0x02, 0x7f}, {0x7f, 0x04, 0x08, 0x10, 0x7f},
    {0x3e, 0x41, 0x41, 0x41, 0x3e}, {0x7f, 0x09, 0x09, 0x09, 0x06},
    {0x3e, 0x41, 0x51, 0x21, 0x5e}, {0x7f, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7f, 0x01, 0x01},
    {0x3f, 0x40, 0x40, 0x40, 0x3f}, {0x1f, 0x20, 0x40, 0x20, 0x1f},
    {0x3f, 0x40, 0x38, 0x40, 0x3f}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
  };
  static const uint8_t lower[26][5] = {
    {0x20,0x54,0x54,0x54,0x78},{0x7f,0x48,0x44,0x44,0x38},
    {0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7f},
    {0x38,0x54,0x54,0x54,0x18},{0x08,0x7e,0x09,0x01,0x02},
    {0x0c,0x52,0x52,0x52,0x3e},{0x7f,0x08,0x04,0x04,0x78},
    {0x00,0x44,0x7d,0x40,0x00},{0x20,0x40,0x44,0x3d,0x00},
    {0x7f,0x10,0x28,0x44,0x00},{0x00,0x41,0x7f,0x40,0x00},
    {0x7c,0x04,0x18,0x04,0x78},{0x7c,0x08,0x04,0x04,0x78},
    {0x38,0x44,0x44,0x44,0x38},{0x7c,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7c},{0x7c,0x08,0x04,0x04,0x08},
    {0x48,0x54,0x54,0x54,0x20},{0x04,0x3f,0x44,0x40,0x20},
    {0x3c,0x40,0x40,0x20,0x7c},{0x1c,0x20,0x40,0x20,0x1c},
    {0x3c,0x40,0x30,0x40,0x3c},{0x44,0x28,0x10,0x28,0x44},
    {0x0c,0x50,0x50,0x50,0x3c},{0x44,0x64,0x54,0x4c,0x44}
  };
  static const struct { unsigned char code; uint8_t pixels[5]; } punctuation[] = {
    {34,{0,7,0,7,0}},{35,{0x14,0x7f,0x14,0x7f,0x14}},
    {36,{0x24,0x2a,0x7f,0x2a,0x12}},{38,{0x36,0x49,0x55,0x22,0x50}},
    {39,{0,5,3,0,0}},{40,{0,0x1c,0x22,0x41,0}},
    {41,{0,0x41,0x22,0x1c,0}},{43,{8,8,0x3e,8,8}},
    {44,{0,0x50,0x30,0,0}},{59,{0,0x56,0x36,0,0}},
    {60,{8,0x14,0x22,0x41,0}},{61,{0x14,0x14,0x14,0x14,0x14}},
    {62,{0,0x41,0x22,0x14,8}},{63,{2,1,0x51,9,6}},
    {91,{0,0x7f,0x41,0x41,0}},{92,{2,4,8,0x10,0x20}},
    {93,{0,0x41,0x41,0x7f,0}},{94,{4,2,1,2,4}},
    {96,{0,1,2,4,0}},{123,{0,8,0x36,0x41,0}},
    {124,{0,0,0x7f,0,0}},{125,{0,0x41,0x36,8,0}},
    {126,{8,4,8,0x10,8}}
  };
  char value = raw;
  if (value >= 'a' && value <= 'z') return lower[value - 'a'];
  if (value >= 'A' && value <= 'Z') return letters[value - 'A'];
  if (value >= '0' && value <= '9') return digits[value - '0'];
  if (value == '-') return dash;
  if (value == '.') return dot;
  if (value == ':') return colon;
  if (value == '/') return slash;
  if (value == '%') return percent;
  if (value == '_') return underscore;
  if (value == '!') return bang;
  if (value == '@') return at;
  if (value == '*') return star;
  for (size_t i = 0; i < sizeof(punctuation) / sizeof(punctuation[0]); ++i)
    if ((unsigned char)value == punctuation[i].code) return punctuation[i].pixels;
  return space;
}

static int text_width(const char *text, int scale) {
  return text ? (int)strlen(text) * 6 * scale - scale : 0;
}

static void draw_text(int x, int y, const char *text, int scale, uint16_t color) {
  if (!text) return;
  for (; *text; ++text) {
    const uint8_t *columns = glyph(*text);
    for (int column = 0; column < 5; ++column) {
      for (int row = 0; row < 7; ++row) {
        if (columns[column] & (1U << row)) rect(x + column * scale, y + row * scale, scale, scale, color);
      }
    }
    x += 6 * scale;
  }
}

static void centered_text(int y, const char *text, int scale, uint16_t color) {
  int x = (LANTERN_DISPLAY_WIDTH - text_width(text, scale)) / 2;
  draw_text(x < 0 ? 0 : x, y, text, scale, color);
}

static void wrapped_text(int x, int y, int width, const char *text, int max_lines,
                         uint16_t color) {
  if (!text || !text[0] || width < 12) return;
  const int capacity = width / 6;
  char line[48];
  const char *cursor = text;
  for (int row = 0; row < max_lines && *cursor; ++row) {
    while (*cursor == ' ') ++cursor;
    size_t length = strlen(cursor);
    if (length > (size_t)capacity) {
      length = (size_t)capacity;
      while (length > 4 && cursor[length] != ' ') --length;
      if (length <= 4) length = (size_t)capacity;
    }
    if (length >= sizeof(line)) length = sizeof(line) - 1;
    memcpy(line, cursor, length);
    line[length] = '\0';
    draw_text(x, y + row * 12, line, 1, color);
    cursor += length;
  }
}

esp_err_t lantern_display_init(void) {
  gpio_config_t backlight = {
    .pin_bit_mask = 1ULL << LANTERN_DISPLAY_BACKLIGHT_GPIO,
    .mode = GPIO_MODE_OUTPUT,
  };
  ESP_ERROR_CHECK(gpio_config(&backlight));
  gpio_set_level(LANTERN_DISPLAY_BACKLIGHT_GPIO, 0);

  spi_bus_config_t bus = {
    .mosi_io_num = LANTERN_DISPLAY_MOSI_GPIO,
    .miso_io_num = LANTERN_DISPLAY_MISO_GPIO,
    .sclk_io_num = LANTERN_DISPLAY_SCLK_GPIO,
    .quadwp_io_num = GPIO_NUM_NC,
    .quadhd_io_num = GPIO_NUM_NC,
    .max_transfer_sz = LANTERN_DISPLAY_WIDTH * LANTERN_DISPLAY_HEIGHT * sizeof(uint16_t),
  };
  ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "SPI init failed");

  esp_lcd_panel_io_spi_config_t io = {
    .cs_gpio_num = LANTERN_DISPLAY_CS_GPIO,
    .dc_gpio_num = LANTERN_DISPLAY_DC_GPIO,
#if LANTERN_DISPLAY_DRIVER_ILI9341
    .spi_mode = 0,
#else
    .spi_mode = 3,
#endif
    .pclk_hz = 40 * 1000 * 1000,
    .trans_queue_depth = 1,
    .on_color_trans_done = transfer_finished,
    .lcd_cmd_bits = 8,
    .lcd_param_bits = 8,
  };
  ESP_RETURN_ON_ERROR(
    esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST, &io, &s_panel_io),
    TAG,
    "panel IO failed");

#if LANTERN_DISPLAY_DRIVER_ST7789
  esp_lcd_panel_dev_config_t panel_config = {
    .reset_gpio_num = LANTERN_DISPLAY_RESET_GPIO,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
    // The framebuffer contains native ESP32 little-endian RGB565 words. Without
    // this RAMCTRL setting, the ST7789 interprets Lantern green as purple.
    .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
    .bits_per_pixel = 16,
  };
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_panel_io, &panel_config, &s_panel), TAG, "panel failed");
  ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, false));
  ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, false));
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
#elif LANTERN_DISPLAY_DRIVER_ILI9341
  ESP_RETURN_ON_ERROR(ili9341_init(), TAG, "ILI9341 init failed");
#endif

  s_pixels = heap_caps_malloc(
    LANTERN_DISPLAY_WIDTH * LANTERN_DISPLAY_HEIGHT * sizeof(uint16_t),
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_pixels) {
    s_pixels = heap_caps_malloc(
      LANTERN_DISPLAY_WIDTH * LANTERN_DISPLAY_HEIGHT * sizeof(uint16_t),
      MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  }
  if (!s_pixels) return ESP_ERR_NO_MEM;
  s_transfer_pixels = heap_caps_malloc(
    LANTERN_DISPLAY_WIDTH * TRANSFER_ROWS * sizeof(uint16_t),
    MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  if (!s_transfer_pixels) return ESP_ERR_NO_MEM;
  s_transfer_done = xSemaphoreCreateBinary();
  s_updates = xQueueCreate(1, sizeof(display_update_t));
  if (!s_transfer_done || !s_updates) return ESP_ERR_NO_MEM;
  if (xTaskCreate(display_task, "lantern_display", 4096, NULL, 2, NULL) != pdPASS) {
    vQueueDelete(s_updates);
    s_updates = NULL;
    return ESP_ERR_NO_MEM;
  }
  gpio_set_level(LANTERN_DISPLAY_BACKLIGHT_GPIO, 1);
  lantern_display_show(LANTERN_SCREEN_BOOTING, "HARDWARE CHECK");
  ESP_LOGI(TAG, "%s ready at %dx%d",
    LANTERN_DISPLAY_DRIVER_ILI9341 ? "ILI9341" : "ST7789",
    LANTERN_DISPLAY_WIDTH, LANTERN_DISPLAY_HEIGHT);
  return ESP_OK;
}

static void compact_submit(display_update_t *update) {
  gpio_set_level(LANTERN_DISPLAY_BACKLIGHT_GPIO, 1);
  taskENTER_CRITICAL(&s_update_lock);
  update->revision = s_requested.revision;
  if (!memcmp(&s_requested, update, sizeof(*update))) {
    taskEXIT_CRITICAL(&s_update_lock);
    return;
  }
  ++update->revision;
  s_requested = *update;
  taskEXIT_CRITICAL(&s_update_lock);
  if (s_updates) xQueueOverwrite(s_updates, update);
}

void lantern_display_show_face(const lantern_face_t *face) {
  if (!face) return;
  display_update_t update = {0};
  update.view = DISPLAY_FACE;
  update.face = *face;
  if (update.face.eye_open > 100) update.face.eye_open = 100;
  if (update.face.mouth_open > 100) update.face.mouth_open = 100;
  compact_submit(&update);
}

void lantern_display_show_compact_menu(const char *title, const char *const *items,
    size_t count, size_t selected, const char *footer) {
  display_update_t update = {0};
  update.view = DISPLAY_COMPACT_MENU;
  update.item_count = count > LANTERN_DISPLAY_MENU_ITEMS ? LANTERN_DISPLAY_MENU_ITEMS : count;
  update.selected = selected < update.item_count ? (unsigned)selected : 0;
  snprintf(update.title, sizeof(update.title), "%s", title ? title : "");
  snprintf(update.detail, sizeof(update.detail), "%s", footer ? footer : "");
  for (size_t i = 0; i < update.item_count; ++i)
    snprintf(update.items[i], sizeof(update.items[i]), "%s", items && items[i] ? items[i] : "");
  compact_submit(&update);
}

void lantern_display_show_input(const char *title, const char *context,
    const char *preview, const char *selected, unsigned length, unsigned limit,
    const char *hint) {
  display_update_t update = {0};
  update.view = DISPLAY_INPUT;
  snprintf(update.title, sizeof(update.title), "%s", title ? title : "");
  snprintf(update.detail, sizeof(update.detail), "%s", context ? context : "");
  snprintf(update.input, sizeof(update.input), "%s", preview ? preview : "");
  snprintf(update.items[0], sizeof(update.items[0]), "%s", selected ? selected : "");
  snprintf(update.items[1], sizeof(update.items[1]), "%s", hint ? hint : "");
  update.input_length = length; update.input_limit = limit;
  compact_submit(&update);
}

void lantern_display_show_volume(uint8_t volume) {
  display_update_t update = {0};
  update.view = DISPLAY_VOLUME;
  update.volume = volume > 100 ? 100 : volume;
  compact_submit(&update);
}

void lantern_display_show(lantern_screen_t screen, const char *detail) {
  gpio_set_level(LANTERN_DISPLAY_BACKLIGHT_GPIO, 1);
  char copied_detail[sizeof(s_requested.detail)] = {0};
  snprintf(copied_detail, sizeof(copied_detail), "%s", detail ? detail : "");
  display_update_t update;
  taskENTER_CRITICAL(&s_update_lock);
  s_requested.view = DISPLAY_STATE;
  s_requested.screen = screen;
  memcpy(s_requested.detail, copied_detail, sizeof(s_requested.detail));
  update = s_requested;
  taskEXIT_CRITICAL(&s_update_lock);
  if (s_updates) xQueueOverwrite(s_updates, &update);
}

void lantern_display_show_home(const char *footer) {
  gpio_set_level(LANTERN_DISPLAY_BACKLIGHT_GPIO, 1);
  display_update_t update;
  taskENTER_CRITICAL(&s_update_lock);
  s_requested.view = DISPLAY_HOME;
  ++s_requested.revision;
  s_requested.screen = LANTERN_SCREEN_READY;
  snprintf(s_requested.detail, sizeof(s_requested.detail), "%s", footer ? footer : "");
  update = s_requested;
  taskEXIT_CRITICAL(&s_update_lock);
  if (s_updates) xQueueOverwrite(s_updates, &update);
}

void lantern_display_show_splash(void) {
  gpio_set_level(LANTERN_DISPLAY_BACKLIGHT_GPIO, 1);
  display_update_t update;
  taskENTER_CRITICAL(&s_update_lock);
  s_requested.view = DISPLAY_SPLASH;
  s_requested.screen = LANTERN_SCREEN_READY;
  s_requested.detail[0] = '\0';
  ++s_requested.revision;
  update = s_requested;
  taskEXIT_CRITICAL(&s_update_lock);
  if (s_updates) xQueueOverwrite(s_updates, &update);
}

void lantern_display_show_menu(const char *title, const char *const *items,
                               size_t item_count, const char *footer) {
  gpio_set_level(LANTERN_DISPLAY_BACKLIGHT_GPIO, 1);
  display_update_t update;
  if (item_count > LANTERN_DISPLAY_MENU_ITEMS) item_count = LANTERN_DISPLAY_MENU_ITEMS;
  taskENTER_CRITICAL(&s_update_lock);
  s_requested.view = DISPLAY_MENU;
  ++s_requested.revision;
  snprintf(s_requested.title, sizeof(s_requested.title), "%s", title ? title : "");
  snprintf(s_requested.detail, sizeof(s_requested.detail), "%s", footer ? footer : "");
  s_requested.item_count = item_count;
  for (size_t index = 0; index < LANTERN_DISPLAY_MENU_ITEMS; ++index) {
    snprintf(s_requested.items[index], sizeof(s_requested.items[index]), "%s",
      index < item_count && items && items[index] ? items[index] : "");
  }
  update = s_requested;
  taskEXIT_CRITICAL(&s_update_lock);
  if (s_updates) xQueueOverwrite(s_updates, &update);
}

void lantern_display_show_keyboard(const char *ssid, const char *password, bool uppercase) {
  gpio_set_level(LANTERN_DISPLAY_BACKLIGHT_GPIO, 1);
  display_update_t update;
  taskENTER_CRITICAL(&s_update_lock);
  s_requested.view = DISPLAY_KEYBOARD;
  ++s_requested.revision;
  snprintf(s_requested.title, sizeof(s_requested.title), "%s", ssid ? ssid : "WIFI");
  snprintf(s_requested.input, sizeof(s_requested.input), "%s", password ? password : "");
  s_requested.uppercase = uppercase;
  update = s_requested;
  taskEXIT_CRITICAL(&s_update_lock);
  if (s_updates) xQueueOverwrite(s_updates, &update);
}

void lantern_display_show_detail(const char *title, const char *body, const char *footer) {
  gpio_set_level(LANTERN_DISPLAY_BACKLIGHT_GPIO, 1);
  display_update_t update;
  taskENTER_CRITICAL(&s_update_lock);
  s_requested.view = DISPLAY_DETAIL;
  ++s_requested.revision;
  snprintf(s_requested.title, sizeof(s_requested.title), "%s", title ? title : "");
  snprintf(s_requested.detail, sizeof(s_requested.detail), "%s", body ? body : "");
  snprintf(s_requested.input, sizeof(s_requested.input), "%s", footer ? footer : "");
  update = s_requested;
  taskEXIT_CRITICAL(&s_update_lock);
  if (s_updates) xQueueOverwrite(s_updates, &update);
}

void lantern_display_set_awake(bool awake) {
  gpio_set_level(LANTERN_DISPLAY_BACKLIGHT_GPIO, awake ? 1 : 0);
}

void lantern_display_set_charge_eta(int minutes) {
  display_update_t update;
  if (minutes < 0) minutes = -1;
  if (minutes > 5999) minutes = 5999;
  taskENTER_CRITICAL(&s_update_lock);
  if (s_requested.charge_eta_minutes == minutes) {
    taskEXIT_CRITICAL(&s_update_lock);
    return;
  }
  s_requested.charge_eta_minutes = minutes;
  // The estimate is drawn outside the status bar on both idle views. Force a
  // full redraw so unplugging USB clears the previous text immediately.
  if (s_requested.view == DISPLAY_HOME || s_requested.view == DISPLAY_SPLASH) {
    ++s_requested.revision;
  }
  update = s_requested;
  taskEXIT_CRITICAL(&s_update_lock);
  if (s_updates) xQueueOverwrite(s_updates, &update);
}

void lantern_display_set_status(
    int battery_level,
    lantern_power_state_t power_state,
    lantern_connectivity_t connectivity,
    int wifi_rssi,
    bool paired) {
  if (battery_level > 100) battery_level = 100;
  if (battery_level < 0) battery_level = -1;
  if (wifi_rssi > 0) wifi_rssi = 0;
  if (wifi_rssi < -127) wifi_rssi = -127;

  display_update_t update;
  bool changed;
  taskENTER_CRITICAL(&s_update_lock);
  changed = s_requested.battery_level != battery_level ||
    s_requested.power_state != power_state ||
    s_requested.connectivity != connectivity ||
    s_requested.wifi_rssi != wifi_rssi ||
    s_requested.paired != paired;
  s_requested.battery_level = battery_level;
  s_requested.power_state = power_state;
  s_requested.connectivity = connectivity;
  s_requested.wifi_rssi = wifi_rssi;
  s_requested.paired = paired;
  update = s_requested;
  taskEXIT_CRITICAL(&s_update_lock);
  if (changed && s_updates) xQueueOverwrite(s_updates, &update);
}

static void draw_status_bar(
    int battery_level,
    lantern_power_state_t power_state,
    lantern_connectivity_t connectivity,
    int wifi_rssi,
    bool paired,
    uint16_t background,
    uint16_t foreground,
    uint16_t green,
    uint16_t amber,
    uint16_t red) {
  const uint16_t dim = rgb565(66, 78, 71);
  rect(0, 0, LANTERN_DISPLAY_WIDTH, 29, background);
  rect(0, 28, LANTERN_DISPLAY_WIDTH, 1, dim);

  int bars = 0;
  bool wifi_connected = connectivity >= LANTERN_CONNECTIVITY_WIFI_ONLY;
  if (wifi_connected) {
    bars = wifi_rssi >= -60 ? 3 : wifi_rssi >= -72 ? 2 : 1;
  }
  uint16_t signal_color = !wifi_connected ? red :
    (connectivity == LANTERN_CONNECTIVITY_WEAK || bars == 1) ? amber : green;
  const int heights[] = {4, 8, 12};
  for (int index = 0; index < 3; ++index) {
    rect(7 + index * 6, 21 - heights[index], 3, heights[index],
      index < bars ? signal_color : dim);
  }
  const char *connection = "OFFLINE";
  if (connectivity == LANTERN_CONNECTIVITY_RECONNECTING) connection = "LINKING";
  else if (connectivity == LANTERN_CONNECTIVITY_SETUP) connection = "SETUP";
  else if (connectivity == LANTERN_CONNECTIVITY_WIFI_ONLY) connection = "WIFI";
  else if (connectivity == LANTERN_CONNECTIVITY_WEAK) connection = "WEAK";
  else if (connectivity == LANTERN_CONNECTIVITY_ONLINE) connection = paired ? "ONLINE" : "WIFI";
  draw_text(28, 9, connection, 1, wifi_connected ? signal_color : red);

  char battery[16];
  if (battery_level >= 0) snprintf(battery, sizeof(battery), "%d%%", battery_level);
  else snprintf(battery, sizeof(battery), "--%%");
  uint16_t battery_color = battery_level < 0 ? foreground :
    battery_level <= 15 ? red : battery_level <= 30 ? amber : green;
  const int battery_x = LANTERN_DISPLAY_WIDTH - 27;
  const int label_x = battery_x - text_width(battery, 1) - 7;
  draw_text(label_x, 9, battery, 1, battery_color);
  const char *power_label = power_state == LANTERN_POWER_CHARGING ? "CHG" :
    power_state == LANTERN_POWER_BATTERY ? "BAT" : "PWR";
  const uint16_t power_color = power_state == LANTERN_POWER_CHARGING ? amber : dim;
  const int power_x = label_x - text_width(power_label, 1) - 7;
  draw_text(power_x, 9, power_label, 1, power_color);
  outline_rect(battery_x, 7, 20, 14, 2, battery_color);
  rect(battery_x + 20, 11, 3, 6, battery_color);
  if (battery_level > 0) {
    int fill_width = (battery_level * 14 + 99) / 100;
    if (fill_width < 1) fill_width = 1;
    rect(battery_x + 3, 10, fill_width, 8, battery_color);
  }
}

static bool flush_rows(int first_row, int end_row) {
  if (first_row < 0) first_row = 0;
  if (end_row > LANTERN_DISPLAY_HEIGHT) end_row = LANTERN_DISPLAY_HEIGHT;
  first_row = (first_row / TRANSFER_ROWS) * TRANSFER_ROWS;
  end_row = ((end_row + TRANSFER_ROWS - 1) / TRANSFER_ROWS) * TRANSFER_ROWS;
  if (end_row > LANTERN_DISPLAY_HEIGHT) end_row = LANTERN_DISPLAY_HEIGHT;

  while (xSemaphoreTake(s_transfer_done, 0) == pdTRUE) {}
  for (int y = first_row; y < end_row; y += TRANSFER_ROWS) {
    int rows = end_row - y;
    if (rows > TRANSFER_ROWS) rows = TRANSFER_ROWS;
    const size_t pixel_count = (size_t)rows * LANTERN_DISPLAY_WIDTH;
#if LANTERN_DISPLAY_DRIVER_ILI9341
    // The manufacturer BSP uses big-endian RGB565 transfers for this panel.
    for (size_t index = 0; index < pixel_count; ++index) {
      uint16_t value = s_pixels[y * LANTERN_DISPLAY_WIDTH + index];
      s_transfer_pixels[index] = (uint16_t)((value << 8) | (value >> 8));
    }
    esp_err_t result = ili9341_draw_bitmap(y, rows, s_transfer_pixels);
#else
    memcpy(s_transfer_pixels, s_pixels + y * LANTERN_DISPLAY_WIDTH,
      pixel_count * sizeof(uint16_t));
    esp_err_t result = esp_lcd_panel_draw_bitmap(
      s_panel, 0, y, LANTERN_DISPLAY_WIDTH, y + rows, s_transfer_pixels);
#endif
    if (result != ESP_OK) {
      ESP_LOGE(TAG, "draw failed: %s", esp_err_to_name(result));
      return false;
    }
    if (xSemaphoreTake(s_transfer_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
      ESP_LOGE(TAG, "draw timed out");
      return false;
    }
  }
  return true;
}

static void ellipse(int cx, int cy, int rx, int ry, uint16_t color) {
  for (int y = -ry; y <= ry; ++y)
    for (int x = -rx; x <= rx; ++x)
      if (x*x*ry*ry + y*y*rx*rx <= rx*rx*ry*ry) pixel(cx+x, cy+y, color);
}

static void microphone(int x, int y, bool crossed, uint16_t ink, uint16_t background) {
  rect(x+4, y, 4, 10, ink);
  rect(x+1, y+6, 2, 6, ink); rect(x+9, y+6, 2, 6, ink);
  rect(x+2, y+12, 8, 2, ink); rect(x+5, y+14, 2, 3, ink);
  if (crossed) {
    for (int i = 0; i < 16; ++i) {
      rect(x-2+i, y+15-i, 3, 3, background);
      rect(x-1+i, y+15-i, 1, 2, ink);
    }
  }
}

static void render_compact(const display_update_t *update) {
  const uint16_t mint = rgb565(137, 216, 178);
  const uint16_t ink = rgb565(24, 58, 46);
  const uint16_t quiet = rgb565(70, 133, 104);
  fill(mint);
  if (update->view == DISPLAY_FACE) {
    const lantern_face_t *face = &update->face;
    int eye_height = 1 + face->eye_open * 6 / 100;
    /* Exactly two dot eyes and one straight mouth. Blinks flatten the dots;
     * speech opens the same mouth according to played PCM amplitude. */
    if (face->eye_open < 15) {
      rect(69, 93, 15, 2, ink); rect(157, 93, 15, 2, ink);
    } else {
      ellipse(76, 94, 7, eye_height, ink);
      ellipse(164, 94, 7, eye_height, ink);
    }
    int mouth_height = face->mouth_open ? 4 + face->mouth_open * 22 / 100 : 3;
    rect(94, 149 - mouth_height / 2, 52, mouth_height, ink);
    if (face->muted) microphone(212, 13, true, ink, mint);
    else if (face->listening) microphone(13, 13, false, quiet, mint);
    if (face->thinking) { rect(14, 219, 3, 3, quiet); rect(20, 219, 3, 3, quiet); rect(26, 219, 3, 3, quiet); }
    if (!face->configured) centered_text(224, "Hold centre for settings", 1, quiet);
    else if (!face->wifi_connected) centered_text(224, "Wi-Fi disconnected", 1, quiet);
  } else if (update->view == DISPLAY_COMPACT_MENU) {
    centered_text(19, update->title, strlen(update->title) > 17 ? 1 : 2, ink);
    for (size_t i = 0; i < update->item_count; ++i) {
      int y = 52 + (int)i * 31;
      if (i == update->selected) rect(10, y, 220, 26, ink);
      char label[36];
      snprintf(label, sizeof(label), "%.35s", update->items[i]);
      draw_text(18, y+9, label, 1, i == update->selected ? mint : ink);
    }
    centered_text(225, update->detail, 1, quiet);
  } else if (update->view == DISPLAY_INPUT) {
    centered_text(17, update->title, 2, ink);
    centered_text(49, update->detail, 1, quiet);
    centered_text(77, update->input[0] ? update->input : "_", 1, ink);
    outline_rect(77, 105, 86, 64, 2, ink);
    int scale = strlen(update->items[0]) == 1 ? 5 : 2;
    centered_text(126, update->items[0], scale, ink);
    draw_text(48, 133, "<", 2, quiet); draw_text(181, 133, ">", 2, quiet);
    char count[24];
    snprintf(count, sizeof(count), "%u / %u", update->input_length, update->input_limit);
    centered_text(185, count, 1, quiet);
    centered_text(208, update->items[1], 1, ink);
    centered_text(227, "UP/DOWN chooses character", 1, quiet);
  } else if (update->view == DISPLAY_VOLUME) {
    centered_text(24, "Volume", 2, ink);
    char value[16];
    snprintf(value, sizeof(value), "%u%%", update->volume);
    centered_text(91, value, 4, ink);
    outline_rect(25, 148, 190, 16, 2, ink);
    rect(29, 152, (int)update->volume * 182 / 100, 8, ink);
    centered_text(205, "UP/DOWN changes volume", 1, quiet);
    centered_text(225, "Centre: back", 1, quiet);
  }
  (void)flush_rows(0, LANTERN_DISPLAY_HEIGHT);
}

static void render_screen(const display_update_t *update) {
  static uint32_t previous_revision;
  static int previous_view = -1;
  static int previous_screen = -1;
  static char previous_detail[96];
  static int previous_battery = -2;
  static lantern_power_state_t previous_power = (lantern_power_state_t)-1;
  static int previous_rssi = 1;
  static lantern_connectivity_t previous_connectivity = (lantern_connectivity_t)-1;
  static bool previous_paired;
  if (update->view >= DISPLAY_FACE) {
    render_compact(update);
    previous_view = -1; previous_screen = -1;
    return;
  }
  const lantern_screen_t screen = update->screen;
  const char *detail = update->detail;
  const bool view_changed = (int)update->view != previous_view;
  const bool screen_changed = (int)screen != previous_screen || view_changed ||
    update->revision != previous_revision;
  const bool detail_changed = strcmp(detail, previous_detail) != 0;
  const bool status_changed = update->battery_level != previous_battery ||
    update->power_state != previous_power ||
    update->wifi_rssi != previous_rssi ||
    update->connectivity != previous_connectivity ||
    update->paired != previous_paired;
  if (!screen_changed && !detail_changed && !status_changed) return;

  const uint16_t background = rgb565(17, 25, 22);
  const uint16_t green = rgb565(181, 213, 177);
  const uint16_t foreground = rgb565(233, 238, 231);
  const uint16_t amber = rgb565(255, 184, 72);
  const uint16_t red = rgb565(255, 78, 86);
  uint16_t accent = green;
  const char *title = "QUIPUS";
  const char *status = "BOOTING";

  switch (screen) {
    case LANTERN_SCREEN_SETUP: status = "WIFI SETUP"; accent = amber; break;
    case LANTERN_SCREEN_CONNECTING: status = "CONNECTING"; accent = amber; break;
    case LANTERN_SCREEN_READY: status = "READY"; break;
    case LANTERN_SCREEN_LISTENING: status = "LISTENING"; break;
    case LANTERN_SCREEN_UNDERSTANDING: status = "UNDERSTANDING"; accent = amber; break;
    case LANTERN_SCREEN_CONSENT: status = "CONSENT"; accent = amber; break;
    case LANTERN_SCREEN_RECORDING: status = "RECORDING"; accent = red; break;
    case LANTERN_SCREEN_SAVING: status = "SAVING"; accent = amber; break;
    case LANTERN_SCREEN_COMPLETE: status = "SESSION COMPLETE"; break;
    case LANTERN_SCREEN_STATUS: status = "STATUS REPORT"; break;
    case LANTERN_SCREEN_ERROR: status = "ERROR"; accent = red; break;
    case LANTERN_SCREEN_BOOTING: default: break;
  }

  const int ring_y = LANTERN_DISPLAY_HEIGHT > 240 ? 130 : 108;
  const int status_y = LANTERN_DISPLAY_HEIGHT > 240 ? 220 : 162;
  const int detail_y = LANTERN_DISPLAY_HEIGHT > 240 ? 274 : 214;
  bool complete = true;
  if (!screen_changed) {
    // Status and progress strips are independent, avoiding a full-screen DMA
    // transfer every second while a meeting timer is running.
    if (status_changed) {
      draw_status_bar(update->battery_level, update->power_state,
        update->connectivity, update->wifi_rssi,
        update->paired, background, foreground, green, amber, red);
      complete = flush_rows(0, 29) && complete;
    }
    if (detail_changed) {
      rect(0, detail_y, LANTERN_DISPLAY_WIDTH, 7, background);
      if (detail && detail[0]) centered_text(detail_y, detail, 1, foreground);
      complete = flush_rows(detail_y, detail_y + 7) && complete;
    }
  } else {
    fill(background);
    draw_status_bar(update->battery_level, update->power_state,
      update->connectivity, update->wifi_rssi,
      update->paired, background, foreground, green, amber, red);

    if (update->view == DISPLAY_SPLASH) {
      centered_text(45, "QUIPUS", 2, foreground);
      quipus_mark(LANTERN_DISPLAY_WIDTH / 2, 79, green);
      centered_text(217, "TAP TO OPEN", 2, foreground);
      centered_text(249, "SAY COMPUTER FOR VOICE", 1, green);
      if (update->power_state == LANTERN_POWER_CHARGING && update->charge_eta_minutes >= 0) {
        char charge[40];
        if (update->charge_eta_minutes == 0) snprintf(charge, sizeof(charge), "CHARGED");
        else if (update->charge_eta_minutes >= 60) snprintf(charge, sizeof(charge), "FULL IN ~%dH %02dM",
          update->charge_eta_minutes / 60, update->charge_eta_minutes % 60);
        else snprintf(charge, sizeof(charge), "FULL IN ~%d MIN", update->charge_eta_minutes);
        centered_text(284, charge, 1, amber);
      }
    } else if (update->view == DISPLAY_HOME) {
      centered_text(42, "MAIN MENU", 2, foreground);
      const char *labels[] = {"START SESSION", "STATUS REPORT", "WIFI SETUP"};
      for (int index = 0; index < 3; ++index) {
        int y = 78 + index * 46;
        outline_rect(22, y, LANTERN_DISPLAY_WIDTH - 44, 34, 2, index == 0 ? green : foreground);
        centered_text(y + 13, labels[index], 1, index == 0 ? green : foreground);
      }
      if (update->power_state == LANTERN_POWER_CHARGING && update->charge_eta_minutes >= 0) {
        char charge[40];
        if (update->charge_eta_minutes == 0) snprintf(charge, sizeof(charge), "CHARGED");
        else if (update->charge_eta_minutes >= 60) snprintf(charge, sizeof(charge), "FULL IN ~%dH %02dM",
          update->charge_eta_minutes / 60, update->charge_eta_minutes % 60);
        else snprintf(charge, sizeof(charge), "FULL IN ~%d MIN", update->charge_eta_minutes);
        centered_text(224, charge, 1, amber);
      }
      centered_text(247, "SAY COMPUTER FOR VOICE", 1, green);
      outline_rect(72, 276, 96, 32, 1, foreground);
      centered_text(288, "BACK", 1, foreground);
    } else if (update->view == DISPLAY_MENU) {
      centered_text(38, update->title, 2, foreground);
      for (size_t index = 0; index < update->item_count; ++index) {
        int y = 68 + (int)index * 38;
        outline_rect(12, y, LANTERN_DISPLAY_WIDTH - 24, 31, 1, index == 0 ? green : foreground);
        draw_text(18, y + 12, update->items[index], 1, index == 0 ? green : foreground);
      }
      outline_rect(72, 276, 96, 32, 1, foreground);
      centered_text(288, "BACK", 1, foreground);
    } else if (update->view == DISPLAY_KEYBOARD) {
      centered_text(36, "WIFI PASSWORD", 1, foreground);
      wrapped_text(8, 51, LANTERN_DISPLAY_WIDTH - 16, update->title, 1, green);
      char masked[33] = {0};
      size_t password_length = strlen(update->input);
      size_t shown = password_length < sizeof(masked) - 1 ? password_length : sizeof(masked) - 1;
      memset(masked, '*', shown);
      draw_text(8, 69, masked, 1, foreground);
      static const char *rows[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM._@"};
      for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 10; ++column) {
          int x = column * 24, y = 91 + row * 39;
          outline_rect(x + 1, y, 22, 34, 1, rgb565(66, 78, 71));
          char key[2] = {rows[row][column], 0};
          if (!update->uppercase) key[0] = (char)tolower((unsigned char)key[0]);
          draw_text(x + 9, y + 13, key, 1, foreground);
        }
      }
      outline_rect(3, 252, 68, 40, 1, foreground); centered_text(300, "", 1, foreground);
      draw_text(15, 269, "BACK", 1, foreground);
      outline_rect(85, 252, 68, 40, 1, foreground); draw_text(98, 269, "CASE", 1, foreground);
      outline_rect(167, 252, 70, 40, 2, green); draw_text(185, 269, "JOIN", 1, green);
    } else if (update->view == DISPLAY_DETAIL) {
      centered_text(38, update->title, 1, green);
      wrapped_text(10, 61, LANTERN_DISPLAY_WIDTH - 20, detail, 16, foreground);
      outline_rect(72, 276, 96, 32, 1, foreground);
      centered_text(288, "BACK", 1, foreground);
    } else {
      if (screen == LANTERN_SCREEN_READY || screen == LANTERN_SCREEN_BOOTING) {
        quipus_mark(LANTERN_DISPLAY_WIDTH / 2, ring_y - 44, accent);
      } else if (screen == LANTERN_SCREEN_RECORDING) {
        ring(LANTERN_DISPLAY_WIDTH / 2, ring_y, 17, 17, red);
      } else {
        // No decorative animation while listening, uploading, or reporting.
        rect(LANTERN_DISPLAY_WIDTH / 2 - 22, ring_y, 44, 2, accent);
      }
      centered_text(36, title, 2, foreground);
      centered_text(status_y, status, 2, accent);
      if (detail && detail[0]) centered_text(detail_y, detail, 1, foreground);
    }
    complete = flush_rows(0, LANTERN_DISPLAY_HEIGHT);
  }
  previous_screen = complete ? (int)screen : -1;
  if (complete) {
    previous_view = (int)update->view;
    previous_revision = update->revision;
    snprintf(previous_detail, sizeof(previous_detail), "%s", detail);
    previous_battery = update->battery_level;
    previous_power = update->power_state;
    previous_rssi = update->wifi_rssi;
    previous_connectivity = update->connectivity;
    previous_paired = update->paired;
  }
}
