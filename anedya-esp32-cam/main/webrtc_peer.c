#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_peer.h"
#include "esp_peer_default.h"

#include "anedya_signaling.h"
#include "webrtc_peer.h"

static const char *TAG = "webrtc_peer";

// =============================================================================
// webrtc_peer.c — the WebRTC peer connection and the frame send pipeline.
//
// This file wraps Espressif's esp_peer library. Its responsibilities:
//   - Configure ICE (STUN, and TURN when the browser provides credentials).
//   - Accept the browser's SDP offer and produce an SDP answer (handed back to
//     the signaling layer in anedya_signaling.c).
//   - Run esp_peer's cooperative main loop on a dedicated, core-pinned task.
//   - Accept JPEG frames from the camera task and push them out the DataChannel.
//
// The device is the *answering* (controlled) peer: the browser always creates the
// offer and creates the DataChannel (named "jpeg-test").
//
// Video path is intentionally simple:
//   camera JPEG frame -> WebRTC DataChannel -> browser <img>
// This is NOT full WebRTC RTP video. It is a low-resolution educational preview
// that is easy to inspect from both C and JavaScript.
// =============================================================================

#define JPEG_DATA_CHANNEL_LABEL "jpeg-test"  // channel name the browser creates
#define MESSAGE_QUEUE_DEPTH 8                 // pending SDP/control messages
#define JPEG_QUEUE_DEPTH 1                    // only the newest frame is kept

#define TURN_URL "turn:turn1.ap-in-1.anedya.io:3478"
#define TURN_CREDENTIAL_MAX_LENGTH 128

// Peer-loop task. Pinned to core 1 (core 0 runs WiFi/lwIP) at a high priority so
// the ICE/DTLS/SCTP handshake is not preempted by WiFi/camera work — matching
// Espressif's reference esp_webrtc.c scheduler.
#define PEER_TASK_STACK_BYTES (20 * 1024)
#define PEER_TASK_PRIORITY 18
#define PEER_TASK_CORE 1
#define PEER_LOOP_PERIOD_MS 10               // cooperative main-loop tick cadence

#define MESSAGE_QUEUE_SEND_TIMEOUT_MS 1000   // wait when enqueueing an inbound offer
#define WOULD_BLOCK_LOG_INTERVAL 25          // log every Nth consecutive WOULD_BLOCK

// STUN-only by default. A TURN entry is appended when the browser sends
// credentials alongside the offer (see webrtc_peer_set_turn_credentials).
static esp_peer_ice_server_cfg_t s_ice_servers[2] = {
    {.stun_url = "stun:turn1.ap-in-1.anedya.io:3478", .user = NULL, .psw = NULL},
};
static int s_ice_server_count = 1;

static char s_turn_username[TURN_CREDENTIAL_MAX_LENGTH];
static char s_turn_password[TURN_CREDENTIAL_MAX_LENGTH];
static char s_turn_url[] = TURN_URL;

// A signaling/control message (an SDP answer) waiting to be fed into esp_peer on
// the peer task. Ownership of `data` transfers to the peer task, which frees it.
typedef struct {
    esp_peer_msg_type_t type;
    uint8_t *data;
    int size;
} queued_message_t;

// One JPEG frame waiting to be sent. The producer retains ownership of the buffer;
// we invoke release_callback(release_context) once the frame has been handed to
// esp_peer, which is how the camera task gets its frame buffer back (zero-copy).
typedef struct {
    uint8_t *data;
    int size;
    webrtc_peer_jpeg_release_callback_t release_callback;
    void *release_context;
} queued_jpeg_t;

static esp_peer_handle_t s_peer;            // esp_peer connection handle
static QueueHandle_t s_message_queue;       // inbound SDP/control -> peer task
static QueueHandle_t s_jpeg_queue;          // outbound JPEG frames -> peer task

// Cross-task flags. These are written on the esp_peer callback / signaling tasks
// and read on the peer-loop task (and, for s_data_channel_ready, the camera task).
// `_Atomic` gives each access sequentially-consistent load/store semantics — the
// correct tool for a lock-free cross-core flag. Plain `volatile` would only stop
// the compiler caching the value; it provides no cross-core ordering guarantee.
static _Atomic bool s_start_new_connection;   // request: (re)start a connection
static _Atomic bool s_data_channel_ready;     // is the jpeg-test channel open?
static uint16_t s_data_stream_id;             // SCTP stream id of that channel

// Map an esp_peer state enum to a readable string for logging.
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

// True for any state that means the connection is down or gone.
static bool is_disconnected_state(esp_peer_state_t state)
{
    return state == ESP_PEER_STATE_DISCONNECTED ||
           state == ESP_PEER_STATE_CONNECT_FAILED ||
           state == ESP_PEER_STATE_DATA_CHANNEL_CLOSED ||
           state == ESP_PEER_STATE_DATA_CHANNEL_DISCONNECTED ||
           state == ESP_PEER_STATE_CLOSED;
}

// esp_peer state-change callback. Fires as ICE/DTLS/SCTP progress. We use it to
// track DataChannel readiness and to report the final outcome back to signaling.
static int on_peer_state(esp_peer_state_t state, void *ctx)
{
    ESP_LOGI(TAG, "Peer state: %s (%d)", peer_state_name(state), (int)state);
    if (is_disconnected_state(state)) {
        // Stop feeding frames while down, but do NOT conclude the command here:
        // with auto-reconnect enabled, a DISCONNECTED / DATA_CHANNEL_DISCONNECTED is
        // often transient (e.g. NAT rebind) and esp_peer will re-run connectivity
        // checks. Only CONNECT_FAILED below is treated as terminal. Log at WARN so a
        // drop is visible and a drop/recover cycle is easy to spot in the monitor.
        s_data_channel_ready = false;
        ESP_LOGW(TAG, "Connection down (%s); waiting for esp_peer to recover",
                 peer_state_name(state));
    }

    // Report a terminal connection outcome back to the signaling command so both
    // Anedya and the browser learn the result. The data channel connecting is our
    // "fully working" signal (success); CONNECT_FAILED is the failure signal.
    // anedya_signaling_command_conclude is a no-op if the command was already concluded.
    if (state == ESP_PEER_STATE_DATA_CHANNEL_CONNECTED) {
        ESP_LOGI(TAG, "WebRTC connected: data channel is up");
        anedya_signaling_command_conclude(true, NULL);
    } else if (state == ESP_PEER_STATE_CONNECT_FAILED) {
        ESP_LOGE(TAG, "WebRTC connection failed (ICE/DTLS did not complete)");
        anedya_signaling_command_conclude(false, "webrtc connect failed");
    }
    return 0;
}

// esp_peer callback for outbound signaling messages it wants us to deliver to the
// remote peer. For this example that means the local SDP answer, which we forward
// to the Anedya signaling layer.
static int on_peer_msg(esp_peer_msg_t *msg, void *ctx)
{
    if (!msg || !msg->data || msg->size <= 0) {
        return 0;
    }

    // esp_peer hands us a byte range, not a C string. Copy to a null-terminated
    // buffer before passing it to the signaling helpers.
    char *payload = malloc(msg->size + 1);
    if (!payload) {
        ESP_LOGE(TAG, "OOM copying peer message");
        return -1;
    }
    memcpy(payload, msg->data, msg->size);
    payload[msg->size] = '\0';

    if (msg->type == ESP_PEER_MSG_TYPE_SDP) {
        ESP_LOGI(TAG, "Local SDP answer ready (%d bytes)", msg->size);
        anedya_signaling_write_answer(payload, msg->size);
    } else {
        // Local ICE candidates (type CANDIDATE) are expected and ignored on purpose:
        // we use non-trickle ICE, so the answer SDP is only emitted after gathering
        // completes and already carries all candidates inline. Log anything else so
        // an unexpected message type is visible while debugging.
        ESP_LOGD(TAG, "Ignoring peer msg type=%d (%d bytes)", (int)msg->type, msg->size);
    }

    free(payload);
    return 0;
}

// Callback when a DataChannel opens. We latch the stream id of the "jpeg-test"
// channel (the one the browser reads) and send a one-off hello so you can confirm
// the channel works before frames start flowing.
static int on_channel_open(esp_peer_data_channel_info_t *channel, void *ctx)
{
    const char *label = (channel && channel->label) ? channel->label : "";
    int stream_id = channel ? channel->stream_id : -1;
    ESP_LOGD(TAG, "Data channel opened: label=%s stream_id=%d", label, stream_id);

    // The browser creates the DataChannel named "jpeg-test". Ignore any other
    // channel so we only ever send JPEGs on the stream the browser is reading.
    if (!channel || strcmp(label, JPEG_DATA_CHANNEL_LABEL) != 0) {
        ESP_LOGD(TAG, "Ignoring non-JPEG data channel: label=%s", label);
        return 0;
    }

    s_data_stream_id = channel->stream_id;
    s_data_channel_ready = true;
    ESP_LOGI(TAG, "JPEG data channel open (stream_id=%d); ready to stream", channel->stream_id);

    // Send a one-off hello so you can confirm the channel works in the browser
    // console before frames start flowing.
    const char hello[] = "hello from esp32 data channel";
    esp_peer_data_frame_t frame = {
        .type = ESP_PEER_DATA_CHANNEL_STRING,
        .stream_id = channel->stream_id,
        .data = (uint8_t *)hello,
        .size = sizeof(hello) - 1,
    };
    int ret = esp_peer_send_data(s_peer, &frame);
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGW(TAG, "Failed to send data channel hello: %d", ret);
    }
    return 0;
}

// Callback for data received from the browser over the DataChannel. This example
// only logs it; the browser mostly sends control strings, not bulk data.
static int on_data(esp_peer_data_frame_t *frame, void *ctx)
{
    if (!frame || !frame->data || frame->size <= 0) {
        return 0;
    }
    ESP_LOGI(TAG, "Data channel rx: stream_id=%d size=%d text='%.*s'",
             frame->stream_id, frame->size, frame->size, (const char *)frame->data);
    return 0;
}

// Callback when a DataChannel closes. If it was our jpeg-test channel, mark the
// stream not-ready so the producer stops handing us frames.
static int on_channel_close(esp_peer_data_channel_info_t *channel, void *ctx)
{
    const char *label = (channel && channel->label) ? channel->label : "";
    ESP_LOGI(TAG, "Data channel closed: label=%s", label);
    if (!channel || strcmp(label, JPEG_DATA_CHANNEL_LABEL) == 0 ||
        channel->stream_id == s_data_stream_id) {
        s_data_channel_ready = false;
    }
    return 0;
}

// Return a queued frame's buffer to its producer via the release callback.
static void release_queued_jpeg(const queued_jpeg_t *jpeg)
{
    if (jpeg && jpeg->release_callback) {
        jpeg->release_callback(jpeg->release_context);
    }
}

// Empty the JPEG queue, releasing each frame's buffer. Called when we cannot send
// (channel down) or to make room, so stale frames never pile up as latency.
static void drop_pending_jpegs(void)
{
    if (!s_jpeg_queue) {
        return;
    }

    queued_jpeg_t jpeg;
    while (xQueueReceive(s_jpeg_queue, &jpeg, 0) == pdTRUE) {
        release_queued_jpeg(&jpeg);
    }
}

// Dequeue one frame and push it out the DataChannel. WOULD_BLOCK is normal back
// pressure (SCTP send buffer full) — we just drop the frame and move on rather
// than stall, since live video prefers freshness over completeness.
static void send_queued_jpeg(void)
{
    static uint32_t s_would_block_count;  // consecutive WOULD_BLOCKs, for throttled logging

    // The caller (peer_task) only invokes this while the channel is ready and drains
    // the queue otherwise, so a missing peer/queue here just means nothing to do.
    if (!s_peer || !s_jpeg_queue) {
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
        if (s_would_block_count == 1 || (s_would_block_count % WOULD_BLOCK_LOG_INTERVAL) == 0) {
            ESP_LOGW(TAG, "JPEG send would block len=%d count=%lu",
                     jpeg.size, (unsigned long)s_would_block_count);
        }
    } else {
        ESP_LOGE(TAG, "JPEG send failed len=%d ret=%d", jpeg.size, ret);
    }

    release_queued_jpeg(&jpeg);
}

// The single task that drives everything WebRTC. esp_peer is cooperative (not
// thread-safe and not self-driven), so ALL esp_peer_* calls happen here: starting
// connections, feeding offers, sending frames, and pumping its main loop. Keeping
// it on one core-pinned task is what makes the handshake reliable.
static void peer_task(void *arg)
{
    for (;;) {
        // (Re)start a connection when the receive path requests it.
        if (s_start_new_connection) {
            s_start_new_connection = false;
            s_data_channel_ready = false;
            s_data_stream_id = 0;

            // Tear down any prior session first. Without this, a leftover or
            // previously-failed connection makes esp_peer_update_ice_info return
            // -3 (invalid state) and the new connection inherits stale agent
            // state. esp_peer_disconnect is safe to call even when idle.
            esp_peer_disconnect(s_peer);

            int ice_ret = esp_peer_update_ice_info(s_peer, ESP_PEER_ROLE_CONTROLLED,
                                                    s_ice_servers, s_ice_server_count);
            ESP_LOGD(TAG, "update_ice_info servers=%d ret=%d", s_ice_server_count, ice_ret);
            if (ice_ret == ESP_PEER_ERR_NONE) {
                ESP_LOGI(TAG, "Starting new WebRTC connection (%d ICE server%s)",
                         s_ice_server_count, s_ice_server_count == 1 ? "" : "s");
                int ret = esp_peer_new_connection(s_peer);
                if (ret != ESP_PEER_ERR_NONE) {
                    ESP_LOGE(TAG, "esp_peer_new_connection failed: %d", ret);
                }
            } else {
                ESP_LOGE(TAG, "update_ice_info failed (ret=%d); not starting connection",
                         ice_ret);
            }
        }

        // Feed any queued signaling messages (the remote offer) into esp_peer.
        queued_message_t queued_message;
        while (xQueueReceive(s_message_queue, &queued_message, 0) == pdTRUE) {
            esp_peer_msg_t message = {
                .type = queued_message.type,
                .data = queued_message.data,
                .size = queued_message.size,
            };

            ESP_LOGD(TAG, "Feeding peer message type=%d size=%d starts='%.32s'",
                     (int)message.type, message.size, (const char *)message.data);
            int result = esp_peer_send_msg(s_peer, &message);
            if (result != ESP_PEER_ERR_NONE) {
                ESP_LOGE(TAG, "esp_peer_send_msg failed: %d", result);
            }
            free(queued_message.data);
        }

        // Only touch the JPEG path once the DataChannel is up. During the ICE/DTLS
        // handshake esp_peer_main_loop must run tight and uninterrupted; mixing
        // frame sends in here starves the agent's STUN/TURN retransmits and is the
        // classic cause of intermittent PAIRING->CONNECT_FAILED. This mirrors
        // Espressif's esp_webrtc.c, which only starts its media-send task once
        // connected and stops it on disconnect.
        if (s_data_channel_ready) {
            send_queued_jpeg();
        } else {
            drop_pending_jpegs();
        }

        // Pump esp_peer once. Being cooperative, this single call is what actually
        // drives ICE, DTLS, SCTP, retransmits, and fires the DataChannel callbacks.
        esp_peer_main_loop(s_peer);

        // This cadence matches Espressif's reference. A 1ms spin wastes CPU that the
        // ICE agent and lwIP need; a longer delay slows the handshake. Note the task
        // is core-pinned and high priority (see webrtc_peer_init) so this loop is
        // not preempted mid-handshake by WiFi/camera work.
        vTaskDelay(pdMS_TO_TICKS(PEER_LOOP_PERIOD_MS));
    }
}

// Public entry point (called from app_main). Sets up esp_peer, the message/frame
// queues, and the core-pinned peer task, then returns. The device is now ready to
// accept an offer via webrtc_peer_on_offer(). Declared in webrtc_peer.h.
void webrtc_peer_init(void)
{
    // DTLS needs a certificate; generating it up front keeps it out of the
    // latency-sensitive handshake path later.
    ESP_LOGI(TAG, "Pre-generating DTLS certificate...");
    int ret = esp_peer_pre_generate_cert();
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGW(TAG, "Pre-generate cert failed: %d", ret);
    }

    s_message_queue = xQueueCreate(MESSAGE_QUEUE_DEPTH, sizeof(queued_message_t));
    if (!s_message_queue) {
        ESP_LOGE(TAG, "Failed to create peer message queue");
        return;
    }

    s_jpeg_queue = xQueueCreate(JPEG_QUEUE_DEPTH, sizeof(queued_jpeg_t));
    if (!s_jpeg_queue) {
        ESP_LOGE(TAG, "Failed to create JPEG queue");
        return;
    }

    // esp_peer implementation-specific tuning for live JPEG preview frames. The
    // send cache is deliberately kept small (via menuconfig) so large frames do
    // not build up as stale latency when the browser/network cannot drain them
    // fast enough. All values are configurable under menuconfig.
    static esp_peer_default_cfg_t default_cfg = {
        // recv() ceiling per loop tick; also bounds STUN/TURN retransmit delay.
        // Keep it small (~100ms): STUN punches through UDP loss via fast
        // retransmit, so a lost packet should cost ~100ms, not 500ms. Raising this
        // slows loss recovery and can push the relay candidate past the gathering
        // window on lossy paths. See the Kconfig help for the full reasoning.
        .agent_recv_timeout = CONFIG_WEBRTC_AGENT_RECV_TIMEOUT_MS,
        .data_ch_cfg = {
            .cache_timeout   = CONFIG_WEBRTC_CACHE_TIMEOUT_MS,
            .send_cache_size = CONFIG_WEBRTC_SEND_CACHE_SIZE,
            .recv_cache_size = CONFIG_WEBRTC_RECV_CACHE_SIZE,
        },
        .alive_binding_retries = 0xFF,
    };

    // The browser creates the offer, so the ESP is the controlled/answering peer.
    // manual_ch_create disables esp_peer's automatic "esp_channel"; this example
    // only uses the browser-created "jpeg-test" channel. Media (audio/video RTP)
    // is disabled because frames go over the DataChannel instead.
    //
    // no_auto_reconnect = false lets esp_peer recover from a transient ICE drop
    // (e.g. a NAT UDP-mapping rebind mid-stream) by re-running connectivity checks
    // against the existing remote candidates, instead of tearing the session down on
    // the first blip. This is why an otherwise-healthy stream previously died after a
    // few minutes: with reconnect disabled, one ICE "disconnected" was fatal. Note
    // this recovers the *same* session; if the browser's mapping also changes, a full
    // ICE restart (new browser offer) is still required.
    esp_peer_cfg_t cfg = {
        .server_lists = s_ice_servers,
        .server_num = 1,
        .role = ESP_PEER_ROLE_CONTROLLED,
        .ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL,
        .audio_dir = ESP_PEER_MEDIA_DIR_NONE,
        .video_dir = ESP_PEER_MEDIA_DIR_NONE,
        .enable_data_channel = true,
        .manual_ch_create = true,
        .no_auto_reconnect = true, // TODO
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
    // See PEER_TASK_* above for why this loop is core-pinned and high priority:
    // keeping it off core 0 and above the camera/MQTT tasks stops the handshake
    // from being preempted, the root cause of the intermittent
    // PAIRING->CONNECT_FAILED failures.
    BaseType_t task_ok = xTaskCreatePinnedToCore(
        peer_task, "peer_loop", PEER_TASK_STACK_BYTES, NULL,
        PEER_TASK_PRIORITY, NULL, PEER_TASK_CORE);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create peer_loop task");
    }
}

// Store TURN relay credentials for the next connection. The browser fetches these
// from Anedya and sends them with the offer; TURN is only needed when a direct
// peer-to-peer path is blocked. Passing NULL/empty falls back to STUN-only.
// Declared in webrtc_peer.h.
void webrtc_peer_set_turn_credentials(const char *username, const char *credential)
{
    if (!username || !credential || !username[0] || !credential[0]) {
        ESP_LOGI(TAG, "No TURN credentials supplied, using STUN only");
        s_ice_server_count = 1;
        return;
    }

    // Warn on truncation: silently cutting a credential would make TURN auth fail
    // in a way that is very hard to diagnose from the ICE logs alone.
    if (strlen(username) >= sizeof(s_turn_username) ||
        strlen(credential) >= sizeof(s_turn_password)) {
        ESP_LOGW(TAG, "TURN credential too long (max %d), truncating; relay auth may fail",
                 TURN_CREDENTIAL_MAX_LENGTH - 1);
    }

    strncpy(s_turn_username, username, sizeof(s_turn_username) - 1);
    s_turn_username[sizeof(s_turn_username) - 1] = '\0';
    strncpy(s_turn_password, credential, sizeof(s_turn_password) - 1);
    s_turn_password[sizeof(s_turn_password) - 1] = '\0';

    s_ice_servers[1] = (esp_peer_ice_server_cfg_t){
        .stun_url = s_turn_url,
        .user = s_turn_username,
        .psw = s_turn_password,
    };
    s_ice_server_count = 2;
    ESP_LOGI(TAG, "TURN credentials set, will use relay %s", s_turn_url);
}

// Accept a remote SDP offer from the signaling layer. We copy it, flag that a new
// connection should start, and hand it to the peer task via the message queue —
// all esp_peer work must happen on that task. Declared in webrtc_peer.h.
void webrtc_peer_on_offer(const char *sdp, size_t sdp_len)
{
    if (!sdp || sdp_len == 0) {
        ESP_LOGW(TAG, "Ignoring empty offer");
        return;
    }

    uint8_t *sdp_copy = malloc(sdp_len + 1);
    if (!sdp_copy) {
        ESP_LOGE(TAG, "Out of memory queueing offer");
        return;
    }
    memcpy(sdp_copy, sdp, sdp_len);
    sdp_copy[sdp_len] = '\0';

    s_start_new_connection = true;

    queued_message_t message = {
        .type = ESP_PEER_MSG_TYPE_SDP,
        .data = sdp_copy,
        .size = (int)sdp_len,
    };

    if (xQueueSend(s_message_queue, &message, pdMS_TO_TICKS(MESSAGE_QUEUE_SEND_TIMEOUT_MS))
            != pdTRUE) {
        ESP_LOGE(TAG, "Offer queue full, dropping");
        free(sdp_copy);
        return;
    }

    ESP_LOGI(TAG, "Remote offer queued len=%d", (int)sdp_len);
}

// True once the browser's jpeg-test DataChannel is open. Declared in webrtc_peer.h.
bool webrtc_peer_data_channel_ready(void)
{
    return s_data_channel_ready;
}

// Send a short text string over the DataChannel (used by test mode). Returns false
// if the channel is not ready or the send fails. Declared in webrtc_peer.h.
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

// Queue a JPEG frame without copying it (zero-copy). The caller transfers ownership
// of the buffer until release_callback(context) is invoked on the peer task once the
// frame has been handed to esp_peer. This is the path the camera task uses so a frame
// is sent straight from the camera's PSRAM buffer. Declared in webrtc_peer.h.
bool webrtc_peer_send_jpeg_by_reference(const uint8_t *data, size_t len,
                                        webrtc_peer_jpeg_release_callback_t release_callback,
                                        void *context)
{
    if (!s_peer || !s_data_channel_ready || !s_jpeg_queue ||
        !data || len == 0 || len > INT_MAX || !release_callback) {
        return false;
    }

    // Drop any older pending frame (releasing its buffer) so only the newest frame
    // is queued. Ownership of this buffer is held until release_callback(context)
    // runs in peer_task after esp_peer_send_data() returns.
    drop_pending_jpegs();

    queued_jpeg_t jpeg = {
        .data = (uint8_t *)data,
        .size = (int)len,
        .release_callback = release_callback,
        .release_context = context,
    };
    if (xQueueSend(s_jpeg_queue, &jpeg, 0) != pdTRUE) {
        return false;
    }

    return true;
}
