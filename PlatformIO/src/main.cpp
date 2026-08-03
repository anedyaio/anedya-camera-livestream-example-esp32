// =============================================================================
// main.cpp — Arduino entry point: camera capture -> WebRTC DataChannel.
//
// setup() brings up the basics: Serial, camera, WiFi, Anedya signaling and the
// WebRTC peer. loop() then does two things forever:
//   1. pump Anedya MQTT signaling (offers in, answers/status out)
//   2. grab a JPEG frame and hand it to the WebRTC send pipeline
//
// The two other source files own the harder parts:
//   - anedya_signaling.cpp  signaling: exchanging the WebRTC offer/answer
//   - webrtc_peer.cpp       the WebRTC peer connection and the send pipeline
//
// Everything you need to configure is in include/app_config.h.
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>

#include "esp_camera.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"

#include "anedya_signaling.h"
#include "camera_pins.h"
#include "app_config.h"
#include "webrtc_peer.h"

static const char *TAG = "webrtc";

#define JPEG_STREAM_PERIOD_MS (1000 / CAMERA_STREAM_FPS)

#define WIFI_CONNECT_TIMEOUT_MS 30000
#define TIME_SYNC_TIMEOUT_MS    10000
#define CAMERA_RETRY_PERIOD_MS  1000

// Log throttling: only emit every Nth event so a persistent fault does not
// flood the console.
#define FRAME_DROP_LOG_INTERVAL 20
#define FRAME_SENT_LOG_INTERVAL 10
#define TEST_SENT_LOG_INTERVAL  20

static bool s_cameraReady;
static uint32_t s_lastFrameMs;
static uint32_t s_lastCameraRetryMs;
static uint32_t s_lastHeapReportMs;
static uint32_t s_framesSent;
static uint32_t s_framesDropped;

#ifdef DATACHANNEL_TEST_MODE
static uint32_t s_testCounter;
static uint32_t s_lastTestMs;
#endif

// -----------------------------------------------------------------------------
// Boot diagnostics
// -----------------------------------------------------------------------------

// Translate a sensor product ID into a readable model name, so the boot log
// confirms which sensor the board actually has.
static const char *sensorPidName(uint16_t pid)
{
    switch (pid) {
        case 0x2640: return "OV2640";
        case 0x3660: return "OV3660";
        case 0x5640: return "OV5640";
        case 0x7670: return "OV7670";
        case 0x7725: return "OV7725";
        case 0x2145: return "GC2145";
        case 0x032a: return "GC032A";
        case 0x9141: return "NT99141";
        default: return "UNKNOWN";
    }
}

static void printHardwareInfo()
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    const char *model = "UNKNOWN";
    switch (chip.model) {
        case CHIP_ESP32: model = "ESP32"; break;
        case CHIP_ESP32S2: model = "ESP32-S2"; break;
        case CHIP_ESP32S3: model = "ESP32-S3"; break;
        case CHIP_ESP32C3: model = "ESP32-C3"; break;
        default: break;
    }

    ESP_LOGI(TAG, "=== HARDWARE INFO ===");
    ESP_LOGI(TAG, "Board:     %s", BOARD_NAME);
    ESP_LOGI(TAG, "Chip:      %s rev %d", model, chip.revision);
    ESP_LOGI(TAG, "Cores:     %d", chip.cores);
    ESP_LOGI(TAG, "CPU freq:  %lu MHz", (unsigned long)getCpuFrequencyMhz());
    if (esp_psram_is_initialized()) {
        ESP_LOGI(TAG, "PSRAM:     yes (%u KB)", (unsigned)(esp_psram_get_size() / 1024));
    } else {
        // Without PSRAM the camera frame buffer allocation simply fails — a
        // single HVGA JPEG buffer dwarfs the free internal DRAM.
        ESP_LOGE(TAG, "PSRAM:     NOT initialized — camera init will fail");
    }
    ESP_LOGI(TAG, "Heap free: %lu bytes", (unsigned long)ESP.getFreeHeap());
}

static void reportHeap()
{
    ESP_LOGI(TAG,
             "\n\t======= Memory Status ====\n"
             "\tFree internal RAM            : %u bytes (%.2f KB)\n"
             "\tFree PSRAM                   : %u bytes (%.2f KB)\n"
             "\tLargest free block (internal): %u bytes (%.2f KB)\n"
             "\tLargest free block (PSRAM)   : %u bytes (%.2f KB)\n"
             "\tMin free heap ever           : %u bytes (%.2f KB)\n"
             "\t===========================",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024.0,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024.0,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024.0,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024.0,
             (unsigned)esp_get_minimum_free_heap_size(),
             esp_get_minimum_free_heap_size() / 1024.0);
}

// -----------------------------------------------------------------------------
// Camera
// -----------------------------------------------------------------------------

// Initialize the camera driver once. Wires the sensor's parallel data/control
// lines to the board GPIOs (camera_pins.h) and applies the stream profile
// (config.h), then verifies the sensor came up.
static bool cameraInitOnce()
{
    ESP_LOGI(TAG, "Initializing camera...");

    camera_config_t config = {};
    config.pin_pwdn = CAM_PIN_PWDN;
    config.pin_reset = CAM_PIN_RESET;
    config.pin_xclk = CAM_PIN_XCLK;
    config.pin_sccb_sda = CAM_PIN_SIOD;
    config.pin_sccb_scl = CAM_PIN_SIOC;
    config.pin_d7 = CAM_PIN_D7;
    config.pin_d6 = CAM_PIN_D6;
    config.pin_d5 = CAM_PIN_D5;
    config.pin_d4 = CAM_PIN_D4;
    config.pin_d3 = CAM_PIN_D3;
    config.pin_d2 = CAM_PIN_D2;
    config.pin_d1 = CAM_PIN_D1;
    config.pin_d0 = CAM_PIN_D0;
    config.pin_vsync = CAM_PIN_VSYNC;
    config.pin_href = CAM_PIN_HREF;
    config.pin_pclk = CAM_PIN_PCLK;

    config.xclk_freq_hz = CAMERA_STREAM_XCLK_HZ;
    config.ledc_timer = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = CAMERA_STREAM_FRAME_SIZE;
    config.jpeg_quality = CAMERA_STREAM_JPEG_QUALITY;
    config.fb_count = CAMERA_STREAM_FB_COUNT;
    // JPEG frame buffers live in PSRAM; a single frame is far larger than the
    // internal DRAM the chip has free.
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;  // always hand us the newest frame

    esp_err_t error = esp_camera_init(&config);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: 0x%x", error);
        return false;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor) {
        ESP_LOGE(TAG, "Camera sensor handle is NULL");
        return false;
    }

    ESP_LOGI(TAG, "=== CAMERA INFO ===");
    ESP_LOGI(TAG, "Sensor:    %s (PID=0x%04x)", sensorPidName(sensor->id.PID), sensor->id.PID);
    ESP_LOGI(TAG, "Addr:      0x%02x", sensor->slv_addr);
    if (sensor->set_framesize(sensor, CAMERA_STREAM_FRAME_SIZE) != 0) {
        ESP_LOGE(TAG, "Failed to switch sensor to %s", CAMERA_STREAM_FRAME_NAME);
        return false;
    }
    ESP_LOGI(TAG, "Mode:      %s JPEG quality=%d target_fps=%d xclk=%d fb_count=%d",
             CAMERA_STREAM_FRAME_NAME, CAMERA_STREAM_JPEG_QUALITY, CAMERA_STREAM_FPS,
             CAMERA_STREAM_XCLK_HZ, CAMERA_STREAM_FB_COUNT);
    return true;
}

// Return a camera frame buffer to the driver. Passed into the WebRTC send
// pipeline: the frame stays "checked out" until it has actually been sent, then
// this releases it — that is what makes the path zero-copy.
static void releaseCameraFrame(void *context)
{
    if (context) {
        esp_camera_fb_return((camera_fb_t *)context);
    }
}

// Sanity-check that a captured buffer is a complete JPEG. Every JPEG starts
// with the SOI marker 0xFFD8 and ends with EOI 0xFFD9; a truncated capture
// fails one of these, and we drop it rather than send a corrupt frame.
static bool isValidJpeg(const camera_fb_t *frameBuffer)
{
    return frameBuffer && frameBuffer->len >= 4 &&
           frameBuffer->buf[0] == 0xff && frameBuffer->buf[1] == 0xd8 &&
           frameBuffer->buf[frameBuffer->len - 2] == 0xff &&
           frameBuffer->buf[frameBuffer->len - 1] == 0xd9;
}

// -----------------------------------------------------------------------------
// Network
// -----------------------------------------------------------------------------

static void connectWiFi()
{
    ESP_LOGI(TAG, "Connecting to WiFi SSID '%s'...", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
        delay(250);
    }

    if (WiFi.status() != WL_CONNECTED) {
        ESP_LOGE(TAG, "WiFi did not connect within %d ms — restarting", WIFI_CONNECT_TIMEOUT_MS);
        delay(1000);
        ESP.restart();
    }

    ESP_LOGI(TAG, "WiFi connected, IP %s (RSSI %d dBm)",
             WiFi.localIP().toString().c_str(), WiFi.RSSI());

    // Modem power save adds latency spikes that hurt both the ICE/DTLS
    // handshake and the steady frame stream.
    WiFi.setSleep(false);
    ESP_LOGI(TAG, "WiFi power save disabled");
}

// TLS certificate validation needs a plausible wall clock. Best effort — if
// NTP is unreachable we carry on rather than block the whole device.
static void syncTime()
{
    ESP_LOGI(TAG, "Syncing time over NTP...");
    configTime(0, 0, "pool.ntp.org", "time.google.com");

    uint32_t start = millis();
    time_t now = 0;
    while (millis() - start < TIME_SYNC_TIMEOUT_MS) {
        now = time(nullptr);
        if (now > 1700000000) {  // any time after Nov 2023 means NTP replied
            ESP_LOGI(TAG, "Time synced: %s", ctime(&now));
            return;
        }
        delay(200);
    }
    ESP_LOGW(TAG, "NTP sync timed out; continuing (MQTT TLS may reject the broker cert)");
}

// -----------------------------------------------------------------------------
// Streaming
// -----------------------------------------------------------------------------

#ifdef DATACHANNEL_TEST_MODE
// Test mode: instead of camera frames, send a small counter string over the
// DataChannel. Verifies signaling and the DataChannel end to end without a
// working camera.
static void runDataChannelTest()
{
    if (!webrtcPeerDataChannelReady()) {
        return;
    }
    if (millis() - s_lastTestMs < DATACHANNEL_TEST_INTERVAL_MS) {
        return;
    }
    s_lastTestMs = millis();

    char message[64];
    int length = snprintf(message, sizeof(message), "ping %lu from esp32",
                          (unsigned long)s_testCounter);
    if (webrtcPeerSendText(message, length)) {
        if ((s_testCounter % TEST_SENT_LOG_INTERVAL) == 0) {
            ESP_LOGI(TAG, "DC test sent=%lu msg='%s'", (unsigned long)s_testCounter, message);
        }
        s_testCounter++;
    } else {
        ESP_LOGW(TAG, "DC test send failed at counter=%lu", (unsigned long)s_testCounter);
    }
}
#else
// Grab one JPEG frame and hand it to the WebRTC send pipeline, rate-limited to
// CAMERA_STREAM_FPS. Does nothing until a viewer's DataChannel is open — no
// point burning the camera and CPU on frames nobody receives.
static void streamOneFrame()
{
    // Retry a failed camera init rather than giving up for good.
    if (!s_cameraReady) {
        if (millis() - s_lastCameraRetryMs < CAMERA_RETRY_PERIOD_MS) {
            return;
        }
        s_lastCameraRetryMs = millis();
        s_cameraReady = cameraInitOnce();
        if (!s_cameraReady) {
            ESP_LOGE(TAG, "Camera init failed; retrying in %d ms", CAMERA_RETRY_PERIOD_MS);
        }
        return;
    }

    if (!webrtcPeerDataChannelReady()) {
        return;
    }
    if (millis() - s_lastFrameMs < JPEG_STREAM_PERIOD_MS) {
        return;
    }
    s_lastFrameMs = millis();

    camera_fb_t *frameBuffer = esp_camera_fb_get();
    if (!frameBuffer) {
        // No buffer at all — usually a camera/DMA fault, not a bad frame.
        s_framesDropped++;
        if ((s_framesDropped % FRAME_DROP_LOG_INTERVAL) == 1) {
            ESP_LOGE(TAG, "esp_camera_fb_get returned NULL (dropped=%lu)",
                     (unsigned long)s_framesDropped);
        }
        return;
    }

    if (!isValidJpeg(frameBuffer)) {
        s_framesDropped++;
        if ((s_framesDropped % FRAME_DROP_LOG_INTERVAL) == 1) {
            ESP_LOGW(TAG, "Dropping invalid JPEG frame len=%u (dropped=%lu)",
                     (unsigned)frameBuffer->len, (unsigned long)s_framesDropped);
        }
        esp_camera_fb_return(frameBuffer);
        return;
    }

    // Zero-copy send: hand over the frame buffer plus the release callback. On
    // success the pipeline owns the buffer and returns it once sent; on failure
    // it never took ownership, so we return it ourselves.
    if (webrtcPeerSendJpegByReference(frameBuffer->buf, frameBuffer->len,
                                      releaseCameraFrame, frameBuffer)) {
        s_framesSent++;
        if ((s_framesSent % FRAME_SENT_LOG_INTERVAL) == 0) {
            ESP_LOGI(TAG, "JPEG stream sent=%lu last=%u bytes dropped=%lu fps=%d",
                     (unsigned long)s_framesSent, (unsigned)frameBuffer->len,
                     (unsigned long)s_framesDropped, CAMERA_STREAM_FPS);
        }
    } else {
        s_framesDropped++;
        ESP_LOGD(TAG, "Send pipeline rejected frame len=%u (dropped=%lu)",
                 (unsigned)frameBuffer->len, (unsigned long)s_framesDropped);
        esp_camera_fb_return(frameBuffer);
    }
}
#endif  // DATACHANNEL_TEST_MODE

// -----------------------------------------------------------------------------
// Arduino entry points
// -----------------------------------------------------------------------------

void setup()
{
    Serial.begin(115200);
    delay(1500);  // give the USB CDC port time to enumerate before the first log

    ESP_LOGI(TAG, "[APP] Anedya WebRTC camera livestream (Arduino)");
    printHardwareInfo();

    // INFO gives a clean story of the connection lifecycle. To trace the
    // low-level signaling steps (base64 / inflate / JSON sizes), raise a module
    // to DEBUG here:
    //   esp_log_level_set("anedya_signaling", ESP_LOG_DEBUG);
    //   esp_log_level_set("webrtc_peer",      ESP_LOG_DEBUG);
    esp_log_level_set("*", ESP_LOG_INFO);

    // libpeer's SCTP plumbing logs every TSN and heartbeat at INFO, which
    // floods the console and buries the events that matter. AGENT stays at INFO
    // so TURN permission refreshes and relay failures remain visible.
    esp_log_level_set("BUF_MNGR", ESP_LOG_WARN);
    esp_log_level_set("SCTP", ESP_LOG_WARN);

#ifdef DATACHANNEL_TEST_MODE
    ESP_LOGI(TAG, "DataChannel test mode active — camera not initialized");
#else
    // Init the camera before WiFi so its PSRAM frame buffers get a clean heap.
    s_cameraReady = cameraInitOnce();
    if (!s_cameraReady) {
        ESP_LOGE(TAG, "Camera init failed at boot; will retry from loop()");
    }
#endif

    connectWiFi();
    syncTime();

    // Signaling first, then the peer: the device is ready to receive an offer
    // the moment the MQTT subscription lands.
    anedyaSignalingBegin();
    webrtcPeerBegin();

    s_lastHeapReportMs = millis();
    ESP_LOGI(TAG, "Setup complete — waiting for a viewer");
}

void loop()
{
    // Offers in, answers and status updates out.
    anedyaSignalingLoop();

#ifdef DATACHANNEL_TEST_MODE
    runDataChannelTest();
#else
    streamOneFrame();
#endif

    // Heap heartbeat, handy for spotting a leak over a long run.
    if (millis() - s_lastHeapReportMs >= HEAP_REPORT_PERIOD_MS) {
        s_lastHeapReportMs = millis();
        reportHeap();
    }

    // Yield so lower-priority system tasks (and the idle task's TWDT feed) run.
    delay(1);
}
