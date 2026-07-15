#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "anedya.h"
#include "anedya_commons.h"
#include "anedya_json_parse.h"
#include "anedya_models.h"
#include "anedya_op_commands.h"
#include "anedya_operations.h"
#include "mbedtls/base64.h"
#include "miniz.h"

#include "anedya_signaling.h"
#include "webrtc_peer.h"

static const char *TAG = "anedya_signaling";

// =============================================================================
// anedya_signaling.c — WebRTC signaling over Anedya Commands.
//
// "Signaling" is how the two WebRTC peers exchange their session descriptions
// (SDP) before any media can flow: the browser sends an *offer*, the device
// replies with an *answer*. Normally you'd run a signaling server for this; here
// we reuse Anedya's Commands feature so no extra server is needed.
//
// The flow (the command id is the only thing that ties an answer to its offer):
//
//   Browser -> ESP:  POST /commands/send  name="webrtc_offer"
//                    data = base64(deflate(offer JSON))          [string datatype]
//
//   ESP -> Browser:  anedya_op_cmd_status_update(command id, status, ack data)
//                    received   -> "got it"
//                    processing -> ack data = base64(deflate(answer SDP))
//                    success    -> WebRTC connected  (terminal)
//                    failure    -> could not connect (terminal)
//                    The browser polls POST /commands/status {"id": command id}.
//
// SDP is compressed (raw deflate) and base64-encoded because a full SDP is larger
// than the command payload budget. Commands arrive on the Anedya MQTT task, and
// esp_peer produces the answer on its own task — so we keep those callbacks fast
// by pushing the actual MQTT publish onto a small background queue
// (signaling_transmit_task).
// =============================================================================

#define MQTT_CONNECTED_BIT BIT0            // event-group bit: set while MQTT is connected
#define SIGNALING_TX_QUEUE_DEPTH 4         // pending status-update slots in the transmit queue
#define SIGNALING_TX_RETRIES 3             // publish attempts before giving up on one update
#define SIGNALING_TX_RETRY_DELAY_MS 500    // wait between publish retries
#define SIGNALING_TX_QUEUE_SEND_TIMEOUT_MS 1000  // wait when enqueueing a status update
#define SIGNALING_TX_TASK_STACK_BYTES 12288 // peak ~8820B measured on S3 (SDK JSON+MQTT publish of base64 ack) + margin
#define SIGNALING_TX_TASK_PRIORITY 4       // FreeRTOS priority for the transmit task

#define HEARTBEAT_PERIOD_MS 30000          // how often to heartbeat Anedya (online status)
#define HEARTBEAT_TASK_STACK_BYTES 8192    // peak ~7172B measured on S3 (TLS/JSON publish) + margin
#define HEARTBEAT_TASK_PRIORITY 3          // FreeRTOS priority for the heartbeat task

#define STATUS_UPDATE_TIMEOUT_MS 10000     // wait for one status update to complete
#define MQTT_CONNECT_TIMEOUT_MS 30000      // wait for the MQTT connection at startup
#define ANEDYA_REQUEST_TIMEOUT_MS 30000    // per-request timeout configured on the SDK client

// Working-buffer sizes for the offer receive path. The decoded/inflated offer must
// fit within these; oversized input is rejected up front (see decode_offer) rather
// than overflowing a fixed buffer.
#define OFFER_DEFLATE_MAX_BYTES 800        // raw deflate bytes after base64 decode
#define OFFER_JSON_MAX_BYTES 2048          // inflated offer JSON (incl. NUL terminator)

// Working-buffer size for the answer transmit path.
#define ANSWER_DEFLATE_MAX_BYTES 1024      // raw deflate bytes of the compressed answer

#define ANEDYA_DEVICE_ID_STRING_SIZE 37    // 36-char UUID + NUL terminator
#define OFFER_JSON_MAX_TOKENS 12           // json_t pool size for the tiny-json parser

// Background queue item: a command status update to publish.
//   status    = ANEDYA_CMD_STATUS_* ("processing" for the answer, "success"/
//               "failure" to conclude).
//   ack_data  = optional null-terminated string payload (the answer SDP for
//               "processing", a short reason for "failure", NULL otherwise).
//               Heap-owned; freed by the transmit task.
typedef struct {
    anedya_uuid_t command_id;
    anedya_cmd_status_t status;
    char *ack_data;
    size_t ack_data_length;
} signaling_tx_item_t;

// The command id of the in-flight offer. esp_peer's answer callback fires on the
// peer-loop task; it reads this to know which command to acknowledge. Single
// active connection at a time (one camera, one viewer).
static anedya_uuid_t s_active_command_id;
static bool s_have_active_command;

static anedya_config_t s_config;
static anedya_client_t s_client;
static EventGroupHandle_t s_connection_events;
static QueueHandle_t s_transmit_queue;

// Anedya SDK operations are asynchronous: you submit a transaction and get called
// back when it completes. This callback simply wakes the task that is waiting on
// that transaction (via a task notification), turning the async API into a
// blocking one for our purposes.
static void transaction_notify_task(anedya_txn_t *transaction, anedya_context_t context)
{
    TaskHandle_t *task = (TaskHandle_t *)context;
    xTaskNotify(*task, 0x01, eSetValueWithOverwrite);
}

// Send a heartbeat to Anedya so the cloud knows the device is online, and block
// until it completes (or times out). Anedya marks a device offline if it stops
// receiving heartbeats. Returns true on success. Skips the attempt (returns
// false) if MQTT is not connected. Runs on the heartbeat task, which has enough
// stack for the TLS/JSON publish path (app_main's stack does not).
static bool send_heartbeat(void)
{
    if (!s_connection_events ||
        (xEventGroupGetBits(s_connection_events) & MQTT_CONNECTED_BIT) == 0) {
        ESP_LOGW(TAG, "Skipping heartbeat: MQTT not connected");
        return false;
    }

    TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    anedya_txn_t transaction = {0};
    uint32_t notification = 0;

    anedya_txn_register_callback(&transaction, transaction_notify_task, &caller);
    anedya_err_t error = anedya_device_send_heartbeat(&s_client, &transaction);
    if (error != ANEDYA_OK) {
        ESP_LOGE(TAG, "Heartbeat submit failed: %s", anedya_err_to_name(error));
        return false;
    }

    xTaskNotifyWait(0x00, ULONG_MAX, &notification, pdMS_TO_TICKS(STATUS_UPDATE_TIMEOUT_MS));
    if (notification == 0x01 && transaction.is_success) {
        ESP_LOGI(TAG, "Heartbeat sent to Anedya");
        return true;
    }

    ESP_LOGE(TAG, "Heartbeat failed/timeout notification=0x%lx complete=%d success=%d op_err=%d",
             (unsigned long)notification, transaction.is_complete, transaction.is_success,
             transaction._op_err);

    // Release the SDK transaction slot if it never completed (see
    // command_status_blocking for the rationale — slots are a limited resource).
    if (!transaction.is_complete && transaction.desc > 0) {
        transaction.callback = NULL;
        transaction.ctx = NULL;
        _anedya_txn_store_release_slot(&s_client.txn_store, &transaction);
    }
    return false;
}

// Heartbeat task. Waits until MQTT is connected, then heartbeats Anedya once per
// HEARTBEAT_PERIOD_MS forever. Runs on its own task so the blocking publish (TLS,
// JSON, base64) has adequate stack — the app_main task does not.
static void heartbeat_task(void *argument)
{
    int connection_check_counter=0;
    for (;;) {
        if ((xEventGroupGetBits(s_connection_events) & MQTT_CONNECTED_BIT) == 0) {
            connection_check_counter++;
            if(connection_check_counter>20){
                esp_restart();
            }
        }else{
            ESP_LOGW(TAG, "Sending heartbeat to Anedya");
            send_heartbeat();
            connection_check_counter=0;
        }
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
    }
}

// Publish a status update for command_id and block until it completes (or times
// out). Optional ack_data (may be NULL) is sent as a string datatype so the SDK's
// strlen-based base64 path is bypassed (it would NUL-truncate raw binary). Runs
// only on the transmit task, which is allowed to block on the network.
static bool command_status_blocking(const anedya_uuid_t command_id, anedya_cmd_status_t status,
                                    char *ack_data, size_t ack_data_length)
{
    TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    anedya_txn_t transaction = {0};
    uint32_t notification = 0;

    anedya_req_cmd_status_update_t request = {
        .status = status,
        .data = (unsigned char *)ack_data,
        .data_len = ack_data ? ack_data_length : 0,
        .data_type = ANEDYA_DATATYPE_STRING,
    };
    memcpy(request.cmdId, command_id, sizeof(request.cmdId));

    anedya_txn_register_callback(&transaction, transaction_notify_task, &caller);
    anedya_err_t error = anedya_op_cmd_status_update(&s_client, &transaction, &request);
    if (error != ANEDYA_OK) {
        ESP_LOGE(TAG, "Command status update submit failed: %d", error);
        return false;
    }

    xTaskNotifyWait(0x00, ULONG_MAX, &notification, pdMS_TO_TICKS(STATUS_UPDATE_TIMEOUT_MS));
    if (notification == 0x01 && transaction.is_success) {
        ESP_LOGI(TAG, "Command status=%s set (%d bytes ack data)",
                 status, (int)(ack_data ? ack_data_length : 0));
        return true;
    }

    ESP_LOGE(TAG, "Command status update failed/timeout notification=0x%lx complete=%d success=%d op_err=%d",
             (unsigned long)notification, transaction.is_complete, transaction.is_success,
             transaction._op_err);

    // If the SDK transaction never completed (e.g. we timed out waiting), release
    // its slot before retrying. This mirrors the SDK's normal cleanup path and
    // prevents leaking transaction slots, which are a fixed, limited resource.
    if (!transaction.is_complete && transaction.desc > 0) {
        transaction.callback = NULL;
        transaction.ctx = NULL;
        _anedya_txn_store_release_slot(&s_client.txn_store, &transaction);
    }
    return false;
}

// Background publisher task. It blocks on the transmit queue, waits until MQTT is
// actually connected, then publishes each status update (retrying a few times).
// Running this on its own task keeps the MQTT and esp_peer callbacks from blocking
// on network I/O.
static void signaling_transmit_task(void *argument)
{
    signaling_tx_item_t item;

    for (;;) {
        if (xQueueReceive(s_transmit_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        xEventGroupWaitBits(
            s_connection_events, MQTT_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

        bool published = false;
        for (int attempt = 1; attempt <= SIGNALING_TX_RETRIES; attempt++) {
            ESP_LOGD(TAG, "Status transmit status=%s length=%d attempt=%d",
                     item.status, (int)item.ack_data_length, attempt);
            published = command_status_blocking(item.command_id, item.status, item.ack_data,
                                                item.ack_data_length);
            if (published) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(SIGNALING_TX_RETRY_DELAY_MS));
        }

        if (!published) {
            ESP_LOGE(TAG, "Status transmit failed after retries (status=%s)", item.status);
        }
        free(item.ack_data);
    }
}

// Queue a command status update for the transmit task. Takes ownership of ack_data
// (may be NULL). Returns true if the update was enqueued, false if it was dropped.
static bool queue_status(const anedya_uuid_t command_id, anedya_cmd_status_t status,
                         char *ack_data, size_t ack_data_length)
{
    if (!s_transmit_queue) {
        ESP_LOGE(TAG, "Status transmit queue not ready");
        free(ack_data);
        return false;
    }

    signaling_tx_item_t item = {0};
    memcpy(item.command_id, command_id, sizeof(item.command_id));
    item.status = status;
    item.ack_data = ack_data;
    item.ack_data_length = ack_data_length;

    if (xQueueSend(s_transmit_queue, &item, pdMS_TO_TICKS(SIGNALING_TX_QUEUE_SEND_TIMEOUT_MS))
            != pdTRUE) {
        ESP_LOGE(TAG, "Status transmit queue full, dropping");
        free(ack_data);
        return false;
    }
    return true;
}

// Conclude the in-flight command as failed with a short reason, and clear it so
// no later answer/outcome update tries to touch the now-terminal command.
static void fail_active_command(const char *reason)
{
    if (!s_have_active_command) {
        return;
    }
    char *reason_copy = reason ? strdup(reason) : NULL;
    queue_status(s_active_command_id, ANEDYA_CMD_STATUS_FAILED, reason_copy,
                 reason_copy ? strlen(reason_copy) : 0);
    s_have_active_command = false;
}

// Decode the browser's offer payload (base64 -> inflate) into a heap-allocated,
// NUL-terminated JSON string. Returns the buffer (caller frees) on success, or
// NULL on failure — in which case the active command has already been concluded as
// failed. The buffers are heap-allocated rather than on the stack because this runs
// on the MQTT task, whose stack is small, and the decompressor state alone is ~11KB.
static char *decode_offer(const anedya_command_obj_t *command)
{
    if (command->cmd_data_type != ANEDYA_DATATYPE_STRING) {
        ESP_LOGE(TAG, "webrtc_offer command has unexpected datatype %u", command->cmd_data_type);
        fail_active_command("bad datatype");
        return NULL;
    }

    // Guard against a payload larger than our fixed decode buffer. The Anedya
    // base64 decoder writes strlen(input)/4*3 bytes with no output-size check, so
    // we must verify it fits *before* decoding to avoid a heap overflow.
    size_t encoded_length = strlen((const char *)command->data);
    size_t decoded_length_estimate = encoded_length / 4 * 3;
    if (decoded_length_estimate > OFFER_DEFLATE_MAX_BYTES) {
        ESP_LOGE(TAG, "Offer too large: %u encoded bytes exceed %d-byte decode buffer",
                 (unsigned)encoded_length, OFFER_DEFLATE_MAX_BYTES);
        fail_active_command("offer too large");
        return NULL;
    }

    // Step 1: base64-decode into the raw deflate bytes.
    char *deflate_buffer = malloc(OFFER_DEFLATE_MAX_BYTES);
    if (!deflate_buffer) {
        ESP_LOGE(TAG, "Out of memory allocating deflate buffer");
        fail_active_command("out of memory");
        return NULL;
    }
    unsigned int deflate_length = _anedya_base64_decode((unsigned char *)command->data,
                                                        deflate_buffer);
    if (deflate_length == 0) {
        ESP_LOGE(TAG, "base64 decode failed (data_len=%u)", command->data_len);
        free(deflate_buffer);
        fail_active_command("base64 decode failed");
        return NULL;
    }
    ESP_LOGD(TAG, "Offer base64 decoded: %u bytes", deflate_length);

    // Step 2: inflate (decompress) the deflate bytes back into the offer JSON.
    // Note: the convenient tinfl_decompress_mem_to_mem() puts the ~11KB
    // tinfl_decompressor struct on the stack, which overflows the MQTT task's
    // stack. So we use the low-level API and heap-allocate that struct ourselves.
    tinfl_decompressor *decompressor = malloc(sizeof(tinfl_decompressor));
    char *offer_buffer = malloc(OFFER_JSON_MAX_BYTES);
    if (!decompressor || !offer_buffer) {
        ESP_LOGE(TAG, "Out of memory allocating decompressor/offer buffer");
        free(decompressor);
        free(offer_buffer);
        free(deflate_buffer);
        fail_active_command("out of memory");
        return NULL;
    }

    tinfl_init(decompressor);
    size_t input_bytes = deflate_length;
    size_t output_bytes = OFFER_JSON_MAX_BYTES - 1;  // leave room for the NUL terminator
    tinfl_status status = tinfl_decompress(
        decompressor,
        (const mz_uint8 *)deflate_buffer, &input_bytes,
        (mz_uint8 *)offer_buffer, (mz_uint8 *)offer_buffer, &output_bytes,
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(decompressor);
    free(deflate_buffer);

    if (status < 0) {
        ESP_LOGE(TAG, "tinfl decompression failed: status=%d", (int)status);
        free(offer_buffer);
        fail_active_command("decompress failed");
        return NULL;
    }
    offer_buffer[output_bytes] = '\0';
    ESP_LOGD(TAG, "Offer inflated to JSON: %d bytes", (int)output_bytes);
    return offer_buffer;
}

// Parse the offer JSON and hand its SDP + optional TURN credentials to the WebRTC
// layer, which starts the connection and produces the answer asynchronously. The
// JSON looks like {"sdp": "...", "turn": {"username": "...", "credential": "..."}}.
// Concludes the command as failed if the required 'sdp' field is missing.
static void parse_and_dispatch_offer(char *offer_json)
{
    json_t json_pool[OFFER_JSON_MAX_TOKENS];
    json_t const *json = json_create(offer_json, json_pool,
                                     sizeof(json_pool) / sizeof(*json_pool));
    const char *sdp = json ? json_getPropertyValue(json, "sdp") : NULL;
    if (!sdp) {
        ESP_LOGE(TAG, "webrtc_offer command JSON missing 'sdp' field");
        fail_active_command("missing sdp");
        return;
    }

    // TURN credentials are optional; if absent the WebRTC layer falls back to STUN.
    json_t const *turn = json_getProperty(json, "turn");
    bool turn_is_object = turn && json_getType(turn) == JSON_OBJ;
    const char *turn_username = turn_is_object ? json_getPropertyValue(turn, "username") : NULL;
    const char *turn_credential = turn_is_object ? json_getPropertyValue(turn, "credential") : NULL;
    webrtc_peer_set_turn_credentials(turn_username, turn_credential);

    // The WebRTC layer will build and return the answer via
    // anedya_signaling_write_answer() once ICE gathering completes.
    webrtc_peer_on_offer(sdp, strlen(sdp));
}

// Handle one incoming Anedya command. This is the top of the receive path: it
// filters for the "webrtc_offer" command, acknowledges it, then delegates decoding
// and dispatch to the helpers above. Each failure path concludes the command as
// "failed" so the browser stops waiting.
static void handle_command(const anedya_command_obj_t *command)
{
    // Only the "webrtc_offer" command concerns us. Its command id is the key we
    // use to correlate the answer back to this offer (see s_active_command_id). The
    // payload is base64(deflate(offer JSON)) sent as a string datatype.
    if (strcmp(command->command, "webrtc_offer") != 0) {
        ESP_LOGD(TAG, "Ignoring command: %s", command->command);
        return;
    }
    ESP_LOGI(TAG, "WebRTC offer received (%u bytes)", command->data_len);

    // Record the command id up front so any failure below can conclude *this*
    // command as failed (and so esp_peer's later answer/outcome callbacks, which
    // run on the peer-loop task, know which command to update). We support a single
    // active connection at a time: one camera, one viewer.
    memcpy(s_active_command_id, command->cmdId, sizeof(s_active_command_id));
    s_have_active_command = true;

    // Acknowledge receipt before doing any work. Status flow:
    // received -> processing(+answer) -> success | failure.
    queue_status(s_active_command_id, ANEDYA_CMD_STATUS_RECEIVED, NULL, 0);

    char *offer_json = decode_offer(command);
    if (!offer_json) {
        return;  // decode_offer already concluded the command as failed
    }
    parse_and_dispatch_offer(offer_json);
    free(offer_json);
}

// Anedya SDK event dispatcher. We only care about incoming commands; everything
// else is logged at debug level and ignored.
static void event_handler(anedya_client_t *client, anedya_event_t event, void *event_data)
{
    if (event == ANEDYA_EVENT_COMMAND) {
        anedya_command_obj_t *command = (anedya_command_obj_t *)event_data;
        ESP_LOGD(TAG, "Raw command: name='%s' datatype=%u data_len=%u data='%.40s'",
                 command->command, command->cmd_data_type, command->data_len, command->data);
        handle_command(command);
    } else {
        ESP_LOGD(TAG, "Ignoring Anedya event type: %u", (unsigned)event);
    }
}

// Connection state callbacks. They flip the MQTT_CONNECTED_BIT so the transmit
// task knows when it is safe to publish.
static void on_connect(anedya_context_t context)
{
    ESP_LOGI(TAG, "MQTT connected");
    EventGroupHandle_t *events = (EventGroupHandle_t *)context;
    xEventGroupSetBits(s_connection_events, MQTT_CONNECTED_BIT);
    xEventGroupSetBits(*events, MQTT_CONNECTED_BIT);
}

static void on_disconnect(anedya_context_t context)
{
    ESP_LOGW(TAG, "MQTT disconnected");
    EventGroupHandle_t *events = (EventGroupHandle_t *)context;
    xEventGroupClearBits(*events, MQTT_CONNECTED_BIT);
    xEventGroupClearBits(s_connection_events, MQTT_CONNECTED_BIT);
}

// Public entry point (called from app_main). Creates the sync primitives and
// transmit task, configures the Anedya client from the menuconfig device
// credentials, connects, and blocks until MQTT is up (or times out). After this
// returns the device is ready to receive offers.
void anedya_signaling_init(void)
{
    s_connection_events = xEventGroupCreate();
    s_transmit_queue = xQueueCreate(SIGNALING_TX_QUEUE_DEPTH, sizeof(signaling_tx_item_t));
    if (!s_connection_events || !s_transmit_queue) {
        ESP_LOGE(TAG, "Failed to create Anedya signaling primitives");
        return;
    }
    if (xTaskCreate(signaling_transmit_task, "anedya_signaling_tx",
                    SIGNALING_TX_TASK_STACK_BYTES, NULL,
                    SIGNALING_TX_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create signaling transmit task (out of memory?)");
        return;
    }

    // Catch the most common first-run mistake early: credentials left blank in
    // menuconfig. Without this the failure surfaces later as a confusing parse or
    // connect error.
    if (CONFIG_ANEDYA_DEVICE_ID[0] == '\0' || CONFIG_ANEDYA_CONNECTION_KEY[0] == '\0') {
        ESP_LOGE(TAG, "Anedya credentials not set. Run 'idf.py menuconfig' -> "
                      "'Anedya WebRTC Camera' and set Device ID and Connection Key.");
        return;
    }

    char device_id_string[ANEDYA_DEVICE_ID_STRING_SIZE];
    strncpy(device_id_string, CONFIG_ANEDYA_DEVICE_ID, sizeof(device_id_string) - 1);
    device_id_string[sizeof(device_id_string) - 1] = '\0';

    anedya_device_id_t device_id;
    anedya_err_t error = anedya_parse_device_id(device_id_string, device_id);
    if (error != ANEDYA_OK) {
        ESP_LOGE(TAG, "Invalid Anedya Device ID '%s' in menuconfig (err=%d); expected a UUID",
                 device_id_string, error);
        return;
    }

    anedya_config_init(
        &s_config, device_id, CONFIG_ANEDYA_CONNECTION_KEY,
        strlen(CONFIG_ANEDYA_CONNECTION_KEY));
    anedya_config_set_region(&s_config, ANEDYA_REGION_AP_IN_1);
    anedya_config_set_timeout(&s_config, ANEDYA_REQUEST_TIMEOUT_MS);
    anedya_config_set_connect_cb(&s_config, on_connect, &s_connection_events);
    anedya_config_set_disconnect_cb(&s_config, on_disconnect, &s_connection_events);
    anedya_config_register_event_handler(&s_config, event_handler, NULL);

    anedya_client_init(&s_config, &s_client);
    error = anedya_client_connect(&s_client);
    if (error != ANEDYA_OK) {
        ESP_LOGE(TAG, "anedya_client_connect failed: %d", error);
        return;
    }

    ESP_LOGI(TAG, "Waiting for MQTT connection...");
    EventBits_t bits = xEventGroupWaitBits(
        s_connection_events,
        MQTT_CONNECTED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(MQTT_CONNECT_TIMEOUT_MS));
    if ((bits & MQTT_CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "Anedya MQTT did not connect within %d ms", MQTT_CONNECT_TIMEOUT_MS);
        return;
    }

    ESP_LOGI(TAG, "Anedya signaling ready");

    // Start the heartbeat task so Anedya tracks this device as online.
    if (xTaskCreate(heartbeat_task, "anedya_hb", HEARTBEAT_TASK_STACK_BYTES, NULL,
                    HEARTBEAT_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create heartbeat task (out of memory?)");
    }
}

// Publish the local SDP answer. Called by the WebRTC layer once esp_peer has
// produced the answer. This is the mirror image of the offer decode above:
// deflate the SDP, base64-encode it, and send it as the command's ack data with
// status "processing". Declared in anedya_signaling.h.
void anedya_signaling_write_answer(const char *sdp, size_t sdp_length)
{
    if (!s_have_active_command) {
        ESP_LOGE(TAG, "Answer ready but no active command to acknowledge");
        return;
    }

    // Compress then base64-encode the answer SDP so it fits the ~1KB command
    // ack data budget, mirroring the offer path. tdefl_compressor's state is large,
    // so we heap-allocate it to keep it off this caller's stack (the esp_peer
    // peer-loop task). We base64 ourselves (mbedtls) and send as a string datatype
    // because the SDK's strlen-based base64 would NUL-truncate raw deflate bytes.
    tdefl_compressor *compressor = malloc(sizeof(tdefl_compressor));
    unsigned char *deflate_buffer = malloc(ANSWER_DEFLATE_MAX_BYTES);
    if (!compressor || !deflate_buffer) {
        ESP_LOGE(TAG, "Out of memory allocating compressor/deflate buffer");
        free(compressor);
        free(deflate_buffer);
        return;
    }

    // raw deflate (no zlib header), matching the browser's "deflate-raw" and the
    // raw tinfl inflate used for the offer. Omitting TDEFL_WRITE_ZLIB_HEADER = raw.
    tdefl_init(compressor, NULL, NULL, TDEFL_DEFAULT_MAX_PROBES);
    size_t input_bytes = sdp_length;
    size_t output_bytes = ANSWER_DEFLATE_MAX_BYTES;
    tdefl_status deflate_status = tdefl_compress(compressor, sdp, &input_bytes, deflate_buffer,
                                                 &output_bytes, TDEFL_FINISH);
    free(compressor);
    if (deflate_status != TDEFL_STATUS_DONE) {
        // TDEFL_STATUS_OKAY with TDEFL_FINISH means the output buffer filled before
        // the whole SDP was compressed — i.e. the answer is too large for our fixed
        // buffer. Call that out distinctly from a genuine compressor error so an
        // oversized-answer failure is obvious in the logs.
        if (deflate_status == TDEFL_STATUS_OKAY) {
            ESP_LOGE(TAG, "Answer too large: %d-byte SDP does not fit %d-byte deflate buffer",
                     (int)sdp_length, ANSWER_DEFLATE_MAX_BYTES);
            fail_active_command("answer too large");
        } else {
            ESP_LOGE(TAG, "deflate failed: status=%d in=%d out=%d",
                     (int)deflate_status, (int)input_bytes, (int)output_bytes);
            fail_active_command("answer deflate failed");
        }
        free(deflate_buffer);
        return;
    }
    size_t deflate_length = output_bytes;

    // base64 the deflate bytes (length-aware, binary-safe)
    size_t base64_capacity = ((deflate_length + 2) / 3) * 4 + 1;
    char *base64 = malloc(base64_capacity);
    if (!base64) {
        ESP_LOGE(TAG, "Out of memory allocating base64 buffer");
        free(deflate_buffer);
        fail_active_command("out of memory");
        return;
    }
    size_t base64_length = 0;
    int encode_result = mbedtls_base64_encode((unsigned char *)base64, base64_capacity,
                                              &base64_length, deflate_buffer, deflate_length);
    free(deflate_buffer);
    if (encode_result != 0) {
        ESP_LOGE(TAG, "base64 encode failed: -0x%x", -encode_result);
        free(base64);
        fail_active_command("answer base64 failed");
        return;
    }
    base64[base64_length] = '\0';

    ESP_LOGI(TAG, "WebRTC answer ready, sending to browser");
    ESP_LOGD(TAG, "Answer sizes: raw=%dB -> deflate=%dB -> base64=%dB",
             (int)sdp_length, (int)deflate_length, (int)base64_length);

    // Send the answer as "processing", not "success": success/failure are terminal
    // in Anedya, so we only conclude once the WebRTC connection actually succeeds
    // or fails (see anedya_signaling_command_conclude). The browser reads the answer
    // from the ack data while status is "processing". queue_status takes ownership of
    // base64. We keep s_have_active_command set so the later conclusion can reference
    // this command.
    queue_status(s_active_command_id, ANEDYA_CMD_STATUS_PROCESSING, base64, base64_length);
}

// Conclude the in-flight command with a terminal status. Called by the WebRTC
// layer when the connection reaches a final outcome: data channel open -> success,
// connect failed -> failure. Declared in anedya_signaling.h.
void anedya_signaling_command_conclude(bool success, const char *reason)
{
    if (!s_have_active_command) {
        ESP_LOGW(TAG, "Conclude requested but no active command");
        return;
    }
    if (success) {
        queue_status(s_active_command_id, ANEDYA_CMD_STATUS_SUCCESS, NULL, 0);
    } else {
        char *reason_copy = reason ? strdup(reason) : NULL;
        queue_status(s_active_command_id, ANEDYA_CMD_STATUS_FAILED, reason_copy,
                     reason_copy ? strlen(reason_copy) : 0);
    }
    s_have_active_command = false;
}
