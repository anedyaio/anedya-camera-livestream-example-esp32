idf_component_register(
  SRCS "anedya_signaling.c" "app_main.c" "webrtc_peer.c" "esp_log_compat.c"
  INCLUDE_DIRS "."
  PRIV_REQUIRES mqtt nvs_flash esp_netif esp_event esp_wifi protocol_examples_common esp_psram log
               espressif__esp32-camera anedya__anedya-esp espressif__esp_peer
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")
