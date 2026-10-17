idf_component_register(
  SRCS "app_main.c" "dot_config.c" "dot_clock.c" "dot_wifi.c"
       "dot_audio.c" "dot_mcp.c" "dot_tunnel.c" "lantern_display.c"
       "dot_ui.c" "dot_buttons.c" "dot_preferences.c" "dot_intent.c"
  INCLUDE_DIRS "."
  REQUIRES driver esp_adc esp_event esp_netif esp_wifi esp_http_client
           esp_lcd esp_timer esp-tls json lwip mbedtls nvs_flash
)
