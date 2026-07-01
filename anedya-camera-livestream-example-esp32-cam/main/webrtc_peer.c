#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_peer.h"
#include "esp_peer_default.h"

#include "anedya_sig.h"
#include "webrtc_peer.h"

static const char *TAG = "webrtc_peer";

/*
 * WebRTC peer for the ESP32-CAM example.
 *
 * Signaling is intentionally simple:
 *   browser writes offer_<session>       -> Anedya ValueStore
 *   ESP writes answer_<session>          -> Anedya ValueStore
 *   optional ICE candidates use candidate_* keys
 *
 * Video is also intentionally simple:
 *   camera JPEG frame -> WebRTC DataChannel -> browser <img>
 *
 * This is not full WebRTC RTP video. It is a low-resolution educational preview
 * path that is easy to inspect in C and JavaScript.
 */

#define JPEG_DATA_CHANNEL_LABEL "jpeg-test"
#define MSG_QUEUE_DEPTH 8
#define JPEG_QUEUE_DEPTH 1

#define TURN_URL "turn:turn1.ap-in-1.anedya.io:3478"
#define TURN_CRED_MAX_LEN 128

// STUN-only by default. A TURN entry is appended when the browser sends
// credentials alongside the offer (see webrtc_peer_set_turn_credentials).
static esp_peer_ice_server_cfg_t s_ice_servers[2] = {
    {.stun_url = "stun:turn1.ap-in-1.anedya.io:3478", .user = NULL, .psw = NULL},
};
static int s_ice_server_count = 1;

static char s_turn_user[TURN_CRED_MAX_LEN];
static char s_turn_psw[TURN_CRED_MAX_LEN];
static char s_turn_url[] = TURN_URL;

typedef struct {
    esp_peer_msg_type_t type;
    uint8_t *data;
    int size;
} queued_msg_t;

typedef struct {
    uint8_t *data;
    int size;
    bool owns_data;
    webrtc_peer_jpeg_release_cb_t release_cb;
    void *release_ctx;
} queued_jpeg_t;

static esp_peer_handle_t s_peer;
static QueueHandle_t s_msg_queue;
static QueueHandle_t s_jpeg_queue;


static volatile bool s_start_new_connection;
static volatile bool s_data_channel_ready;
static uint16_t s_data_stream_id;

static const char *peer_state_name(esp_peer_state_t state)
{
    switch (state) {
        case ESP_PEER_STATE_CLOSED: return "CLOSED";
        case ESP_PEER_STATE_DISCONNECTED: return "DISCONNECTED";
        case ESP_PEER_STATE_NEW_CONNECTION: return "NEW_CONNECTION";
        case ESP_PEER_STATE_CANDIDATE_GATHERING: return "CANDIDATE_GATHERING";
        case ESP_PEER_STATE_PAIRING: return "PAIRING";
        case ESP_PEER_STATE_PAIRED: return "PAIRED";
        case ESP_PEER_STATE_CONNECTING: return "CONNECTING";
        case ESP_PEER_STATE_CONNECTED: return "CONNECTED";
        case ESP_PEER_STATE_CONNECT_FAILED: return "CONNECT_FAILED";
        case ESP_PEER_STATE_DATA_CHANNEL_CONNECTED: return "DATA_CHANNEL_CONNECTED";
        case ESP_PEER_STATE_DATA_CHANNEL_OPENED: return "DATA_CHANNEL_OPENED";
        case ESP_PEER_STATE_DATA_CHANNEL_CLOSED: return "DATA_CHANNEL_CLOSED";
        case ESP_PEER_STATE_DATA_CHANNEL_DISCONNECTED: return "DATA_CHANNEL_DISCONNECTED";
        default: return "UNKNOWN";
    }
}

static bool is_disconnected_state(esp_peer_state_t state)
{
    return state == ESP_PEER_STATE_DISCONNECTED ||
           state == ESP_PEER_STATE_CONNECT_FAILED ||
           state == ESP_PEER_STATE_DATA_CHANNEL_CLOSED ||
           state == ESP_PEER_STATE_DATA_CHANNEL_DISCONNECTED ||
           state == ESP_PEER_STATE_CLOSED;
}

static int on_peer_state(esp_peer_state_t state, void *ctx)
{
    ESP_LOGI(TAG, "Peer state: %s (%d)", peer_state_name(state), (int)state);
    if (is_disconnected_state(state)) {
        s_data_channel_ready = false;
    }

    /*
     * Conclude the signaling command on a terminal connection outcome so both
     * Anedya and the browser learn the result. The data channel opening is our
     * "fully working" signal (success); CONNECT_FAILED is the failure signal.
     * anedya_sig_command_conclude is a no-op if the command was already concluded.
     */
    if (state == ESP_PEER_STATE_DATA_CHANNEL_CONNECTED) {
        anedya_sig_command_conclude(true, NULL);
    } else if (state == ESP_PEER_STATE_CONNECT_FAILED) {
        anedya_sig_command_conclude(false, "webrtc connect failed");
    }
    return 0;
}

static int on_peer_msg(esp_peer_msg_t *msg, void *ctx)
{
    if (!msg || !msg->data || msg->size <= 0) {
        return 0;
    }

    /*
     * esp_peer gives byte ranges. Copy to a null-terminated string before
     * passing the payload to the Anedya ValueStore helpers.
     */
    char *payload = malloc(msg->size + 1);
    if (!payload) {
        ESP_LOGE(TAG, "OOM copying peer message");
        return -1;
    }
    memcpy(payload, msg->data, msg->size);
    payload[msg->size] = '\0';

    if (msg->type == ESP_PEER_MSG_TYPE_SDP) {
        ESP_LOGI(TAG, "Local SDP answer ready (%d bytes)", msg->size);
        anedya_sig_write_answer(payload, msg->size);
    }
    /*
     * Local ICE candidates are ignored: the answer SDP is sent only after ICE
     * gathering completes, so it already carries all candidates (non-trickle).
     */

    free(payload);
    return 0;
}

static int on_channel_open(esp_peer_data_channel_info_t *ch, void *ctx)
{
    const char *label = (ch && ch->label) ? ch->label : "";
    int stream_id = ch ? ch->stream_id : -1;
    ESP_LOGI(TAG, "Data channel opened: label=%s stream_id=%d", label, stream_id);

    /*
     * The browser creates the DataChannel named "jpeg-test".
     * Ignore any other channel so JPEGs are sent on the stream the browser reads.
     */
    if (!ch || strcmp(label, JPEG_DATA_CHANNEL_LABEL) != 0) {
        ESP_LOGI(TAG, "Ignoring non-JPEG data channel: label=%s", label);
        return 0;
    }

    s_data_stream_id = ch->stream_id;
    s_data_channel_ready = true;

    const char hello[] = "hello from esp32 data channel";
    esp_peer_data_frame_t frame = {
        .type = ESP_PEER_DATA_CHANNEL_STRING,
        .stream_id = ch->stream_id,
        .data = (uint8_t *)hello,
        .size = sizeof(hello) - 1,
    };
    int ret = esp_peer_send_data(s_peer, &frame);
    ESP_LOGI(TAG, "Sent data channel text test (%d bytes), ret=%d",
             (int)frame.size, ret);
    return 0;
}

static int on_data(esp_peer_data_frame_t *frame, void *ctx)
{
    if (!frame || !frame->data || frame->size <= 0) {
        return 0;
    }
    ESP_LOGI(TAG, "Data channel rx: stream_id=%d size=%d text='%.*s'",
             frame->stream_id, frame->size, frame->size, (const char *)frame->data);
    return 0;
}

static int on_channel_close(esp_peer_data_channel_info_t *ch, void *ctx)
{
    const char *label = (ch && ch->label) ? ch->label : "";
    ESP_LOGI(TAG, "Data channel closed: label=%s", label);
    if (!ch || strcmp(label, JPEG_DATA_CHANNEL_LABEL) == 0 ||
        ch->stream_id == s_data_stream_id) {
        s_data_channel_ready = false;
    }
    return 0;
}

static void drop_pending_jpegs(void)
{
    if (!s_jpeg_queue) {
        return;
    }

    queued_jpeg_t jpeg;
    while (xQueueReceive(s_jpeg_queue, &jpeg, 0) == pdTRUE) {
        if (jpeg.release_cb) {
            jpeg.release_cb(jpeg.release_ctx);
        } else if (jpeg.owns_data) {
            free(jpeg.data);
        }
    }
}

static void release_queued_jpeg(queued_jpeg_t *jpeg)
{
    if (!jpeg) {
        return;
    }
    if (jpeg->release_cb) {
        jpeg->release_cb(jpeg->release_ctx);
    } else if (jpeg->owns_data) {
        free(jpeg->data);
    }
}

static void send_queued_jpeg(void)
{
    static uint32_t s_would_block_count;

    if (!s_peer || !s_data_channel_ready || !s_jpeg_queue) {
        drop_pending_jpegs();
        return;
    }

    queued_jpeg_t jpeg;
    if (xQueueReceive(s_jpeg_queue, &jpeg, 0) != pdTRUE) {
        return;
    }

    esp_peer_data_frame_t frame = {
        .type = ESP_PEER_DATA_CHANNEL_DATA,
        .stream_id = s_data_stream_id,
        .data = jpeg.data,
        .size = jpeg.size,
    };

    int ret = esp_peer_send_data(s_peer, &frame);
    if (ret == ESP_PEER_ERR_NONE) {
        s_would_block_count = 0;
    } else if (ret == ESP_PEER_ERR_WOULD_BLOCK) {
        s_would_block_count++;
        if (s_would_block_count == 1 || (s_would_block_count % 25) == 0) {
            ESP_LOGW(TAG, "JPEG send would block len=%d count=%lu",
                     jpeg.size, (unsigned long)s_would_block_count);
        }
    } else {
        ESP_LOGE(TAG, "JPEG send failed len=%d ret=%d", jpeg.size, ret);
    }

    release_queued_jpeg(&jpeg);
}

static void peer_task(void *arg)
{
    for (;;) {
        if (s_start_new_connection) {
            s_start_new_connection = false;
            s_data_channel_ready = false;
            s_data_stream_id = 0;

            /*
             * Tear down any prior session first. Without this, a previous
             * CONNECT_FAILED/leftover connection makes esp_peer_update_ice_info
             * return -3 (invalid state) and the new connection inherits stale
             * agent state. esp_peer_disconnect is safe to call when idle.
             */
            esp_peer_disconnect(s_peer);
            
            int ice_ret = esp_peer_update_ice_info(s_peer, ESP_PEER_ROLE_CONTROLLED,
                                                    s_ice_servers, s_ice_server_count);
            ESP_LOGI(TAG, "esp_peer_update_ice_info servers=%d ret=%d",
                     s_ice_server_count, ice_ret);
            if (ice_ret == ESP_PEER_ERR_NONE) {
                ESP_LOGI(TAG, "Starting new WebRTC connection");
                int ret = esp_peer_new_connection(s_peer);
                ESP_LOGI(TAG, "esp_peer_new_connection ret=%d", ret);
            } else {
                ESP_LOGE(TAG, "update_ice_info failed (ret=%d); not starting connection",
                         ice_ret);
            }
        }

        queued_msg_t queued_msg;
        while (xQueueReceive(s_msg_queue, &queued_msg, 0) == pdTRUE) {
            esp_peer_msg_t msg = {
                .type = queued_msg.type,
                .data = queued_msg.data,
                .size = queued_msg.size,
            };

            ESP_LOGI(TAG, "Sending peer msg type=%d size=%d starts='%.32s'",
                     (int)msg.type, msg.size, (const char *)msg.data);
            int ret = esp_peer_send_msg(s_peer, &msg);
            ESP_LOGI(TAG, "esp_peer_send_msg ret=%d", ret);
            free(queued_msg.data);
        }

        /*
         * Only touch the JPEG path once the DataChannel is up. During ICE/DTLS
         * handshake esp_peer_main_loop must run tight and uninterrupted; mixing
         * frame sends in here starves the agent's STUN/TURN retransmits and is
         * the classic cause of intermittent PAIRING->CONNECT_FAILED. This mirrors
         * Espressif's esp_webrtc.c, which only starts its media-send task on
         * ESP_PEER_STATE_CONNECTED and stops it on DISCONNECTED.
         */
        if (s_data_channel_ready) {
            send_queued_jpeg();
        } else {
            drop_pending_jpegs();
        }

        /*
         * esp_peer is cooperative: this call drives ICE, DTLS, SCTP,
         * retransmits, and DataChannel callbacks.
         */
        esp_peer_main_loop(s_peer);

        /*
         * 10ms matches Espressif's reference pc_task cadence. A 1ms spin wastes
         * CPU that the agent and lwIP need, and (when unpinned) lets higher-prio
         * WiFi/camera tasks preempt mid-handshake.
         */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void webrtc_peer_init(void)
{
    ESP_LOGI(TAG, "Pre-generating DTLS certificate...");
    int ret = esp_peer_pre_generate_cert();
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGW(TAG, "Pre-generate cert failed: %d", ret);
    }

    s_msg_queue = xQueueCreate(MSG_QUEUE_DEPTH, sizeof(queued_msg_t));
    if (!s_msg_queue) {
        ESP_LOGE(TAG, "Failed to create peer message queue");
        return;
    }

    s_jpeg_queue = xQueueCreate(JPEG_QUEUE_DEPTH, sizeof(queued_jpeg_t));
    if (!s_jpeg_queue) {
        ESP_LOGE(TAG, "Failed to create JPEG queue");
        return;
    }

    /*
     * TODO: DataChannel tuning for live JPEG preview frames.
     * Keep the send cache small so large VGA frames do not build up as stale
     * latency when the browser/network cannot drain them fast enough.
     */
    static esp_peer_default_cfg_t default_cfg = {
        /*
         * recv() ceiling per loop tick; also bounds STUN/TURN retransmit delay.
         * Keep small (100ms): STUN punches through UDP loss via fast retransmit,
         * so a lost packet should cost ~100ms, not 500ms. Raising this slows
         * loss recovery and pushes the relay candidate past the gathering
         * window on lossy paths. See CONFIG help for details.
         */
        .agent_recv_timeout = CONFIG_WEBRTC_AGENT_RECV_TIMEOUT_MS,
        .data_ch_cfg = {
            .cache_timeout   = CONFIG_WEBRTC_CACHE_TIMEOUT_MS,
            .send_cache_size = CONFIG_WEBRTC_SEND_CACHE_SIZE,
            .recv_cache_size = CONFIG_WEBRTC_RECV_CACHE_SIZE,
        },
        .alive_binding_retries = 0xFF,
    };

    /*
     * The browser creates the offer, so ESP is the controlled/answering peer.
     * manual_ch_create disables esp_peer's automatic "esp_channel"; this example
     * only uses the browser-created "jpeg-test" channel.
     */
    esp_peer_cfg_t cfg = {
        .server_lists = s_ice_servers,
        .server_num = 1,
        .role = ESP_PEER_ROLE_CONTROLLED,
        .ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL,
        .audio_dir = ESP_PEER_MEDIA_DIR_NONE,
        .video_dir = ESP_PEER_MEDIA_DIR_NONE,
        .enable_data_channel = true,
        .manual_ch_create = true,
        .no_auto_reconnect = true,
        .extra_cfg = &default_cfg,
        .extra_size = sizeof(default_cfg),
        .on_state = on_peer_state,
        .on_msg = on_peer_msg,
        .on_channel_open = on_channel_open,
        .on_data = on_data,
        .on_channel_close = on_channel_close,
        .ctx = NULL,
    };

    ret = esp_peer_open(&cfg, esp_peer_get_default_impl(), &s_peer);
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "esp_peer_open failed: %d", ret);
        return;
    }

    ESP_LOGI(TAG, "esp_peer opened, starting main loop task");
    /*
     * Pin the peer loop to core 1 at high priority, matching Espressif's
     * reference esp_webrtc.c scheduler (pc_task: core_id=1, priority=18,
     * stack=25KB). Core 0 runs the WiFi/lwIP stack; keeping the ICE/DTLS/SCTP
     * loop off that core and above the camera/MQTT tasks stops the handshake
     * from being preempted, which is what caused the intermittent
     * PAIRING->CONNECT_FAILED ("Failed to receive request") failures.
     */
    BaseType_t task_ok = xTaskCreatePinnedToCore(
        peer_task, "peer_loop", 20 * 1024, NULL, 18, NULL, 1);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create peer_loop task");
    }
}

void webrtc_peer_set_turn_credentials(const char *username, const char *credential)
{
    if (!username || !credential || !username[0] || !credential[0]) {
        ESP_LOGI(TAG, "No TURN credentials supplied, using STUN only");
        s_ice_server_count = 1;
        return;
    }

    strncpy(s_turn_user, username, sizeof(s_turn_user) - 1);
    s_turn_user[sizeof(s_turn_user) - 1] = '\0';
    strncpy(s_turn_psw, credential, sizeof(s_turn_psw) - 1);
    s_turn_psw[sizeof(s_turn_psw) - 1] = '\0';

    s_ice_servers[1] = (esp_peer_ice_server_cfg_t){
        .stun_url = s_turn_url,
        .user = s_turn_user,
        .psw = s_turn_psw,
    };
    s_ice_server_count = 2;
    ESP_LOGI(TAG, "TURN credentials set, will use relay %s", s_turn_url);
}

void webrtc_peer_on_offer(const char *sdp, size_t sdp_len)
{
    if (!sdp || sdp_len == 0) {
        ESP_LOGW(TAG, "Ignoring empty offer");
        return;
    }

    uint8_t *copy = malloc(sdp_len + 1);
    if (!copy) {
        ESP_LOGE(TAG, "OOM queueing offer");
        return;
    }
    memcpy(copy, sdp, sdp_len);
    copy[sdp_len] = '\0';

    s_start_new_connection = true;

    queued_msg_t msg = {
        .type = ESP_PEER_MSG_TYPE_SDP,
        .data = copy,
        .size = (int)sdp_len,
    };

    if (xQueueSend(s_msg_queue, &msg, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Offer queue full, dropping");
        free(copy);
        return;
    }

    ESP_LOGI(TAG, "Remote offer queued len=%d", (int)sdp_len);
}

bool webrtc_peer_data_channel_ready(void)
{
    return s_data_channel_ready;
}

bool webrtc_peer_send_text(const char *text, int len)
{
    if (!s_peer || !s_data_channel_ready || !text || len <= 0) {
        return false;
    }

    esp_peer_data_frame_t frame = {
        .type = ESP_PEER_DATA_CHANNEL_STRING,
        .stream_id = s_data_stream_id,
        .data = (uint8_t *)text,
        .size = len,
    };
    return esp_peer_send_data(s_peer, &frame) == ESP_PEER_ERR_NONE;
}

bool webrtc_peer_send_jpeg(const uint8_t *data, size_t len)
{
    if (!s_peer || !s_data_channel_ready || !s_jpeg_queue ||
        !data || len == 0 || len > INT_MAX) {
        return false;
    }

    /*
     * Copy because the camera buffer is returned immediately after this call.
     * Drop older pending frames so latency stays bounded.
     */
    drop_pending_jpegs();

    uint8_t *copy = malloc(len);
    if (!copy) {
        ESP_LOGW(TAG, "OOM copying JPEG len=%d", (int)len);
        return false;
    }
    memcpy(copy, data, len);

    queued_jpeg_t jpeg = {
        .data = copy,
        .size = (int)len,
        .owns_data = true,
    };
    if (xQueueSend(s_jpeg_queue, &jpeg, 0) != pdTRUE) {
        free(copy);
        return false;
    }

    return true;
}

bool webrtc_peer_send_jpeg_ref(const uint8_t *data, size_t len,
                               webrtc_peer_jpeg_release_cb_t release_cb,
                               void *ctx)
{
    if (!s_peer || !s_data_channel_ready || !s_jpeg_queue ||
        !data || len == 0 || len > INT_MAX || !release_cb) {
        return false;
    }

    /*
     * Drop older pending frames and release their camera buffers. The camera
     * task transfers ownership of this buffer until release_cb(ctx) runs in
     * peer_task after esp_peer_send_data() returns.
     */
    drop_pending_jpegs();

    queued_jpeg_t jpeg = {
        .data = (uint8_t *)data,
        .size = (int)len,
        .owns_data = false,
        .release_cb = release_cb,
        .release_ctx = ctx,
    };
    if (xQueueSend(s_jpeg_queue, &jpeg, 0) != pdTRUE) {
        return false;
    }

    return true;
}
