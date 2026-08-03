#pragma once

/*
 * Camera pin map — selected by the "Camera board" choice in menuconfig
 * (Kconfig.projbuild -> CONFIG_CAMERA_BOARD_*).
 *
 * Defines:
 *   CAM_PIN_PWDN, CAM_PIN_RESET, CAM_PIN_XCLK
 *   CAM_PIN_SIOD, CAM_PIN_SIOC
 *   CAM_PIN_D0..D7, CAM_PIN_VSYNC, CAM_PIN_HREF, CAM_PIN_PCLK
 *   BOARD_HAS_PSRAM  — 1 if board has PSRAM, 0 if not
 *   BOARD_NAME       — human-readable string for log output
 */

#if defined(CONFIG_CAMERA_BOARD_XIAO_ESP32S3)

/* ── Seeed Studio XIAO ESP32S3 Sense (OV2640/OV3660) ────────────────────────
 * Pin map from Seeed's official camera_pins.h (CAMERA_MODEL_XIAO_ESP32S3).
 * PWDN and RESET are not wired out — leave at -1.                            */
#define BOARD_NAME "Seeed XIAO ESP32S3 Sense"
#define BOARD_HAS_PSRAM 1
#define CAM_PIN_PWDN -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 10
#define CAM_PIN_SIOD 40
#define CAM_PIN_SIOC 39
#define CAM_PIN_D7 48
#define CAM_PIN_D6 11
#define CAM_PIN_D5 12
#define CAM_PIN_D4 14
#define CAM_PIN_D3 16
#define CAM_PIN_D2 18
#define CAM_PIN_D1 17
#define CAM_PIN_D0 15
#define CAM_PIN_VSYNC 38
#define CAM_PIN_HREF 47
#define CAM_PIN_PCLK 13

#elif defined(CONFIG_CAMERA_BOARD_AI_THINKER)

/* ── AI Thinker ESP32-CAM ───────────────────────────────────────────────── */
#define BOARD_NAME "AI Thinker ESP32-CAM"
#define BOARD_HAS_PSRAM 1
#define CAM_PIN_PWDN 32
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 0
#define CAM_PIN_SIOD 26
#define CAM_PIN_SIOC 27
#define CAM_PIN_D7 35
#define CAM_PIN_D6 34
#define CAM_PIN_D5 39
#define CAM_PIN_D4 36
#define CAM_PIN_D3 21
#define CAM_PIN_D2 19
#define CAM_PIN_D1 18
#define CAM_PIN_D0 5
#define CAM_PIN_VSYNC 25
#define CAM_PIN_HREF 23
#define CAM_PIN_PCLK 22

#elif defined(CONFIG_CAMERA_BOARD_DFROBOT_ESP32S3)
#define BOARD_NAME "DFRobot ESP32-S3 AI Camera"
#define BOARD_HAS_PSRAM 1
#define CAM_PIN_PWDN -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 5
#define CAM_PIN_SIOD 8
#define CAM_PIN_SIOC 9
#define CAM_PIN_D7 4  /* Y9 */
#define CAM_PIN_D6 6  /* Y8 */
#define CAM_PIN_D5 7  /* Y7 */
#define CAM_PIN_D4 14 /* Y6 */
#define CAM_PIN_D3 17 /* Y5 */
#define CAM_PIN_D2 21 /* Y4 */
#define CAM_PIN_D1 18 /* Y3 */
#define CAM_PIN_D0 16 /* Y2 */
#define CAM_PIN_VSYNC 1
#define CAM_PIN_HREF 2
#define CAM_PIN_PCLK 15

#endif
