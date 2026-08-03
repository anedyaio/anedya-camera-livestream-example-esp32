#pragma once

/*
 * Camera pin map — selected by the CAMERA_MODEL_* macro, which the PlatformIO
 * environment passes in as a build flag (see platformio.ini):
 *
 *   pio run -e seeed_xiao_esp32s3  ->  -DCAMERA_MODEL_XIAO_ESP32S3
 *   pio run -e esp32cam            ->  -DCAMERA_MODEL_AI_THINKER
 *
 * app_config.h falls back to the XIAO map if nothing is defined, so the code
 * still builds outside those environments.
 *
 * Defines:
 *   CAM_PIN_PWDN, CAM_PIN_RESET, CAM_PIN_XCLK
 *   CAM_PIN_SIOD, CAM_PIN_SIOC
 *   CAM_PIN_D0..D7, CAM_PIN_VSYNC, CAM_PIN_HREF, CAM_PIN_PCLK
 *   BOARD_NAME  — human-readable string for the boot log
 *
 * Adding a board: add an #elif block here, add a matching [env:...] to
 * platformio.ini with -DCAMERA_MODEL_<yours>, and — if its flash size or PSRAM
 * mode differs from the two below — a sdkconfig.defaults.<target> file.
 */

#include "app_config.h"

#if defined(CAMERA_MODEL_XIAO_ESP32S3)

/* ── Seeed Studio XIAO ESP32S3 Sense (OV2640) ───────────────────────────────
 * Pin map from Seeed's official camera_pins.h (CAMERA_MODEL_XIAO_ESP32S3).
 * PWDN and RESET are not wired out — leave at -1.                            */
#define BOARD_NAME "Seeed XIAO ESP32S3 Sense"
#define CAM_PIN_PWDN  -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK  10
#define CAM_PIN_SIOD  40
#define CAM_PIN_SIOC  39
#define CAM_PIN_D7    48
#define CAM_PIN_D6    11
#define CAM_PIN_D5    12
#define CAM_PIN_D4    14
#define CAM_PIN_D3    16
#define CAM_PIN_D2    18
#define CAM_PIN_D1    17
#define CAM_PIN_D0    15
#define CAM_PIN_VSYNC 38
#define CAM_PIN_HREF  47
#define CAM_PIN_PCLK  13

#elif defined(CAMERA_MODEL_DFROBOT_ESP32S3)

/* ── DFRobot FireBeetle 2 ESP32-S3 / Romeo ESP32-S3 (OV2640) ────────────────
 * Both boards expose the camera on the same pins, so one map covers them.
 * Pin values from arduino-esp32's CameraWebServer camera_pins.h, where they are
 * spelled CAMERA_MODEL_DFRobot_FireBeetle2_ESP32S3 / _Romeo_ESP32S3.
 * PWDN and RESET are not wired out — leave at -1.
 *
 * Camera support needs the PSRAM variants (FireBeetle 2 ESP32-S3 N16R8); the
 * no-PSRAM SKUs cannot allocate a frame buffer.                              */
#define BOARD_NAME "DFRobot ESP32-S3 (FireBeetle 2 / Romeo)"
#define CAM_PIN_PWDN  -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK  45
#define CAM_PIN_SIOD  1
#define CAM_PIN_SIOC  2
#define CAM_PIN_D7    48
#define CAM_PIN_D6    46
#define CAM_PIN_D5    8
#define CAM_PIN_D4    7
#define CAM_PIN_D3    4
#define CAM_PIN_D2    41
#define CAM_PIN_D1    40
#define CAM_PIN_D0    39
#define CAM_PIN_VSYNC 6
#define CAM_PIN_HREF  42
#define CAM_PIN_PCLK  5

#elif defined(CAMERA_MODEL_AI_THINKER)

/* ── ESP32-CAM style boards (OV2640 / OV3660) ───────────────────────────────
 * The AI Thinker ESP32-CAM pinout. DFRobot and the other ESP32-CAM clones use
 * the same module and the same pins, so this one map covers them all.
 * RESET is not wired out — leave at -1.
 *
 * Note GPIO 0 doubles as XCLK and as the bootstrap pin: it must be pulled to
 * GND to enter the bootloader, and released before the camera will run.       */
#define BOARD_NAME "ESP32-CAM (AI Thinker pinout)"
#define CAM_PIN_PWDN  32
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK  0
#define CAM_PIN_SIOD  26
#define CAM_PIN_SIOC  27
#define CAM_PIN_D7    35
#define CAM_PIN_D6    34
#define CAM_PIN_D5    39
#define CAM_PIN_D4    36
#define CAM_PIN_D3    21
#define CAM_PIN_D2    19
#define CAM_PIN_D1    18
#define CAM_PIN_D0    5
#define CAM_PIN_VSYNC 25
#define CAM_PIN_HREF  23
#define CAM_PIN_PCLK  22


#elif defined(CAMERA_MODEL_DFROBOT_AI_CAMERA)
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

#else
#error "No camera board selected — build with an env from platformio.ini, or define CAMERA_MODEL_XIAO_ESP32S3 / CAMERA_MODEL_DFROBOT_ESP32S3 / CAMERA_MODEL_AI_THINKER / CAMERA_MODEL_DFROBOT_AI_CAMERA in app_config.h"
#endif