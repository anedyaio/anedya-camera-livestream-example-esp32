#pragma once

// =============================================================================
// app_config.h — everything you need to edit lives here.
//
// The ESP-IDF version of this example put these in menuconfig (Kconfig.projbuild).
// Arduino has no menuconfig, so they are plain #defines: fill in the four
// credentials below, flash, done.
//
// Named app_config.h, not config.h: esp_libsrtp (pulled in transitively by
// esp_peer) registers its esp-port/ directory as a public include dir, and that
// directory holds libsrtp's own autoconf config.h. On the include path that one
// wins, so #include "config.h" here silently resolved to libsrtp's header and
// every macro below came up "not declared in this scope".
// =============================================================================

#include "esp_camera.h"  // framesize_t / FRAMESIZE_* used by the stream profile

// -----------------------------------------------------------------------------
// 1. WiFi
// -----------------------------------------------------------------------------
#define WIFI_SSID     "ssid"
#define WIFI_PASSWORD "password"

// -----------------------------------------------------------------------------
// 2. Anedya device credentials  (Anedya console -> Node details)
// -----------------------------------------------------------------------------
// Device ID  — the node's UUID.
// Connection Key — the node's secret.
// The Node ID is NOT needed here; it is entered in the browser viewer.
#define ANEDYA_DEVICE_ID "YOUR_DEVICE_ID_HERE"
#define ANEDYA_CONNECTION_KEY "YOUR_CONNECTION_KEY_HERE"

// Anedya region code. Full list: https://docs.anedya.io/device/#region
#define ANEDYA_REGION_CODE "ap-in-1"

// Derived endpoints — no need to touch these.
#define ANEDYA_MQTT_HOST "mqtt." ANEDYA_REGION_CODE ".anedya.io"
#define ANEDYA_MQTT_PORT 8883
#define ANEDYA_STUN_URL  "stun:turn1." ANEDYA_REGION_CODE ".anedya.io:3478"
#define ANEDYA_TURN_URL  "turn:turn1." ANEDYA_REGION_CODE ".anedya.io:3478"

// The command name the browser viewer sends. Must match the viewer.
#define ANEDYA_OFFER_COMMAND_NAME "webrtc_offer"

// -----------------------------------------------------------------------------
// 3. Board
// -----------------------------------------------------------------------------
// Normally you do NOT edit this — the board comes from the PlatformIO
// environment you build, which passes the macro as a build flag:
//
//   pio run -e seeed_xiao_esp32s3            ->  -DCAMERA_MODEL_XIAO_ESP32S3
//   pio run -e dfrobot_firebeetle2_esp32s3   ->  -DCAMERA_MODEL_DFROBOT_ESP32S3
//   pio run -e dfrobot_romeo_esp32s3         ->  -DCAMERA_MODEL_DFROBOT_ESP32S3
//   pio run -e esp32cam                      ->  -DCAMERA_MODEL_AI_THINKER
//   pio run -e dfrobot_ai_camera             ->  -DCAMERA_MODEL_DFROBOT_AI_CAMERA
//
// That also picks up the right flash size, PSRAM mode and partition table via
// sdkconfig.defaults.<target>. Building outside those environments falls back
// to the XIAO map. Pin maps live in camera_pins.h.
#if !defined(CAMERA_MODEL_XIAO_ESP32S3) && !defined(CAMERA_MODEL_DFROBOT_ESP32S3) && \
    !defined(CAMERA_MODEL_AI_THINKER) && !defined(CAMERA_MODEL_DFROBOT_AI_CAMERA)
#define CAMERA_MODEL_XIAO_ESP32S3
#endif

// -----------------------------------------------------------------------------
// 4. Camera stream profile
// -----------------------------------------------------------------------------
// Sensor output resolution. Bigger = more bytes per frame = lower delivered FPS.
#define CAMERA_STREAM_FRAME_SIZE   FRAMESIZE_HVGA   // 480x320
#define CAMERA_STREAM_FRAME_NAME   "HVGA"
// JPEG quantizer: 0 = best/largest, 63 = worst/smallest.
// Typical: 10 (~6 KB/frame), 25 (~3-4 KB), 40 (~2 KB).
#define CAMERA_STREAM_JPEG_QUALITY 25
// PSRAM frame buffers. With 2+ the DVP DMA runs continuously; 1 stops/starts it
// per frame and roughly halves throughput.
#define CAMERA_STREAM_FB_COUNT     2

// Sensor master clock.
#define CAMERA_STREAM_XCLK_HZ      20000000
// Software rate limiter for the capture loop. Actual FPS at the browser is
// bounded by DataChannel bandwidth (~600 kbps / frame bytes).
#define CAMERA_STREAM_FPS          20

// Alternative "max FPS / lower quality" preset:
//   #define CAMERA_STREAM_FRAME_SIZE   FRAMESIZE_QVGA
//   #define CAMERA_STREAM_JPEG_QUALITY 20
//   #define CAMERA_STREAM_FB_COUNT     3

// -----------------------------------------------------------------------------
// 5. WebRTC DataChannel tuning
// -----------------------------------------------------------------------------
// The label the browser gives its DataChannel. Must match the viewer.
#define WEBRTC_DATA_CHANNEL_LABEL "jpeg-test"

// SCTP send buffer: how many outgoing bytes SCTP holds while waiting on the
// peer receive window. Must be >= one max frame or every send hits WOULD_BLOCK.
// 8 KB is fine on a direct path, but over the Anedya TURN relay (RTT
// 200-1700 ms) many frames are in flight unacked at once; an 8 KB cache jams
// the send path, and that back-pressure starves the ICE agent's STUN consent
// Bindings — which shows up as a healthy stream dying ~90 s in.
#define WEBRTC_SEND_CACHE_SIZE      65536
// The device only receives small control strings from the browser.
#define WEBRTC_RECV_CACHE_SIZE      8192
// How long esp_peer keeps a frame in the send cache before dropping it as
// stale. Set to 10000 (10 seconds) so esp_peer NEVER drops packets internally. 
// Dropping un-ACKed packets on a reliable DataChannel breaks the stream!
// We only want to drop NEW frames at the application layer via WOULD_BLOCK.
#define WEBRTC_CACHE_TIMEOUT_MS     10000
// Max time the ICE agent blocks in recv() per loop tick — also bounds the
// STUN/TURN retransmit delay. 100 ms is ideal for a direct path, but relay RTT
// is 200-1100 ms, so at 100 ms *every* relay consent Binding times out before
// its response arrives and the session tears down. 500 ms buys relay
// transactions room to complete.
#define WEBRTC_AGENT_RECV_TIMEOUT_MS 500

// -----------------------------------------------------------------------------
// 6. DataChannel test mode
// -----------------------------------------------------------------------------
// Uncomment to skip camera init entirely and send "ping N from esp32" over the
// DataChannel instead. Use it to prove signaling + WebRTC work before blaming
// the camera.
// #define DATACHANNEL_TEST_MODE
#define DATACHANNEL_TEST_INTERVAL_MS 500

// -----------------------------------------------------------------------------
// 7. Timing / limits (rarely changed)
// -----------------------------------------------------------------------------
#define ANEDYA_HEARTBEAT_PERIOD_MS   30000  // keeps the node "online" in Anedya
#define ANEDYA_MQTT_RETRY_PERIOD_MS  5000   // between MQTT reconnect attempts
#define ANEDYA_MQTT_BUFFER_BYTES     4096   // PubSubClient rx/tx buffer
#define ANEDYA_MQTT_KEEPALIVE_S      60

#define OFFER_DEFLATE_MAX_BYTES      800    // raw deflate bytes of the offer
#define OFFER_JSON_MAX_BYTES         2048   // inflated offer JSON + NUL
#define ANSWER_DEFLATE_MAX_BYTES     1024   // raw deflate bytes of the answer

#define HEAP_REPORT_PERIOD_MS        30000  // free-heap heartbeat in the log
