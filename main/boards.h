#pragma once

/*
 * Board pin maps for esp32-camera-compatible development boards.
 * Select your board in menuconfig: Anedya WebRTC Camera → Board Selection.
 *
 * Each board defines:
 *   CAM_PIN_PWDN, CAM_PIN_RESET, CAM_PIN_XCLK
 *   CAM_PIN_SIOD, CAM_PIN_SIOC
 *   CAM_PIN_D0..D7, CAM_PIN_VSYNC, CAM_PIN_HREF, CAM_PIN_PCLK
 *   BOARD_HAS_PSRAM  — 1 if board has PSRAM, 0 if not
 *   BOARD_NAME       — human-readable string for log output
 */

/* ── AI Thinker ESP32-CAM ───────────────────────────────────────────────── */
#if defined(CONFIG_BOARD_ESP32CAM_AITHINKER)
#define BOARD_NAME       "AI Thinker ESP32-CAM"
#define BOARD_HAS_PSRAM  1
#define CAM_PIN_PWDN     32
#define CAM_PIN_RESET    -1
#define CAM_PIN_XCLK      0
#define CAM_PIN_SIOD     26
#define CAM_PIN_SIOC     27
#define CAM_PIN_D7       35
#define CAM_PIN_D6       34
#define CAM_PIN_D5       39
#define CAM_PIN_D4       36
#define CAM_PIN_D3       21
#define CAM_PIN_D2       19
#define CAM_PIN_D1       18
#define CAM_PIN_D0        5
#define CAM_PIN_VSYNC    25
#define CAM_PIN_HREF     23
#define CAM_PIN_PCLK     22

/* ── ESP-WROVER-KIT ─────────────────────────────────────────────────────── */
#elif defined(CONFIG_BOARD_WROVER_KIT)
#define BOARD_NAME       "ESP-WROVER-KIT"
#define BOARD_HAS_PSRAM  1
#define CAM_PIN_PWDN     -1
#define CAM_PIN_RESET    -1
#define CAM_PIN_XCLK     21
#define CAM_PIN_SIOD     26
#define CAM_PIN_SIOC     27
#define CAM_PIN_D7       35
#define CAM_PIN_D6       34
#define CAM_PIN_D5       39
#define CAM_PIN_D4       36
#define CAM_PIN_D3       19
#define CAM_PIN_D2       18
#define CAM_PIN_D1        5
#define CAM_PIN_D0        4
#define CAM_PIN_VSYNC    25
#define CAM_PIN_HREF     23
#define CAM_PIN_PCLK     22

/* ── Freenove ESP32-Wrover CAM ──────────────────────────────────────────── */
#elif defined(CONFIG_BOARD_FREENOVE_WROVER)
#define BOARD_NAME       "Freenove ESP32-Wrover CAM"
#define BOARD_HAS_PSRAM  1
#define CAM_PIN_PWDN     -1
#define CAM_PIN_RESET    -1
#define CAM_PIN_XCLK     21
#define CAM_PIN_SIOD     26
#define CAM_PIN_SIOC     27
#define CAM_PIN_D7       35
#define CAM_PIN_D6       34
#define CAM_PIN_D5       39
#define CAM_PIN_D4       36
#define CAM_PIN_D3       19
#define CAM_PIN_D2       18
#define CAM_PIN_D1        5
#define CAM_PIN_D0        4
#define CAM_PIN_VSYNC    25
#define CAM_PIN_HREF     23
#define CAM_PIN_PCLK     22

/* ── ESP-EYE ────────────────────────────────────────────────────────────── */
#elif defined(CONFIG_BOARD_ESP_EYE)
#define BOARD_NAME       "ESP-EYE"
#define BOARD_HAS_PSRAM  1
#define CAM_PIN_PWDN     -1
#define CAM_PIN_RESET    -1
#define CAM_PIN_XCLK      4
#define CAM_PIN_SIOD     18
#define CAM_PIN_SIOC     23
#define CAM_PIN_D7       36
#define CAM_PIN_D6       37
#define CAM_PIN_D5       38
#define CAM_PIN_D4       39
#define CAM_PIN_D3       35
#define CAM_PIN_D2       14
#define CAM_PIN_D1       13
#define CAM_PIN_D0       34
#define CAM_PIN_VSYNC     5
#define CAM_PIN_HREF     27
#define CAM_PIN_PCLK     25

/* ── ESP32-S3-EYE ───────────────────────────────────────────────────────── */
#elif defined(CONFIG_BOARD_ESP32S3_EYE)
#define BOARD_NAME       "ESP32-S3-EYE"
#define BOARD_HAS_PSRAM  1
#define CAM_PIN_PWDN     -1
#define CAM_PIN_RESET    -1
#define CAM_PIN_XCLK     15
#define CAM_PIN_SIOD      4
#define CAM_PIN_SIOC      5
#define CAM_PIN_D7       16
#define CAM_PIN_D6       17
#define CAM_PIN_D5       18
#define CAM_PIN_D4       12
#define CAM_PIN_D3       10
#define CAM_PIN_D2        8
#define CAM_PIN_D1        9
#define CAM_PIN_D0       11
#define CAM_PIN_VSYNC     6
#define CAM_PIN_HREF      7
#define CAM_PIN_PCLK     13

/* ── Seeed XIAO ESP32-S3 Sense ──────────────────────────────────────────── */
#elif defined(CONFIG_BOARD_XIAO_ESP32S3)
#define BOARD_NAME       "Seeed XIAO ESP32-S3 Sense"
#define BOARD_HAS_PSRAM  1
#define CAM_PIN_PWDN     -1
#define CAM_PIN_RESET    -1
#define CAM_PIN_XCLK     10
#define CAM_PIN_SIOD     40
#define CAM_PIN_SIOC     39
#define CAM_PIN_D7       48
#define CAM_PIN_D6       11
#define CAM_PIN_D5       12
#define CAM_PIN_D4       14
#define CAM_PIN_D3       16
#define CAM_PIN_D2       18
#define CAM_PIN_D1       17
#define CAM_PIN_D0       15
#define CAM_PIN_VSYNC    38
#define CAM_PIN_HREF     47
#define CAM_PIN_PCLK     13

/* ── DFRobot ESP32-S3 AI Camera (DFR1154) ──────────────────────────────── */
#elif defined(CONFIG_BOARD_DFROBOT_ESP32S3)
#define BOARD_NAME       "DFRobot ESP32-S3 AI Camera"
#define BOARD_HAS_PSRAM  1
#define CAM_PIN_PWDN     -1
#define CAM_PIN_RESET    -1
#define CAM_PIN_XCLK     15
#define CAM_PIN_SIOD      4
#define CAM_PIN_SIOC      5
#define CAM_PIN_D7       16
#define CAM_PIN_D6       17
#define CAM_PIN_D5       18
#define CAM_PIN_D4       12
#define CAM_PIN_D3       10
#define CAM_PIN_D2        8
#define CAM_PIN_D1        9
#define CAM_PIN_D0       11
#define CAM_PIN_VSYNC     6
#define CAM_PIN_HREF      7
#define CAM_PIN_PCLK     13

/* ── M5Stack Camera (Model A) ───────────────────────────────────────────── */
#elif defined(CONFIG_BOARD_M5STACK_CAMERA_A)
#define BOARD_NAME       "M5Stack Camera Model A"
#define BOARD_HAS_PSRAM  0
#define CAM_PIN_PWDN     -1
#define CAM_PIN_RESET    15
#define CAM_PIN_XCLK     27
#define CAM_PIN_SIOD     25
#define CAM_PIN_SIOC     23
#define CAM_PIN_D7       19
#define CAM_PIN_D6       36
#define CAM_PIN_D5       18
#define CAM_PIN_D4       39
#define CAM_PIN_D3        5
#define CAM_PIN_D2       34
#define CAM_PIN_D1       35
#define CAM_PIN_D0       32
#define CAM_PIN_VSYNC    22
#define CAM_PIN_HREF     26
#define CAM_PIN_PCLK     21

/* ── M5Stack Camera (Model B) ───────────────────────────────────────────── */
#elif defined(CONFIG_BOARD_M5STACK_CAMERA_B)
#define BOARD_NAME       "M5Stack Camera Model B"
#define BOARD_HAS_PSRAM  0
#define CAM_PIN_PWDN     -1
#define CAM_PIN_RESET    15
#define CAM_PIN_XCLK     27
#define CAM_PIN_SIOD     22
#define CAM_PIN_SIOC     23
#define CAM_PIN_D7       19
#define CAM_PIN_D6       36
#define CAM_PIN_D5       18
#define CAM_PIN_D4       39
#define CAM_PIN_D3        5
#define CAM_PIN_D2       34
#define CAM_PIN_D1       35
#define CAM_PIN_D0       32
#define CAM_PIN_VSYNC    25
#define CAM_PIN_HREF     26
#define CAM_PIN_PCLK     21

/* ── TTGO T-Camera (with PIR) ───────────────────────────────────────────── */
#elif defined(CONFIG_BOARD_TTGO_T_CAMERA)
#define BOARD_NAME       "TTGO T-Camera"
#define BOARD_HAS_PSRAM  0
#define CAM_PIN_PWDN     -1
#define CAM_PIN_RESET    -1
#define CAM_PIN_XCLK     32
#define CAM_PIN_SIOD     13
#define CAM_PIN_SIOC     12
#define CAM_PIN_D7       39
#define CAM_PIN_D6       36
#define CAM_PIN_D5       23
#define CAM_PIN_D4       18
#define CAM_PIN_D3       15
#define CAM_PIN_D2        4
#define CAM_PIN_D1       14
#define CAM_PIN_D0        5
#define CAM_PIN_VSYNC    27
#define CAM_PIN_HREF     25
#define CAM_PIN_PCLK     19

/* ── TTGO T-Camera Plus ─────────────────────────────────────────────────── */
#elif defined(CONFIG_BOARD_TTGO_T_CAMERA_PLUS)
#define BOARD_NAME       "TTGO T-Camera Plus"
#define BOARD_HAS_PSRAM  1
#define CAM_PIN_PWDN     -1
#define CAM_PIN_RESET    -1
#define CAM_PIN_XCLK      4
#define CAM_PIN_SIOD     18
#define CAM_PIN_SIOC     23
#define CAM_PIN_D7       36
#define CAM_PIN_D6       37
#define CAM_PIN_D5       38
#define CAM_PIN_D4       39
#define CAM_PIN_D3       35
#define CAM_PIN_D2       26
#define CAM_PIN_D1       13
#define CAM_PIN_D0       34
#define CAM_PIN_VSYNC     5
#define CAM_PIN_HREF     27
#define CAM_PIN_PCLK     25

/* ── TTGO T-Journal ─────────────────────────────────────────────────────── */
#elif defined(CONFIG_BOARD_TTGO_T_JOURNAL)
#define BOARD_NAME       "TTGO T-Journal"
#define BOARD_HAS_PSRAM  0
#define CAM_PIN_PWDN     32
#define CAM_PIN_RESET    -1
#define CAM_PIN_XCLK     27
#define CAM_PIN_SIOD     25
#define CAM_PIN_SIOC     23
#define CAM_PIN_D7       19
#define CAM_PIN_D6       36
#define CAM_PIN_D5       18
#define CAM_PIN_D4       39
#define CAM_PIN_D3        5
#define CAM_PIN_D2       34
#define CAM_PIN_D1       35
#define CAM_PIN_D0       17
#define CAM_PIN_VSYNC    22
#define CAM_PIN_HREF     26
#define CAM_PIN_PCLK     21

/* ── Custom / manual pin entry ──────────────────────────────────────────── */
#elif defined(CONFIG_BOARD_CUSTOM)
#define BOARD_NAME       "Custom"
#define BOARD_HAS_PSRAM  CONFIG_CUSTOM_BOARD_HAS_PSRAM
#define CAM_PIN_PWDN     CONFIG_CUSTOM_CAM_PIN_PWDN
#define CAM_PIN_RESET    CONFIG_CUSTOM_CAM_PIN_RESET
#define CAM_PIN_XCLK     CONFIG_CUSTOM_CAM_PIN_XCLK
#define CAM_PIN_SIOD     CONFIG_CUSTOM_CAM_PIN_SIOD
#define CAM_PIN_SIOC     CONFIG_CUSTOM_CAM_PIN_SIOC
#define CAM_PIN_D7       CONFIG_CUSTOM_CAM_PIN_D7
#define CAM_PIN_D6       CONFIG_CUSTOM_CAM_PIN_D6
#define CAM_PIN_D5       CONFIG_CUSTOM_CAM_PIN_D5
#define CAM_PIN_D4       CONFIG_CUSTOM_CAM_PIN_D4
#define CAM_PIN_D3       CONFIG_CUSTOM_CAM_PIN_D3
#define CAM_PIN_D2       CONFIG_CUSTOM_CAM_PIN_D2
#define CAM_PIN_D1       CONFIG_CUSTOM_CAM_PIN_D1
#define CAM_PIN_D0       CONFIG_CUSTOM_CAM_PIN_D0
#define CAM_PIN_VSYNC    CONFIG_CUSTOM_CAM_PIN_VSYNC
#define CAM_PIN_HREF     CONFIG_CUSTOM_CAM_PIN_HREF
#define CAM_PIN_PCLK     CONFIG_CUSTOM_CAM_PIN_PCLK

#else
#error "No board selected. Choose a board in menuconfig: Anedya WebRTC Camera → Board Selection."
#endif
