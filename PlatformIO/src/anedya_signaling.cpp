// =============================================================================
// anedya_signaling.cpp — WebRTC signaling over Anedya Commands (MQTT).
//
// "Signaling" is how two WebRTC peers exchange session descriptions (SDP)
// before media can flow: the browser sends an *offer*, the device replies with
// an *answer*. Normally that needs a signaling server; here we reuse Anedya's
// Commands feature, so there is no extra server to run.
//
// The command id is the only thing tying an answer back to its offer:
//
//   Browser -> ESP:  POST /commands/send  command="webrtc_offer"
//                    data = base64(deflate-raw(offer JSON))     [datatype string]
//
//   ESP -> Browser:  publish to $anedya/device/<id>/commands/updateStatus/json
//                    received   -> "got it"
//                    processing -> ackdata = base64(deflate-raw(answer SDP))
//                    success    -> WebRTC connected   (terminal)
//                    failure    -> could not connect  (terminal)
//                    The browser polls /commands/getDetails for that command.
//
// SDP is deflated and base64'd because a full SDP is larger than the ~1 KB
// command payload budget.
//
// Arduino port note: the ESP-IDF version of this example used the Anedya
// ESP-IDF SDK. Arduino has no such SDK, so this talks to the same MQTT
// endpoints directly with PubSubClient + ArduinoJson — the same libraries
// Anedya's own Arduino examples use. The bytes on the wire are identical, so
// the same browser viewer drives either firmware.
// =============================================================================

#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "miniz.h"  // tdefl/tinfl live in ESP32 ROM — see esp_rom/include/miniz.h

#include "anedya_signaling.h"
#include "app_config.h"
#include "webrtc_peer.h"

static const char *TAG = "anedya_signaling";

// Anedya Root CA 3 (ECC-256). https://docs.anedya.io/device/mqtt-endpoints/#tls
static const char ANEDYA_ROOT_CA[] = R"EOF(
-----BEGIN CERTIFICATE-----
MIICDDCCAbOgAwIBAgITQxd3Dqj4u/74GrImxc0M4EbUvDAKBggqhkjOPQQDAjBL
MQswCQYDVQQGEwJJTjEQMA4GA1UECBMHR3VqYXJhdDEPMA0GA1UEChMGQW5lZHlh
MRkwFwYDVQQDExBBbmVkeWEgUm9vdCBDQSAzMB4XDTI0MDEwMTAwMDAwMFoXDTQz
MTIzMTIzNTk1OVowSzELMAkGA1UEBhMCSU4xEDAOBgNVBAgTB0d1amFyYXQxDzAN
BgNVBAoTBkFuZWR5YTEZMBcGA1UEAxMQQW5lZHlhIFJvb3QgQ0EgMzBZMBMGByqG
SM49AgEGCCqGSM49AwEHA0IABKsxf0vpbjShIOIGweak0/meIYS0AmXaujinCjFk
BFShcaf2MdMeYBPPFwz4p5I8KOCopgshSTUFRCXiiKwgYPKjdjB0MA8GA1UdEwEB
/wQFMAMBAf8wHQYDVR0OBBYEFNz1PBRXdRsYQNVsd3eYVNdRDcH4MB8GA1UdIwQY
MBaAFNz1PBRXdRsYQNVsd3eYVNdRDcH4MA4GA1UdDwEB/wQEAwIBhjARBgNVHSAE
CjAIMAYGBFUdIAAwCgYIKoZIzj0EAwIDRwAwRAIgR/rWSG8+L4XtFLces0JYS7bY
5NH1diiFk54/E5xmSaICIEYYbhvjrdR0GVLjoay6gFspiRZ7GtDDr9xF91WbsK0P
-----END CERTIFICATE-----
)EOF";

// Command status strings, as Anedya expects them. success/failure are terminal.
static const char *STATUS_RECEIVED   = "received";
static const char *STATUS_PROCESSING = "processing";
static const char *STATUS_SUCCESS    = "success";
static const char *STATUS_FAILURE    = "failure";

#define COMMAND_ID_MAX_LENGTH 40  // 36-char UUID + NUL, rounded up
#define STATUS_QUEUE_DEPTH    4
#define STATUS_PUBLISH_TRIES  3

// One pending command status update. The MQTT publish must not happen on the
// esp_peer task (PubSubClient is not thread-safe, and a blocking TLS write from
// that high-priority task would stall the ICE/DTLS handshake), so producers
// push here and loop() drains it.
//   status  — one of the STATUS_* literals above (never freed).
//   ackData — optional heap-owned NUL-terminated payload; freed after publish.
struct StatusItem {
    char commandId[COMMAND_ID_MAX_LENGTH];
    const char *status;
    char *ackData;
    size_t ackDataLength;
};

static WiFiClientSecure s_tlsClient;
static PubSubClient s_mqtt(s_tlsClient);

static char s_topicCommands[96];
static char s_topicStatus[128];
static char s_topicHeartbeat[96];
static char s_topicResponse[96];
static char s_topicErrors[96];

static QueueHandle_t s_statusQueue;
static SemaphoreHandle_t s_commandMutex;

// The command id of the in-flight offer. Written on the loop task (when a
// command arrives) and read on the esp_peer task (when the answer or the final
// outcome is ready), hence the mutex. One camera, one viewer, one at a time.
static char s_activeCommandId[COMMAND_ID_MAX_LENGTH];
static bool s_haveActiveCommand;

static uint32_t s_lastHeartbeatMs;
static uint32_t s_lastConnectAttemptMs;

// -----------------------------------------------------------------------------
// Active-command bookkeeping
// -----------------------------------------------------------------------------

static void setActiveCommand(const char *commandId)
{
    xSemaphoreTake(s_commandMutex, portMAX_DELAY);
    strncpy(s_activeCommandId, commandId, sizeof(s_activeCommandId) - 1);
    s_activeCommandId[sizeof(s_activeCommandId) - 1] = '\0';
    s_haveActiveCommand = true;
    xSemaphoreGive(s_commandMutex);
}

// Copy the active command id into `out`. If `clear` is true the command is also
// marked concluded, so a later callback cannot update a terminal command.
// Returns false when there is no active command.
static bool takeActiveCommand(char *out, size_t outSize, bool clear)
{
    xSemaphoreTake(s_commandMutex, portMAX_DELAY);
    bool have = s_haveActiveCommand;
    if (have) {
        strncpy(out, s_activeCommandId, outSize - 1);
        out[outSize - 1] = '\0';
        if (clear) {
            s_haveActiveCommand = false;
        }
    }
    xSemaphoreGive(s_commandMutex);
    return have;
}

// -----------------------------------------------------------------------------
// Status update queue
// -----------------------------------------------------------------------------

// Queue a status update. Takes ownership of ackData (may be nullptr).
static bool queueStatus(const char *commandId, const char *status, char *ackData,
                        size_t ackDataLength)
{
    if (!s_statusQueue) {
        free(ackData);
        return false;
    }

    StatusItem item = {};
    strncpy(item.commandId, commandId, sizeof(item.commandId) - 1);
    item.status = status;
    item.ackData = ackData;
    item.ackDataLength = ackDataLength;

    if (xQueueSend(s_statusQueue, &item, 0) != pdTRUE) {
        ESP_LOGE(TAG, "Status queue full, dropping status=%s", status);
        free(ackData);
        return false;
    }
    return true;
}

// Conclude the in-flight command as failed with a short reason, and clear it so
// nothing later tries to update a now-terminal command.
static void failActiveCommand(const char *reason)
{
    char commandId[COMMAND_ID_MAX_LENGTH];
    if (!takeActiveCommand(commandId, sizeof(commandId), true)) {
        return;
    }
    char *reasonCopy = reason ? strdup(reason) : nullptr;
    queueStatus(commandId, STATUS_FAILURE, reasonCopy, reasonCopy ? strlen(reasonCopy) : 0);
}

// Publish one status update. Runs on the loop task only.
static bool publishStatus(const StatusItem &item)
{
    // ackdata is always base64 or a short ASCII reason, so neither needs JSON
    // escaping — a plain concat is safe and avoids a second large buffer.
    size_t capacity = item.ackDataLength + 200;
    char *payload = (char *)malloc(capacity);
    if (!payload) {
        ESP_LOGE(TAG, "Out of memory building status payload");
        return false;
    }

    int length = snprintf(payload, capacity,
                          "{\"reqId\":\"\",\"commandId\":\"%s\",\"status\":\"%s\","
                          "\"ackdata\":\"%s\",\"ackdatatype\":\"%s\"}",
                          item.commandId, item.status,
                          item.ackData ? item.ackData : "",
                          item.ackData ? "string" : "");

    bool published = false;
    if (length > 0 && (size_t)length < capacity) {
        for (int attempt = 1; attempt <= STATUS_PUBLISH_TRIES && !published; attempt++) {
            published = s_mqtt.publish(s_topicStatus, (const uint8_t *)payload, length, false);
            if (!published) {
                ESP_LOGW(TAG, "Status publish attempt %d failed (len=%d)", attempt, length);
                s_mqtt.loop();
            }
        }
    } else {
        ESP_LOGE(TAG, "Status payload did not fit (%d bytes needed)", length);
    }

    if (published) {
        ESP_LOGI(TAG, "Command status=%s sent (%u bytes ack data)", item.status,
                 (unsigned)item.ackDataLength);
    }
    free(payload);
    return published;
}

// -----------------------------------------------------------------------------
// Offer receive path: base64 -> inflate -> JSON -> esp_peer
// -----------------------------------------------------------------------------

// Decode the browser's offer payload into a heap-allocated, NUL-terminated JSON
// string (caller frees). Returns nullptr on failure, having already concluded
// the command as failed.
//
// Buffers are heap-allocated on purpose: this runs inside the MQTT callback on
// the Arduino loop task, and tinfl_decompressor alone is ~11 KB of state.
static char *decodeOffer(const char *base64Data, size_t base64Length)
{
    uint8_t *deflateBuffer = (uint8_t *)malloc(OFFER_DEFLATE_MAX_BYTES);
    if (!deflateBuffer) {
        ESP_LOGE(TAG, "Out of memory allocating deflate buffer");
        failActiveCommand("out of memory");
        return nullptr;
    }

    // Step 1: base64 -> raw deflate bytes. mbedtls' decoder is length-aware and
    // reports BUFFER_TOO_SMALL rather than overflowing, so an oversized offer is
    // rejected here instead of corrupting the heap.
    size_t deflateLength = 0;
    int rc = mbedtls_base64_decode(deflateBuffer, OFFER_DEFLATE_MAX_BYTES, &deflateLength,
                                   (const unsigned char *)base64Data, base64Length);
    if (rc != 0) {
        ESP_LOGE(TAG, "base64 decode failed: -0x%x (encoded=%u bytes, buffer=%d)",
                 -rc, (unsigned)base64Length, OFFER_DEFLATE_MAX_BYTES);
        free(deflateBuffer);
        failActiveCommand(rc == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL ? "offer too large"
                                                                   : "base64 decode failed");
        return nullptr;
    }
    ESP_LOGD(TAG, "Offer base64 decoded: %u bytes", (unsigned)deflateLength);

    // Step 2: inflate back to the offer JSON. The convenient
    // tinfl_decompress_mem_to_mem() puts the ~11 KB decompressor struct on the
    // stack; we use the low-level API and heap-allocate it instead.
    tinfl_decompressor *decompressor = (tinfl_decompressor *)malloc(sizeof(tinfl_decompressor));
    char *offerJson = (char *)malloc(OFFER_JSON_MAX_BYTES);
    if (!decompressor || !offerJson) {
        ESP_LOGE(TAG, "Out of memory allocating decompressor/offer buffer");
        free(decompressor);
        free(offerJson);
        free(deflateBuffer);
        failActiveCommand("out of memory");
        return nullptr;
    }

    tinfl_init(decompressor);
    size_t inputBytes = deflateLength;
    size_t outputBytes = OFFER_JSON_MAX_BYTES - 1;  // leave room for the NUL
    tinfl_status status = tinfl_decompress(
        decompressor,
        (const mz_uint8 *)deflateBuffer, &inputBytes,
        (mz_uint8 *)offerJson, (mz_uint8 *)offerJson, &outputBytes,
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(decompressor);
    free(deflateBuffer);

    if (status < 0) {
        ESP_LOGE(TAG, "tinfl decompression failed: status=%d", (int)status);
        free(offerJson);
        failActiveCommand("decompress failed");
        return nullptr;
    }
    offerJson[outputBytes] = '\0';
    ESP_LOGD(TAG, "Offer inflated to JSON: %u bytes", (unsigned)outputBytes);
    return offerJson;
}

// Parse the offer JSON and hand its SDP + optional TURN credentials to the
// WebRTC layer, which connects and produces the answer asynchronously.
// Shape: {"sdp":"...", "turn":{"username":"...", "credential":"..."}}
static void parseAndDispatchOffer(const char *offerJson)
{
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, offerJson);
    if (error) {
        ESP_LOGE(TAG, "Offer JSON parse failed: %s", error.c_str());
        failActiveCommand("bad offer json");
        return;
    }

    const char *sdp = doc["sdp"];
    if (!sdp) {
        ESP_LOGE(TAG, "Offer JSON missing 'sdp' field");
        failActiveCommand("missing sdp");
        return;
    }

    // TURN credentials are optional; without them the peer falls back to STUN.
    const char *turnUsername = doc["turn"]["username"];
    const char *turnCredential = doc["turn"]["credential"];
    webrtcPeerSetTurnCredentials(turnUsername, turnCredential);

    // The WebRTC layer calls anedyaSignalingWriteAnswer() once ICE gathering
    // completes and esp_peer has produced the answer.
    webrtcPeerOnOffer(sdp, strlen(sdp));
}

// Handle one incoming Anedya command. Every failure path concludes the command
// as "failed" so the browser stops waiting.
static void handleCommand(JsonDocument &doc)
{
    const char *name = doc["command"];
    if (!name || strcmp(name, ANEDYA_OFFER_COMMAND_NAME) != 0) {
        ESP_LOGD(TAG, "Ignoring command: %s", name ? name : "(none)");
        return;
    }

    const char *commandId = doc["commandId"];
    if (!commandId) {
        ESP_LOGE(TAG, "webrtc_offer command has no commandId");
        return;
    }

    // Record the command id up front so any failure below can conclude *this*
    // command, and so the esp_peer callbacks know what to acknowledge.
    setActiveCommand(commandId);

    const char *dataType = doc["datatype"];
    if (dataType && strcmp(dataType, "string") != 0) {
        ESP_LOGE(TAG, "webrtc_offer has unexpected datatype '%s'", dataType);
        failActiveCommand("bad datatype");
        return;
    }

    const char *data = doc["data"];
    if (!data) {
        ESP_LOGE(TAG, "webrtc_offer command has no data");
        failActiveCommand("missing data");
        return;
    }

    ESP_LOGI(TAG, "WebRTC offer received (%u encoded bytes)", (unsigned)strlen(data));

    // Acknowledge receipt before doing any work.
    // Status flow: received -> processing(+answer) -> success | failure.
    queueStatus(commandId, STATUS_RECEIVED, nullptr, 0);

    char *offerJson = decodeOffer(data, strlen(data));
    if (!offerJson) {
        return;  // decodeOffer already concluded the command as failed
    }
    parseAndDispatchOffer(offerJson);
    free(offerJson);
}

// PubSubClient message callback. Runs on the loop task, inside s_mqtt.loop().
static void mqttCallback(char *topic, uint8_t *payload, unsigned int length)
{
    if (strcmp(topic, s_topicCommands) != 0) {
        // /response and /errors are only useful while debugging.
        ESP_LOGD(TAG, "MQTT rx on %s: %.*s", topic, (int)length, (const char *)payload);
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload, length);
    if (error) {
        ESP_LOGE(TAG, "Command JSON parse failed: %s (len=%u)", error.c_str(), length);
        return;
    }
    handleCommand(doc);
}

// -----------------------------------------------------------------------------
// Connection management
// -----------------------------------------------------------------------------

// One non-blocking connect attempt. Anedya's own Arduino examples spin here
// with delay(5000); we return instead so the camera keeps streaming to an
// already-connected viewer while MQTT is down.
static void tryConnect()
{
    ESP_LOGI(TAG, "Connecting to Anedya broker %s:%d ...", ANEDYA_MQTT_HOST, ANEDYA_MQTT_PORT);
    if (!s_mqtt.connect(ANEDYA_DEVICE_ID, ANEDYA_DEVICE_ID, ANEDYA_CONNECTION_KEY)) {
        ESP_LOGE(TAG, "Anedya broker connect failed, rc=%d (retry in %d ms)",
                 s_mqtt.state(), ANEDYA_MQTT_RETRY_PERIOD_MS);
        return;
    }

    ESP_LOGI(TAG, "Connected to Anedya broker");
    s_mqtt.subscribe(s_topicCommands);
    s_mqtt.subscribe(s_topicResponse);
    s_mqtt.subscribe(s_topicErrors);
    ESP_LOGI(TAG, "Anedya signaling ready — waiting for a viewer");
}

static void sendHeartbeat()
{
    // Anedya marks a node offline if heartbeats stop arriving.
    if (!s_mqtt.publish(s_topicHeartbeat, "{}")) {
        ESP_LOGW(TAG, "Heartbeat publish failed");
    }
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

void anedyaSignalingBegin()
{
    if (ANEDYA_DEVICE_ID[0] == '\0' || ANEDYA_CONNECTION_KEY[0] == '\0') {
        ESP_LOGE(TAG, "Anedya credentials are blank — set ANEDYA_DEVICE_ID and "
                      "ANEDYA_CONNECTION_KEY in include/app_config.h");
        return;
    }

    snprintf(s_topicCommands, sizeof(s_topicCommands),
             "$anedya/device/%s/commands", ANEDYA_DEVICE_ID);
    snprintf(s_topicStatus, sizeof(s_topicStatus),
             "$anedya/device/%s/commands/updateStatus/json", ANEDYA_DEVICE_ID);
    snprintf(s_topicHeartbeat, sizeof(s_topicHeartbeat),
             "$anedya/device/%s/heartbeat/json", ANEDYA_DEVICE_ID);
    snprintf(s_topicResponse, sizeof(s_topicResponse),
             "$anedya/device/%s/response", ANEDYA_DEVICE_ID);
    snprintf(s_topicErrors, sizeof(s_topicErrors),
             "$anedya/device/%s/errors", ANEDYA_DEVICE_ID);

    s_statusQueue = xQueueCreate(STATUS_QUEUE_DEPTH, sizeof(StatusItem));
    s_commandMutex = xSemaphoreCreateMutex();
    if (!s_statusQueue || !s_commandMutex) {
        ESP_LOGE(TAG, "Failed to create signaling primitives");
        return;
    }

    s_tlsClient.setCACert(ANEDYA_ROOT_CA);
    s_mqtt.setServer(ANEDYA_MQTT_HOST, ANEDYA_MQTT_PORT);
    s_mqtt.setKeepAlive(ANEDYA_MQTT_KEEPALIVE_S);
    s_mqtt.setCallback(mqttCallback);
    // A command carrying a compressed SDP offer is ~1.4 KB; PubSubClient's
    // 256-byte default would silently drop it.
    if (!s_mqtt.setBufferSize(ANEDYA_MQTT_BUFFER_BYTES)) {
        ESP_LOGE(TAG, "Could not grow the MQTT buffer to %d bytes", ANEDYA_MQTT_BUFFER_BYTES);
    }

    s_lastHeartbeatMs = millis();
    s_lastConnectAttemptMs = millis();
    tryConnect();
}

void anedyaSignalingLoop()
{
    if (!s_statusQueue) {
        return;  // begin() bailed out (missing credentials)
    }

    if (!s_mqtt.connected()) {
        if (millis() - s_lastConnectAttemptMs >= ANEDYA_MQTT_RETRY_PERIOD_MS) {
            s_lastConnectAttemptMs = millis();
            tryConnect();
        }
        return;
    }

    s_mqtt.loop();

    // Drain queued status updates produced by the esp_peer task.
    StatusItem item;
    while (xQueueReceive(s_statusQueue, &item, 0) == pdTRUE) {
        if (!publishStatus(item)) {
            ESP_LOGE(TAG, "Giving up on status update (status=%s)", item.status);
        }
        free(item.ackData);
    }

    if (millis() - s_lastHeartbeatMs >= ANEDYA_HEARTBEAT_PERIOD_MS) {
        s_lastHeartbeatMs = millis();
        sendHeartbeat();
    }
}

bool anedyaSignalingConnected()
{
    return s_mqtt.connected();
}

void anedyaSignalingWriteAnswer(const char *sdp, size_t sdpLength)
{
    char commandId[COMMAND_ID_MAX_LENGTH];
    if (!takeActiveCommand(commandId, sizeof(commandId), false)) {
        ESP_LOGE(TAG, "Answer ready but no active command to acknowledge");
        return;
    }

    // Mirror of the offer decode: deflate, then base64, so the answer fits the
    // ~1 KB ack-data budget. tdefl_compressor state is large, so keep it off
    // this caller's stack — this runs on the esp_peer task.
    tdefl_compressor *compressor = (tdefl_compressor *)malloc(sizeof(tdefl_compressor));
    uint8_t *deflateBuffer = (uint8_t *)malloc(ANSWER_DEFLATE_MAX_BYTES);
    if (!compressor || !deflateBuffer) {
        ESP_LOGE(TAG, "Out of memory allocating compressor/deflate buffer");
        free(compressor);
        free(deflateBuffer);
        failActiveCommand("out of memory");
        return;
    }

    // Raw deflate (no zlib header) to match the browser's "deflate-raw".
    // Omitting TDEFL_WRITE_ZLIB_HEADER is what makes it raw.
    tdefl_init(compressor, nullptr, nullptr, TDEFL_DEFAULT_MAX_PROBES);
    size_t inputBytes = sdpLength;
    size_t outputBytes = ANSWER_DEFLATE_MAX_BYTES;
    tdefl_status deflateStatus = tdefl_compress(compressor, sdp, &inputBytes, deflateBuffer,
                                                &outputBytes, TDEFL_FINISH);
    free(compressor);
    if (deflateStatus != TDEFL_STATUS_DONE) {
        // TDEFL_STATUS_OKAY together with TDEFL_FINISH means the output buffer
        // filled before the whole SDP was consumed — the answer is simply too
        // large. Call that out separately from a real compressor error.
        if (deflateStatus == TDEFL_STATUS_OKAY) {
            ESP_LOGE(TAG, "Answer too large: %u-byte SDP does not fit %d-byte deflate buffer",
                     (unsigned)sdpLength, ANSWER_DEFLATE_MAX_BYTES);
            failActiveCommand("answer too large");
        } else {
            ESP_LOGE(TAG, "deflate failed: status=%d", (int)deflateStatus);
            failActiveCommand("answer deflate failed");
        }
        free(deflateBuffer);
        return;
    }
    size_t deflateLength = outputBytes;

    size_t base64Capacity = ((deflateLength + 2) / 3) * 4 + 1;
    char *base64 = (char *)malloc(base64Capacity);
    if (!base64) {
        ESP_LOGE(TAG, "Out of memory allocating base64 buffer");
        free(deflateBuffer);
        failActiveCommand("out of memory");
        return;
    }
    size_t base64Length = 0;
    int rc = mbedtls_base64_encode((unsigned char *)base64, base64Capacity, &base64Length,
                                   deflateBuffer, deflateLength);
    free(deflateBuffer);
    if (rc != 0) {
        ESP_LOGE(TAG, "base64 encode failed: -0x%x", -rc);
        free(base64);
        failActiveCommand("answer base64 failed");
        return;
    }
    base64[base64Length] = '\0';

    ESP_LOGI(TAG, "WebRTC answer ready, sending to browser");
    ESP_LOGD(TAG, "Answer sizes: raw=%uB -> deflate=%uB -> base64=%uB",
             (unsigned)sdpLength, (unsigned)deflateLength, (unsigned)base64Length);

    // Sent as "processing", not "success": success/failure are terminal in
    // Anedya, so we only conclude once WebRTC actually connects or fails (see
    // anedyaSignalingConclude). The browser reads the answer out of ackdata
    // while the status is "processing". queueStatus takes ownership of base64,
    // and the command stays active so the conclusion can reference it.
    queueStatus(commandId, STATUS_PROCESSING, base64, base64Length);
}

void anedyaSignalingConclude(bool success, const char *reason)
{
    char commandId[COMMAND_ID_MAX_LENGTH];
    if (!takeActiveCommand(commandId, sizeof(commandId), true)) {
        ESP_LOGW(TAG, "Conclude requested but no active command");
        return;
    }

    if (success) {
        queueStatus(commandId, STATUS_SUCCESS, nullptr, 0);
    } else {
        char *reasonCopy = reason ? strdup(reason) : nullptr;
        queueStatus(commandId, STATUS_FAILURE, reasonCopy, reasonCopy ? strlen(reasonCopy) : 0);
    }
}
