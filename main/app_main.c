#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "anedya_sig.h"
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
#include "protocol_examples_common.h"
#include "webrtc_peer.h"
#include "boards.h"

static const char *TAG = "webrtc";

void esp_log_compat_include(void);

/* JPEG-over-DataChannel preview settings — configured via menuconfig. */
#define JPEG_STREAM_FPS        CONFIG_CAMERA_STREAM_FPS
#define JPEG_STREAM_PERIOD_MS  (1000 / JPEG_STREAM_FPS)
#define CAMERA_STREAM_XCLK_HZ  20000000

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
#define CAMERA_STREAM_JPEG_QUALITY      CONFIG_CAMERA_JPEG_QUALITY
#define CAMERA_STREAM_FB_COUNT          CONFIG_CAMERA_FB_COUNT
#define CAMERA_STREAM_GRAB_MODE         CAMERA_GRAB_LATEST

/* Pin map comes from boards.h, selected via menuconfig Board Selection. */

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

static esp_err_t camera_init_once(void)
{
    ESP_LOGE(TAG, ".............. Initializing camera ..............");
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
        .fb_location  = BOARD_HAS_PSRAM ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM,
        .grab_mode    = CAMERA_STREAM_GRAB_MODE,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: 0x%x", err);
        return err;
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

static void release_camera_frame(void *ctx)
{
    if (ctx) {
        esp_camera_fb_return((camera_fb_t *)ctx);
    }
}

static bool is_valid_jpeg(const camera_fb_t *fb)
{
    return fb && fb->len >= 4 &&
           fb->buf[0] == 0xff && fb->buf[1] == 0xd8 &&
           fb->buf[fb->len - 2] == 0xff && fb->buf[fb->len - 1] == 0xd9;
}

#if CONFIG_DATACHANNEL_TEST_MODE
static void datachannel_test_task(void *arg)
{
    uint32_t counter = 0;
    char msg[64];

    ESP_LOGI(TAG, "DataChannel test mode active — no camera, sending text at %d ms interval",
             CONFIG_DATACHANNEL_TEST_INTERVAL_MS);

    for (;;) {
        if (!webrtc_peer_data_channel_ready()) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        int len = snprintf(msg, sizeof(msg), "ping %lu from esp32", (unsigned long)counter);
        bool ok = webrtc_peer_send_text(msg, len);
        if (ok) {
            if ((counter % 20) == 0) {
                ESP_LOGI(TAG, "DC test sent=%lu msg='%s'", (unsigned long)counter, msg);
            }
            counter++;
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_DATACHANNEL_TEST_INTERVAL_MS));
    }
}
#endif /* CONFIG_DATACHANNEL_TEST_MODE */

static void jpeg_stream_task(void *arg)
{
    uint32_t sent = 0;
    uint32_t dropped = 0;
    bool camera_ready = false;

    for (;;) {
        if (!camera_ready) {
            ESP_LOGI(TAG, "DataChannel ready; starting camera stream at %d FPS",
                     JPEG_STREAM_FPS);
            if (camera_init_once() != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            camera_ready = true;
        }

        if (!webrtc_peer_data_channel_ready())
        {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        camera_fb_t *fb = esp_camera_fb_get();
        if (!is_valid_jpeg(fb)) {
            dropped++;
            ESP_LOGW(TAG, "Dropping invalid/missing JPEG frame");
            if (fb) {
                esp_camera_fb_return(fb);
            }
            vTaskDelay(pdMS_TO_TICKS(JPEG_STREAM_PERIOD_MS));
            continue;
        }

        if (webrtc_peer_send_jpeg_ref(fb->buf, fb->len, release_camera_frame, fb)) {
            sent++;
            if ((sent % 10) == 0) {
                ESP_LOGI(TAG, "JPEG stream sent=%lu last=%u bytes dropped=%lu fps=%d",
                         (unsigned long)sent, (unsigned)fb->len,
                         (unsigned long)dropped, JPEG_STREAM_FPS);
            }
        } else {
            dropped++;
            esp_camera_fb_return(fb);
        }
        vTaskDelay(pdMS_TO_TICKS(JPEG_STREAM_PERIOD_MS));
    }
}

void app_main(void)
{
    esp_log_compat_include();

    ESP_LOGI(TAG, "[APP] Startup");
    print_hardware_info();

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("anedya_sig", ESP_LOG_DEBUG);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "WiFi power save disabled");

#if CONFIG_DATACHANNEL_TEST_MODE
    xTaskCreate(datachannel_test_task, "dc_test", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "DataChannel test task started (no camera)");
#else
    xTaskCreate(jpeg_stream_task, "jpeg_stream", 8192, NULL, 4, NULL);
    ESP_LOGI(TAG, "JPEG-over-DataChannel stream task started");
#endif

    anedya_sig_init();
    webrtc_peer_init();

    for (;;) {
        ESP_LOGI(TAG, "Main loop: free memory: %lu bytes", esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
