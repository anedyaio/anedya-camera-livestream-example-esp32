// =============================================================================
// webrtc_peer.cpp — the WebRTC peer connection and the frame send pipeline.
//
// Wraps Espressif's esp_peer. Responsibilities:
//   - Configure ICE (STUN always; TURN when the browser supplies credentials).
//   - Accept the browser's SDP offer and produce an SDP answer, handed back to
//     the signaling layer in anedya_signaling.cpp.
//   - Run esp_peer's cooperative main loop on a dedicated, core-pinned task.
//   - Accept JPEG frames from loop() and push them out the DataChannel.
//
// The device is the *answering* (controlled) peer: the browser always creates
// the offer and creates the DataChannel.
//
// The video path is deliberately simple:
//   camera JPEG frame -> WebRTC DataChannel -> browser <img>
// That is not RTP video. It is an easy-to-inspect preview that reads the same
// from C++ and from JavaScript.
//
// Why this is a FreeRTOS task and not part of loop(): esp_peer is cooperative
// and not thread-safe, so every esp_peer_* call must happen on one task, and
// that task must tick tightly (10 ms) at high priority or the ICE/DTLS
// handshake gets preempted mid-flight. loop() does blocking TLS I/O for MQTT,
// which would wreck that. This mirrors Espressif's own esp_webrtc.c scheduler.
// =============================================================================

#include <Arduino.h>
#include <atomic>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_peer.h"
#include "esp_peer_default.h"

#include "anedya_signaling.h"
#include "app_config.h"
#include "webrtc_peer.h"

static const char *TAG = "webrtc_peer";

#define MESSAGE_QUEUE_DEPTH 8  // pending SDP/control messages
#define JPEG_QUEUE_DEPTH    1  // only the newest frame is kept

#define TURN_CREDENTIAL_MAX_LENGTH 128

// Peer-loop task. Pinned to core 1 (core 0 runs WiFi/lwIP) at a high priority
// so the ICE/DTLS/SCTP handshake is not preempted by WiFi or camera work.
#define PEER_TASK_STACK_BYTES (20 * 1024)
#define PEER_TASK_PRIORITY    18
#define PEER_TASK_CORE        1
#define PEER_LOOP_PERIOD_MS   10  // cooperative main-loop tick cadence

#define MESSAGE_QUEUE_SEND_TIMEOUT_MS 1000  // wait when enqueueing an inbound offer
#define WOULD_BLOCK_LOG_INTERVAL      25    // log every Nth consecutive WOULD_BLOCK

// STUN-only by default; a TURN entry is appended when the browser sends
// credentials alongside the offer (see webrtcPeerSetTurnCredentials).
static esp_peer_ice_server_cfg_t s_iceServers[2] = {
    {.stun_url = (char *)ANEDYA_STUN_URL, .user = nullptr, .psw = nullptr},
};
static int s_iceServerCount = 1;

static char s_turnUsername[TURN_CREDENTIAL_MAX_LENGTH];
static char s_turnPassword[TURN_CREDENTIAL_MAX_LENGTH];
static char s_turnUrl[] = ANEDYA_TURN_URL;

// A signaling/control message waiting to be fed into esp_peer on the peer task.
// Ownership of `data` transfers to that task, which frees it.
struct QueuedMessage {
    esp_peer_msg_type_t type;
    uint8_t *data;
    int size;
};

// One JPEG frame waiting to be sent. The producer retains ownership of the
// buffer; releaseCallback(releaseContext) runs once the frame has been handed
// to esp_peer, which is how the camera gets its frame buffer back (zero-copy).
struct QueuedJpeg {
    uint8_t *data;
    int size;
    WebrtcPeerJpegRelease releaseCallback;
    void *releaseContext;
};

static esp_peer_handle_t s_peer;
static QueueHandle_t s_messageQueue;  // inbound SDP/control -> peer task
static QueueHandle_t s_jpegQueue;     // outbound JPEG frames -> peer task

// Cross-task flags: written on the esp_peer callback / signaling tasks, read on
// the peer task (and, for s_dataChannelReady, on loop()). std::atomic gives
// each access sequentially-consistent semantics — the right tool for a
// lock-free cross-core flag. Plain `volatile` only stops the compiler caching
// the value; it guarantees nothing about cross-core ordering.
static std::atomic<bool> s_startNewConnection{false};
static std::atomic<bool> s_dataChannelReady{false};
static uint16_t s_dataStreamId;  // SCTP stream id of the JPEG channel

static const char *peerStateName(esp_peer_state_t state)
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

static bool isDisconnectedState(esp_peer_state_t state)
{
    return state == ESP_PEER_STATE_DISCONNECTED ||
           state == ESP_PEER_STATE_CONNECT_FAILED ||
           state == ESP_PEER_STATE_DATA_CHANNEL_CLOSED ||
           state == ESP_PEER_STATE_DATA_CHANNEL_DISCONNECTED ||
           state == ESP_PEER_STATE_CLOSED;
}

// esp_peer state-change callback. Fires as ICE/DTLS/SCTP progress.
static int onPeerState(esp_peer_state_t state, void *ctx)
{
    ESP_LOGI(TAG, "Peer state: %s (%d)", peerStateName(state), (int)state);

    if (isDisconnectedState(state)) {
        // Stop feeding frames while down, but do NOT conclude the command here:
        // with auto-reconnect on, a DISCONNECTED is often transient (e.g. a NAT
        // rebind) and esp_peer re-runs connectivity checks. Only CONNECT_FAILED
        // below is terminal. WARN so a drop/recover cycle is visible.
        s_dataChannelReady = false;
        ESP_LOGW(TAG, "Connection down (%s); waiting for esp_peer to recover",
                 peerStateName(state));
    }

    // Report the terminal outcome back to the signaling command so both Anedya
    // and the browser learn the result. Data channel up = fully working.
    if (state == ESP_PEER_STATE_DATA_CHANNEL_CONNECTED) {
        ESP_LOGI(TAG, "WebRTC connected: data channel is up");
        anedyaSignalingConclude(true, nullptr);
    } else if (state == ESP_PEER_STATE_CONNECT_FAILED) {
        ESP_LOGE(TAG, "WebRTC connection failed (ICE/DTLS did not complete)");
        anedyaSignalingConclude(false, "webrtc connect failed");
    }
    return 0;
}

// esp_peer wants us to deliver a signaling message to the remote peer. For this
// example that means the local SDP answer.
static int onPeerMsg(esp_peer_msg_t *msg, void *ctx)
{
    if (!msg || !msg->data || msg->size <= 0) {
        return 0;
    }

    // esp_peer hands us a byte range, not a C string.
    char *payload = (char *)malloc(msg->size + 1);
    if (!payload) {
        ESP_LOGE(TAG, "Out of memory copying peer message");
        return -1;
    }
    memcpy(payload, msg->data, msg->size);
    payload[msg->size] = '\0';

    if (msg->type == ESP_PEER_MSG_TYPE_SDP) {
        ESP_LOGI(TAG, "Local SDP answer ready (%d bytes)", msg->size);
        anedyaSignalingWriteAnswer(payload, msg->size);
    } else {
        // Local ICE candidates are ignored on purpose: this uses non-trickle
        // ICE, so the answer SDP is only emitted after gathering completes and
        // already carries every candidate inline.
        ESP_LOGD(TAG, "Ignoring peer msg type=%d (%d bytes)", (int)msg->type, msg->size);
    }

    free(payload);
    return 0;
}

// A DataChannel opened. Latch the stream id of the channel the browser reads
// and send a one-off hello so you can confirm it works before frames flow.
static int onChannelOpen(esp_peer_data_channel_info_t *channel, void *ctx)
{
    const char *label = (channel && channel->label) ? channel->label : "";
    ESP_LOGD(TAG, "Data channel opened: label=%s stream_id=%d", label,
             channel ? channel->stream_id : -1);

    if (!channel || strcmp(label, WEBRTC_DATA_CHANNEL_LABEL) != 0) {
        ESP_LOGD(TAG, "Ignoring non-JPEG data channel: label=%s", label);
        return 0;
    }

    s_dataStreamId = channel->stream_id;
    s_dataChannelReady = true;
    ESP_LOGI(TAG, "JPEG data channel open (stream_id=%d); ready to stream",
             channel->stream_id);

    const char hello[] = "hello from esp32 data channel";
    esp_peer_data_frame_t frame = {};
    frame.type = ESP_PEER_DATA_CHANNEL_STRING;
    frame.stream_id = channel->stream_id;
    frame.data = (uint8_t *)hello;
    frame.size = sizeof(hello) - 1;

    int ret = esp_peer_send_data(s_peer, &frame);
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGW(TAG, "Failed to send data channel hello: %d", ret);
    }
    return 0;
}

// Data received from the browser. The browser mostly sends short control
// strings, so this just logs.
static int onData(esp_peer_data_frame_t *frame, void *ctx)
{
    if (!frame || !frame->data || frame->size <= 0) {
        return 0;
    }
    ESP_LOGI(TAG, "Data channel rx: stream_id=%d size=%d text='%.*s'",
             frame->stream_id, frame->size, frame->size, (const char *)frame->data);
    return 0;
}

static int onChannelClose(esp_peer_data_channel_info_t *channel, void *ctx)
{
    const char *label = (channel && channel->label) ? channel->label : "";
    ESP_LOGI(TAG, "Data channel closed: label=%s", label);
    if (!channel || strcmp(label, WEBRTC_DATA_CHANNEL_LABEL) == 0 ||
        channel->stream_id == s_dataStreamId) {
        s_dataChannelReady = false;
    }
    return 0;
}

static void releaseQueuedJpeg(const QueuedJpeg &jpeg)
{
    if (jpeg.releaseCallback) {
        jpeg.releaseCallback(jpeg.releaseContext);
    }
}

// Empty the JPEG queue, returning each frame's buffer. Called when we cannot
// send (channel down) or to make room, so stale frames never become latency.
static void dropPendingJpegs()
{
    if (!s_jpegQueue) {
        return;
    }
    QueuedJpeg jpeg;
    while (xQueueReceive(s_jpegQueue, &jpeg, 0) == pdTRUE) {
        releaseQueuedJpeg(jpeg);
    }
}

// Dequeue one frame and push it out the DataChannel. WOULD_BLOCK is normal back
// pressure (SCTP send buffer full) — drop the frame and move on, since live
// video prefers freshness over completeness.
static void sendQueuedJpeg()
{
    static uint32_t s_wouldBlockCount;  // consecutive WOULD_BLOCKs, for log throttling

    if (!s_peer || !s_jpegQueue) {
        return;
    }

    QueuedJpeg jpeg;
    if (xQueueReceive(s_jpegQueue, &jpeg, 0) != pdTRUE) {
        return;
    }

    esp_peer_data_frame_t frame = {};
    frame.type = ESP_PEER_DATA_CHANNEL_DATA;
    frame.stream_id = s_dataStreamId;
    frame.data = jpeg.data;
    frame.size = jpeg.size;

    int ret = esp_peer_send_data(s_peer, &frame);
    if (ret == ESP_PEER_ERR_NONE) {
        s_wouldBlockCount = 0;
    } else if (ret == ESP_PEER_ERR_WOULD_BLOCK) {
        s_wouldBlockCount++;
        if (s_wouldBlockCount == 1 || (s_wouldBlockCount % WOULD_BLOCK_LOG_INTERVAL) == 0) {
            ESP_LOGW(TAG, "JPEG send would block len=%d count=%lu",
                     jpeg.size, (unsigned long)s_wouldBlockCount);
        }
    } else {
        ESP_LOGE(TAG, "JPEG send failed len=%d ret=%d", jpeg.size, ret);
    }

    releaseQueuedJpeg(jpeg);
}

// The single task that drives everything WebRTC. Every esp_peer_* call happens
// here: starting connections, feeding offers, sending frames, pumping the loop.
static void peerTask(void *arg)
{
    for (;;) {
        // (Re)start a connection when the receive path asks for one.
        if (s_startNewConnection.exchange(false)) {
            s_dataChannelReady = false;
            s_dataStreamId = 0;

            // Tear down any prior session first. Without this, a leftover or
            // previously-failed connection makes esp_peer_update_ice_info
            // return -3 (invalid state) and the new connection inherits stale
            // agent state. Safe to call even when idle.
            esp_peer_disconnect(s_peer);

            int iceRet = esp_peer_update_ice_info(s_peer, ESP_PEER_ROLE_CONTROLLED,
                                                  s_iceServers, s_iceServerCount);
            ESP_LOGD(TAG, "update_ice_info servers=%d ret=%d", s_iceServerCount, iceRet);
            if (iceRet == ESP_PEER_ERR_NONE) {
                ESP_LOGI(TAG, "Starting new WebRTC connection (%d ICE server%s)",
                         s_iceServerCount, s_iceServerCount == 1 ? "" : "s");
                int ret = esp_peer_new_connection(s_peer);
                if (ret != ESP_PEER_ERR_NONE) {
                    ESP_LOGE(TAG, "esp_peer_new_connection failed: %d", ret);
                }
            } else {
                ESP_LOGE(TAG, "update_ice_info failed (ret=%d); not starting connection", iceRet);
            }
        }

        // Feed queued signaling messages (the remote offer) into esp_peer.
        QueuedMessage queued;
        while (xQueueReceive(s_messageQueue, &queued, 0) == pdTRUE) {
            esp_peer_msg_t message = {};
            message.type = queued.type;
            message.data = queued.data;
            message.size = queued.size;

            ESP_LOGD(TAG, "Feeding peer message type=%d size=%d",
                     (int)message.type, message.size);
            int result = esp_peer_send_msg(s_peer, &message);
            if (result != ESP_PEER_ERR_NONE) {
                ESP_LOGE(TAG, "esp_peer_send_msg failed: %d", result);
            }
            free(queued.data);
        }

        // Only touch the JPEG path once the DataChannel is up. During the
        // ICE/DTLS handshake esp_peer_main_loop must run tight and
        // uninterrupted; mixing frame sends in starves the agent's STUN/TURN
        // retransmits, the classic cause of intermittent
        // PAIRING -> CONNECT_FAILED.
        if (s_dataChannelReady) {
            sendQueuedJpeg();
        } else {
            dropPendingJpegs();
        }

        // Pump esp_peer once. Being cooperative, this single call is what
        // actually drives ICE, DTLS, SCTP, retransmits, and fires the
        // DataChannel callbacks.
        esp_peer_main_loop(s_peer);

        // A 1 ms spin wastes CPU the ICE agent and lwIP need; a longer delay
        // slows the handshake. 10 ms matches Espressif's reference.
        vTaskDelay(pdMS_TO_TICKS(PEER_LOOP_PERIOD_MS));
    }
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

void webrtcPeerBegin()
{
    // DTLS needs a certificate; generating it up front keeps the EC keygen out
    // of the latency-sensitive handshake path later.
    ESP_LOGI(TAG, "Pre-generating DTLS certificate...");
    int ret = esp_peer_pre_generate_cert();
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGW(TAG, "Pre-generate cert failed: %d", ret);
    }

    s_messageQueue = xQueueCreate(MESSAGE_QUEUE_DEPTH, sizeof(QueuedMessage));
    s_jpegQueue = xQueueCreate(JPEG_QUEUE_DEPTH, sizeof(QueuedJpeg));
    if (!s_messageQueue || !s_jpegQueue) {
        ESP_LOGE(TAG, "Failed to create peer queues");
        return;
    }

    // esp_peer tuning for live JPEG frames. The send cache is deliberately
    // bounded so large frames do not pile up as stale latency when the browser
    // or the network cannot drain them. See config.h for why each value is
    // what it is (short version: the TURN relay path needs the headroom).
    static esp_peer_default_cfg_t defaultCfg = {};
    defaultCfg.agent_recv_timeout = WEBRTC_AGENT_RECV_TIMEOUT_MS;
    defaultCfg.data_ch_cfg.cache_timeout = WEBRTC_CACHE_TIMEOUT_MS;
    defaultCfg.data_ch_cfg.send_cache_size = WEBRTC_SEND_CACHE_SIZE;
    defaultCfg.data_ch_cfg.recv_cache_size = WEBRTC_RECV_CACHE_SIZE;
    defaultCfg.alive_binding_retries = 0xFF;

    // The browser creates the offer, so the device is the controlled/answering
    // peer. manual_ch_create disables esp_peer's automatic "esp_channel"; this
    // example only uses the browser-created channel. Media (audio/video RTP) is
    // off because frames ride the DataChannel instead.
    //
    // no_auto_reconnect = false lets esp_peer recover from a transient ICE drop
    // (e.g. a NAT UDP-mapping rebind mid-stream) by re-running connectivity
    // checks against the existing remote candidates, instead of tearing the
    // session down on the first blip. Note this recovers the *same* session; if
    // the browser's mapping also changes, a fresh offer is still required.
    esp_peer_cfg_t cfg = {};
    cfg.server_lists = s_iceServers;
    cfg.server_num = 1;
    cfg.role = ESP_PEER_ROLE_CONTROLLED;
    cfg.ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL;
    cfg.audio_dir = ESP_PEER_MEDIA_DIR_NONE;
    cfg.video_dir = ESP_PEER_MEDIA_DIR_NONE;
    cfg.enable_data_channel = true;
    cfg.manual_ch_create = true;
    cfg.no_auto_reconnect = false;
    cfg.extra_cfg = &defaultCfg;
    cfg.extra_size = sizeof(defaultCfg);
    cfg.on_state = onPeerState;
    cfg.on_msg = onPeerMsg;
    cfg.on_channel_open = onChannelOpen;
    cfg.on_data = onData;
    cfg.on_channel_close = onChannelClose;
    cfg.ctx = nullptr;

    ret = esp_peer_open(&cfg, esp_peer_get_default_impl(), &s_peer);
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "esp_peer_open failed: %d", ret);
        return;
    }

    ESP_LOGI(TAG, "esp_peer opened, starting main loop task");
    BaseType_t taskOk = xTaskCreatePinnedToCore(
        peerTask, "peer_loop", PEER_TASK_STACK_BYTES, nullptr,
        PEER_TASK_PRIORITY, nullptr, PEER_TASK_CORE);
    if (taskOk != pdPASS) {
        ESP_LOGE(TAG, "Failed to create peer_loop task");
    }
}

void webrtcPeerSetTurnCredentials(const char *username, const char *credential)
{
    if (!username || !credential || !username[0] || !credential[0]) {
        ESP_LOGI(TAG, "No TURN credentials supplied, using STUN only");
        s_iceServerCount = 1;
        return;
    }

    // Warn on truncation: silently cutting a credential makes TURN auth fail in
    // a way that is very hard to diagnose from the ICE logs alone.
    if (strlen(username) >= sizeof(s_turnUsername) ||
        strlen(credential) >= sizeof(s_turnPassword)) {
        ESP_LOGW(TAG, "TURN credential too long (max %d), truncating; relay auth may fail",
                 TURN_CREDENTIAL_MAX_LENGTH - 1);
    }

    strncpy(s_turnUsername, username, sizeof(s_turnUsername) - 1);
    s_turnUsername[sizeof(s_turnUsername) - 1] = '\0';
    strncpy(s_turnPassword, credential, sizeof(s_turnPassword) - 1);
    s_turnPassword[sizeof(s_turnPassword) - 1] = '\0';

    s_iceServers[1].stun_url = s_turnUrl;
    s_iceServers[1].user = s_turnUsername;
    s_iceServers[1].psw = s_turnPassword;
    s_iceServerCount = 2;
    ESP_LOGI(TAG, "TURN credentials set, will use relay %s", s_turnUrl);
}

void webrtcPeerOnOffer(const char *sdp, size_t sdpLength)
{
    if (!sdp || sdpLength == 0) {
        ESP_LOGW(TAG, "Ignoring empty offer");
        return;
    }

    uint8_t *sdpCopy = (uint8_t *)malloc(sdpLength + 1);
    if (!sdpCopy) {
        ESP_LOGE(TAG, "Out of memory queueing offer");
        return;
    }
    memcpy(sdpCopy, sdp, sdpLength);
    sdpCopy[sdpLength] = '\0';

    s_startNewConnection = true;

    QueuedMessage message = {};
    message.type = ESP_PEER_MSG_TYPE_SDP;
    message.data = sdpCopy;
    message.size = (int)sdpLength;

    if (xQueueSend(s_messageQueue, &message,
                   pdMS_TO_TICKS(MESSAGE_QUEUE_SEND_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Offer queue full, dropping");
        free(sdpCopy);
        return;
    }

    ESP_LOGI(TAG, "Remote offer queued len=%u", (unsigned)sdpLength);
}

bool webrtcPeerDataChannelReady()
{
    return s_dataChannelReady;
}

bool webrtcPeerSendText(const char *text, int length)
{
    if (!s_peer || !s_dataChannelReady || !text || length <= 0) {
        return false;
    }

    esp_peer_data_frame_t frame = {};
    frame.type = ESP_PEER_DATA_CHANNEL_STRING;
    frame.stream_id = s_dataStreamId;
    frame.data = (uint8_t *)text;
    frame.size = length;
    return esp_peer_send_data(s_peer, &frame) == ESP_PEER_ERR_NONE;
}

bool webrtcPeerSendJpegByReference(const uint8_t *data, size_t length,
                                   WebrtcPeerJpegRelease releaseCallback, void *context)
{
    if (!s_peer || !s_dataChannelReady || !s_jpegQueue ||
        !data || length == 0 || length > INT_MAX || !releaseCallback) {
        return false;
    }

    // Drop any older pending frame (releasing its buffer) so only the newest
    // frame is queued. Ownership of this buffer is held until
    // releaseCallback(context) runs on the peer task after esp_peer_send_data.
    dropPendingJpegs();

    QueuedJpeg jpeg = {};
    jpeg.data = (uint8_t *)data;
    jpeg.size = (int)length;
    jpeg.releaseCallback = releaseCallback;
    jpeg.releaseContext = context;

    return xQueueSend(s_jpegQueue, &jpeg, 0) == pdTRUE;
}
