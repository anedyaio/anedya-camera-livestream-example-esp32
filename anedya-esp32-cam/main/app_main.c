// =============================================================================
// app_main.c — application entry point and camera -> DataChannel stream task.
//
// This is the top of the firmware. Its job is small and easy to follow:
//   1. Bring up the basics: NVS, network interface, WiFi (via the IDF example
//      connection helper), and the Anedya + WebRTC subsystems.
//   2. Run a FreeRTOS task that grabs JPEG frames from the camera and hands
//      them to the WebRTC DataChannel for delivery to the browser.
//
// The two other source files own the harder parts:
//   - anedya_signaling.c handles signaling (exchanging the WebRTC offer/answer).
//   - webrtc_peer.c      handles the WebRTC peer connection and the send pipeline.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_psram.h"
#include "esp_system.h"

#include "anedya_signaling.h"
#include "esp_camera.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_log.h"
#ifdef CONFIG_SPIRAM
#include "esp_psram.h"
#endif
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"  // example_connect(): IDF's WiFi bring-up helper
#include "webrtc_peer.h"
#include "boards.h"                     // camera pin map (AI Thinker ESP32-CAM)

// Tag prefixed to every ESP_LOGx line from this file, so logs are easy to filter.
static const char *TAG = "webrtc";

// Pulls in a small logging-compat shim (see esp_log_compat.c). Calling it forces
// the linker to keep that translation unit even though nothing else references it.
void esp_log_compat_include(void);

// ----------------------------------------------------------------------------
// Stream tuning. These come from menuconfig (Kconfig.projbuild) so you can change
// the profile without editing code: idf.py menuconfig -> "Anedya WebRTC Camera".
// ----------------------------------------------------------------------------
#define JPEG_STREAM_FPS        CONFIG_CAMERA_STREAM_FPS      // target frames per second
#define JPEG_STREAM_PERIOD_MS  (1000 / JPEG_STREAM_FPS)      // delay between frame grabs
#define CAMERA_STREAM_XCLK_HZ  20000000                      // 20 MHz sensor master clock

// Map the menuconfig frame-size choice to the esp32-camera framesize_t enum and a
// human-readable name for logging. HVGA is the default (fastest on the OV3660).
#if defined(CONFIG_CAMERA_FRAMESIZE_96X96)
#  define CAMERA_STREAM_FRAME_SIZE       FRAMESIZE_96X96
#  define CAMERA_STREAM_FRAME_NAME       "96x96"
#elif defined(CONFIG_CAMERA_FRAMESIZE_QQVGA)
#  define CAMERA_STREAM_FRAME_SIZE       FRAMESIZE_QQVGA
#  define CAMERA_STREAM_FRAME_NAME       "QQVGA"
#elif defined(CONFIG_CAMERA_FRAMESIZE_QVGA)
#  define CAMERA_STREAM_FRAME_SIZE       FRAMESIZE_QVGA
#  define CAMERA_STREAM_FRAME_NAME       "QVGA"
#elif defined(CONFIG_CAMERA_FRAMESIZE_HVGA)
#  define CAMERA_STREAM_FRAME_SIZE       FRAMESIZE_HVGA
#  define CAMERA_STREAM_FRAME_NAME       "HVGA"
#elif defined(CONFIG_CAMERA_FRAMESIZE_VGA)
#  define CAMERA_STREAM_FRAME_SIZE       FRAMESIZE_VGA
#  define CAMERA_STREAM_FRAME_NAME       "VGA"
#elif defined(CONFIG_CAMERA_FRAMESIZE_SVGA)
#  define CAMERA_STREAM_FRAME_SIZE       FRAMESIZE_SVGA
#  define CAMERA_STREAM_FRAME_NAME       "SVGA"
#elif defined(CONFIG_CAMERA_FRAMESIZE_HD)
#  define CAMERA_STREAM_FRAME_SIZE       FRAMESIZE_HD
#  define CAMERA_STREAM_FRAME_NAME       "HD"
#else
#  define CAMERA_STREAM_FRAME_SIZE       FRAMESIZE_HVGA
#  define CAMERA_STREAM_FRAME_NAME       "HVGA"
#endif

#define CAMERA_STREAM_ALLOC_FRAME_SIZE  CAMERA_STREAM_FRAME_SIZE
#define CAMERA_STREAM_JPEG_QUALITY      CONFIG_CAMERA_JPEG_QUALITY  // 0=best/big, 63=worst/small
#define CAMERA_STREAM_FB_COUNT          CONFIG_CAMERA_FB_COUNT      // PSRAM frame buffers
#define CAMERA_STREAM_GRAB_MODE         CAMERA_GRAB_LATEST          // always grab newest frame

// FreeRTOS task tuning for the producer tasks started in app_main.
#define STREAM_TASK_STACK_BYTES      8192   // camera JPEG stream task
#define TEST_TASK_STACK_BYTES        4096   // DataChannel test task
#define PRODUCER_TASK_PRIORITY       4

// Timing.
#define VIEWER_POLL_PERIOD_MS        250    // how often to check for a connected viewer
#define CAMERA_RETRY_PERIOD_MS       1000   // wait before retrying a failed camera init
#define HEAP_HEARTBEAT_PERIOD_MS     5000   // how often to log free heap in the idle loop

// Log throttling: only emit every Nth event so a persistent fault does not flood
// the console.
#define FRAME_DROP_LOG_INTERVAL      20     // dropped-frame warnings
#define FRAME_SENT_LOG_INTERVAL      10     // "still streaming" progress lines
#define TEST_SENT_LOG_INTERVAL       20     // test-mode progress lines

// Alternative "max FPS / lower quality" preset if you want to trade image quality
// for frame rate. To use it, replace the four macros above with these values:
//   #define CAMERA_STREAM_FRAME_SIZE   FRAMESIZE_QVGA
//   #define CAMERA_STREAM_JPEG_QUALITY 20
//   #define CAMERA_STREAM_FB_COUNT     3
//   #define CAMERA_STREAM_GRAB_MODE    CAMERA_GRAB_LATEST

// Translate a camera sensor's product ID (PID) into a readable model name. Used
// only for the startup log so you can confirm which sensor the board has.
static const char *sensor_pid_name(uint16_t pid)
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

// Print a one-time summary of the chip, memory, and IDF version at boot. Purely
// informational; handy when debugging "is PSRAM actually enabled?" type issues.
static void print_hardware_info(void)
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
    ESP_LOGI(TAG, "Features:  WiFi=%s  BT=%s  BLE=%s",
             (chip.features & CHIP_FEATURE_WIFI_BGN) ? "yes" : "no",
             (chip.features & CHIP_FEATURE_BT) ? "yes" : "no",
             (chip.features & CHIP_FEATURE_BLE) ? "yes" : "no");
    ESP_LOGI(TAG, "Flash:     %s",
             (chip.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
#ifdef CONFIG_SPIRAM
    ESP_LOGI(TAG, "PSRAM:     yes (%u KB)", (unsigned)(esp_psram_get_size() / 1024));
#else
    ESP_LOGI(TAG, "PSRAM:     not enabled in sdkconfig");
#endif
    ESP_LOGI(TAG, "Heap free: %lu bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "IDF:       %s", esp_get_idf_version());
}

// Initialize the camera driver exactly once. Fills in the pin map from boards.h
// and the stream profile from menuconfig, then verifies the sensor came up and
// switches it to the configured frame size. Returns ESP_OK on success.
static esp_err_t camera_init_once(void)
{
    ESP_LOGI(TAG, "Initializing camera...");
    // The pin_* fields below wire the sensor's parallel data/control lines to the
    // board's GPIOs. All CAM_PIN_* values come from boards.h for this board.
    camera_config_t config = {
        .pin_pwdn     = CAM_PIN_PWDN,
        .pin_reset    = CAM_PIN_RESET,
        .pin_xclk     = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7       = CAM_PIN_D7,
        .pin_d6       = CAM_PIN_D6,
        .pin_d5       = CAM_PIN_D5,
        .pin_d4       = CAM_PIN_D4,
        .pin_d3       = CAM_PIN_D3,
        .pin_d2       = CAM_PIN_D2,
        .pin_d1       = CAM_PIN_D1,
        .pin_d0       = CAM_PIN_D0,
        .pin_vsync    = CAM_PIN_VSYNC,
        .pin_href     = CAM_PIN_HREF,
        .pin_pclk     = CAM_PIN_PCLK,

        .xclk_freq_hz = CAMERA_STREAM_XCLK_HZ,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = CAMERA_STREAM_ALLOC_FRAME_SIZE,
        .jpeg_quality = CAMERA_STREAM_JPEG_QUALITY,
        .fb_count     = CAMERA_STREAM_FB_COUNT,
        // JPEG frame buffers live in PSRAM; a single VGA frame is far larger than
        // the ~320 KB of internal DRAM the ESP32 has free, so PSRAM is required.
        .fb_location  = BOARD_HAS_PSRAM ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM,
        .grab_mode    = CAMERA_STREAM_GRAB_MODE,
    };

    esp_err_t error = esp_camera_init(&config);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: 0x%x", error);
        return error;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor) {
        ESP_LOGE(TAG, "Camera sensor handle is NULL");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "=== CAMERA INFO ===");
    ESP_LOGI(TAG, "Sensor:    %s (PID=0x%04x)",
             sensor_pid_name(sensor->id.PID), sensor->id.PID);
    ESP_LOGI(TAG, "Addr:      0x%02x", sensor->slv_addr);
    int set_result = sensor->set_framesize(sensor, CAMERA_STREAM_FRAME_SIZE);
    if (set_result != 0) {
        ESP_LOGE(TAG, "Failed to switch sensor to %s set_framesize=%d",
                 CAMERA_STREAM_FRAME_NAME, set_result);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Mode:      %s JPEG quality=%d target_fps=%d xclk=%d fb_count=%d",
             CAMERA_STREAM_FRAME_NAME, CAMERA_STREAM_JPEG_QUALITY,
             JPEG_STREAM_FPS, CAMERA_STREAM_XCLK_HZ, CAMERA_STREAM_FB_COUNT);
    return ESP_OK;
}

// Return a camera frame buffer back to the driver so it can be reused. This is
// passed as a callback into the WebRTC send pipeline: the frame stays "checked
// out" until it has actually been sent, then this releases it (zero-copy path).
static void release_camera_frame(void *context)
{
    if (context) {
        esp_camera_fb_return((camera_fb_t *)context);
    }
}

// Sanity-check that a captured buffer is a complete JPEG. Every JPEG begins with
// the SOI marker 0xFFD8 and ends with the EOI marker 0xFFD9; a truncated capture
// fails one of these and we drop it rather than send a corrupt frame.
static bool is_valid_jpeg(const camera_fb_t *frame_buffer)
{
    return frame_buffer && frame_buffer->len >= 4 &&
           frame_buffer->buf[0] == 0xff && frame_buffer->buf[1] == 0xd8 &&
           frame_buffer->buf[frame_buffer->len - 2] == 0xff &&
           frame_buffer->buf[frame_buffer->len - 1] == 0xd9;
}

#if CONFIG_DATACHANNEL_TEST_MODE
// Test-mode task: instead of streaming camera frames, send a small counter string
// over the DataChannel at a fixed interval. Lets you verify signaling and the
// DataChannel end-to-end without a working camera. Enable in menuconfig.
static void datachannel_test_task(void *arg)
{
    uint32_t counter = 0;
    char msg[64];

    ESP_LOGI(TAG, "DataChannel test mode active — no camera, sending text at %d ms interval",
             CONFIG_DATACHANNEL_TEST_INTERVAL_MS);

    for (;;) {
        if (!webrtc_peer_data_channel_ready()) {
            vTaskDelay(pdMS_TO_TICKS(VIEWER_POLL_PERIOD_MS));
            continue;
        }

        int len = snprintf(msg, sizeof(msg), "ping %lu from esp32", (unsigned long)counter);
        bool ok = webrtc_peer_send_text(msg, len);
        if (ok) {
            if ((counter % TEST_SENT_LOG_INTERVAL) == 0) {
                ESP_LOGI(TAG, "DC test sent=%lu msg='%s'", (unsigned long)counter, msg);
            }
            counter++;
        } else {
            ESP_LOGW(TAG, "DC test send failed at counter=%lu", (unsigned long)counter);
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_DATACHANNEL_TEST_INTERVAL_MS));
    }
}
#endif  // CONFIG_DATACHANNEL_TEST_MODE

// Main streaming loop. Runs forever: wait for the DataChannel to be ready, grab a
// JPEG frame, validate it, and hand it to the WebRTC send pipeline. The camera is
// initialized lazily on the first iteration so we don't power it up before there
// is anywhere to send frames.
static void jpeg_stream_task(void *arg)
{
    uint32_t sent = 0;     // running count of frames successfully queued for send
    uint32_t dropped = 0;  // running count of frames dropped (invalid or send failed)
    bool camera_ready = false;

    for (;;) {
        // Lazily initialize the camera on the first iteration. Retry every second
        // if it fails (e.g. sensor not detected) so a transient issue can recover.
        if (!camera_ready) {
            if (camera_init_once() != ESP_OK) {
                ESP_LOGE(TAG, "Camera init failed; retrying in %d ms", CAMERA_RETRY_PERIOD_MS);
                vTaskDelay(pdMS_TO_TICKS(CAMERA_RETRY_PERIOD_MS));
                continue;
            }
            camera_ready = true;
            ESP_LOGI(TAG, "Camera ready; will stream at up to %d FPS once a viewer connects",
                     JPEG_STREAM_FPS);
        }

        // No viewer connected yet: idle here and poll every 250 ms instead of
        // burning the camera and CPU capturing frames nobody will receive.
        if (!webrtc_peer_data_channel_ready())
        {
            vTaskDelay(pdMS_TO_TICKS(VIEWER_POLL_PERIOD_MS));
            continue;
        }

        // Grab the newest JPEG frame from the sensor.
        camera_fb_t *frame_buffer = esp_camera_fb_get();
        if (!frame_buffer) {
            // No buffer returned at all — usually a camera/DMA problem, not a bad
            // frame. Log every time (throttled) since it points at a real fault.
            dropped++;
            if ((dropped % FRAME_DROP_LOG_INTERVAL) == 1) {
                ESP_LOGE(TAG, "esp_camera_fb_get returned NULL (dropped=%lu)",
                         (unsigned long)dropped);
            }
            vTaskDelay(pdMS_TO_TICKS(JPEG_STREAM_PERIOD_MS));
            continue;
        }
        if (!is_valid_jpeg(frame_buffer)) {
            // Got a buffer but it isn't a complete JPEG (missing SOI/EOI marker).
            dropped++;
            if ((dropped % FRAME_DROP_LOG_INTERVAL) == 1) {
                ESP_LOGW(TAG, "Dropping invalid JPEG frame len=%u (dropped=%lu)",
                         (unsigned)frame_buffer->len, (unsigned long)dropped);
            }
            esp_camera_fb_return(frame_buffer);
            vTaskDelay(pdMS_TO_TICKS(JPEG_STREAM_PERIOD_MS));
            continue;
        }

        // Zero-copy send: hand the frame buffer to the pipeline along with the
        // release callback. On success the pipeline owns the buffer and returns it
        // (via release_camera_frame) once sent; on failure we return it ourselves.
        if (webrtc_peer_send_jpeg_by_reference(frame_buffer->buf, frame_buffer->len,
                                               release_camera_frame, frame_buffer)) {
            sent++;
            if ((sent % FRAME_SENT_LOG_INTERVAL) == 0) {
                ESP_LOGI(TAG, "JPEG stream sent=%lu last=%u bytes dropped=%lu fps=%d",
                         (unsigned long)sent, (unsigned)frame_buffer->len,
                         (unsigned long)dropped, JPEG_STREAM_FPS);
            }
        } else {
            // Pipeline rejected the frame (channel not ready or queue full). It
            // did not take ownership, so we must return the buffer ourselves.
            dropped++;
            ESP_LOGD(TAG, "send_jpeg_by_reference rejected frame len=%u (dropped=%lu)",
                     (unsigned)frame_buffer->len, (unsigned long)dropped);
            esp_camera_fb_return(frame_buffer);
        }
        vTaskDelay(pdMS_TO_TICKS(JPEG_STREAM_PERIOD_MS));
    }
}

// Firmware entry point. ESP-IDF calls app_main() after the FreeRTOS scheduler is
// running. We bring up storage/network, start the streaming (or test) task, then
// initialize signaling and the WebRTC peer. The final loop just prints a heap
// heartbeat so you can spot memory leaks while the device runs.
void app_main(void)
{
    esp_log_compat_include();

    ESP_LOGI(TAG, "[APP] Startup");
    print_hardware_info();

    // Global log level INFO. For a normal run this gives a clean, readable story of
    // the connection lifecycle. To trace the low-level signaling steps (base64 /
    // inflate / JSON sizes, per-command dumps), raise a module to DEBUG here, e.g.
    //   esp_log_level_set("anedya_signaling", ESP_LOG_DEBUG);
    //   esp_log_level_set("webrtc_peer",      ESP_LOG_DEBUG);
    esp_log_level_set("*", ESP_LOG_INFO);

    // Standard ESP-IDF bring-up: NVS (used by WiFi to store calibration), the
    // network interface layer, the default event loop, then connect to WiFi using
    // the credentials set under menuconfig -> "Example Connection Configuration".
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());

    // Disable WiFi modem power save. Power save adds latency spikes that hurt the
    // real-time ICE/DTLS handshake and the steady frame stream.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "WiFi power save disabled");

    // Start exactly one producer task: the test-mode counter, or the real camera
    // stream. Which one is chosen at build time in menuconfig.
#if CONFIG_DATACHANNEL_TEST_MODE
    BaseType_t task_ok = xTaskCreate(datachannel_test_task, "dc_test",
                                     TEST_TASK_STACK_BYTES, NULL, PRODUCER_TASK_PRIORITY, NULL);
    if (task_ok == pdPASS) {
        ESP_LOGI(TAG, "DataChannel test task started (no camera)");
    } else {
        ESP_LOGE(TAG, "Failed to create DataChannel test task (out of memory?)");
    }
#else
    BaseType_t task_ok = xTaskCreate(jpeg_stream_task, "jpeg_stream",
                                     STREAM_TASK_STACK_BYTES, NULL, PRODUCER_TASK_PRIORITY, NULL);
    if (task_ok == pdPASS) {
        ESP_LOGI(TAG, "JPEG-over-DataChannel stream task started");
    } else {
        ESP_LOGE(TAG, "Failed to create JPEG stream task (out of memory?)");
    }
#endif

    // Bring up signaling (Anedya MQTT + Commands) and the WebRTC peer. After this
    // the device waits for a browser to send an offer.
    anedya_signaling_init();
    webrtc_peer_init();

    // Heap heartbeat. app_main must not return, so we idle here.
  while (1) {
      size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
      size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
      size_t largest_free_block_internal =
          heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
      size_t largest_free_block_psram =
          heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
      size_t min_free_heap = esp_get_minimum_free_heap_size();
      size_t total_free_heap = esp_get_free_heap_size();
      size_t psram_size = 0;
      if (esp_psram_is_initialized())
        psram_size = esp_psram_get_size();

      ESP_LOGI(TAG,
               "\n\t======= Memory Status ====\n"
               "\tFree internal RAM            : %u bytes (%.2f KB)\n"
               "\tFree PSRAM                   : %u bytes (%.2f KB)\n"
               "\tTotal PSRAM Size             : %u bytes (%.2f KB)\n"
               "\tTotal Free Heap              : %u bytes (%.2f KB)\n"
               "\tMin Free Heap                : %u bytes (%.2f KB)\n"
               "\tLargest free block (internal): %u bytes (%.2f KB)\n"
               "\tLargest free block (PSRAM)   : %u bytes (%.2f KB)\n"
               "\t===========================",
               (unsigned)free_internal, free_internal / 1024.0,
               (unsigned)free_psram, free_psram / 1024.0, (unsigned)psram_size,
               psram_size / 1024.0, (unsigned)total_free_heap,
               total_free_heap / 1024.0, (unsigned)min_free_heap,
               min_free_heap / 1024.0, (unsigned)largest_free_block_internal,
               largest_free_block_internal / 1024.0,
               (unsigned)largest_free_block_psram,
               largest_free_block_psram / 1024.0);

      vTaskDelay(pdMS_TO_TICKS(30000)); // Print every 5 seconds
    }
}
