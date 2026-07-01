#pragma once

/*
 * Camera pin map for the ESP32-CAM.
 *
 * Defines:
 *   CAM_PIN_PWDN, CAM_PIN_RESET, CAM_PIN_XCLK
 *   CAM_PIN_SIOD, CAM_PIN_SIOC
 *   CAM_PIN_D0..D7, CAM_PIN_VSYNC, CAM_PIN_HREF, CAM_PIN_PCLK
 *   BOARD_HAS_PSRAM  — 1 if board has PSRAM, 0 if not
 *   BOARD_NAME       — human-readable string for log output
 */

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
