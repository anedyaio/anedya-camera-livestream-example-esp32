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

#include "anedya_sig.h"
#include "webrtc_peer.h"

static const char *TAG = "anedya_sig";

/*
 * Signaling rides entirely on Anedya Commands. The WebRTC offer *is* a command;
 * its command-id is the only correlation key (no session id, no ValueStore):
 *
 *   Browser -> ESP:  POST /commands/send  name="webrtc_offer"
 *                    data = base64(deflate(offer JSON))   [string datatype]
 *
 *   ESP -> Browser:  anedya_op_cmd_status_update(cmdId, status=success,
 *                    ackdata = base64(deflate(answer SDP)))   [string ackdata]
 *                    Browser polls POST /commands/status {"id": cmdId}.
 *
 * Commands arrive on the Anedya MQTT task. We keep that callback short and push
 * the answer to a small background queue so the esp_peer callback stays fast.
 */

#define MQTT_CONNECTED_BIT BIT0
#define SIG_TX_QUEUE_DEPTH 4
#define SIG_TX_RETRIES 3

// Background queue item: a command status update to publish.
//   status   = ANEDYA_CMD_STATUS_* ("processing" for the answer, "success"/
//              "failure" to conclude).
//   ackdata  = optional null-terminated string payload (the answer SDP for
//              "processing", a short reason for "failure", NULL otherwise).
//              Heap-owned; freed by the TX task.
typedef struct {
    anedya_uuid_t cmd_id;
    anedya_cmd_status_t status;
    char *ackdata;
    size_t ackdata_len;
} sig_tx_item_t;

// The command-id of the in-flight offer. esp_peer's answer callback fires on the
// peer-loop task; it reads this to know which command to acknowledge. Single
// active connection at a time (one camera, one viewer).
static anedya_uuid_t s_active_cmd_id;
static bool s_have_active_cmd;

static anedya_config_t s_config;
static anedya_client_t s_client;
static EventGroupHandle_t s_conn_events;
static QueueHandle_t s_tx_queue;

static void txn_notify_task(anedya_txn_t *txn, anedya_context_t ctx)
{
    TaskHandle_t *task = (TaskHandle_t *)ctx;
    xTaskNotify(*task, 0x01, eSetValueWithOverwrite);
}

// Publish a status update for command cmd_id. Optional ackdata (may be NULL) is
// sent as a string datatype so the SDK's strlen-based base64 path is bypassed
// (it would NUL-truncate raw binary).
static bool cmd_status_blocking(const anedya_uuid_t cmd_id, anedya_cmd_status_t status,
                                char *ackdata, size_t ackdata_len)
{
    TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    anedya_txn_t txn = {0};
    uint32_t notif = 0;

    anedya_req_cmd_status_update_t req = {
        .status = status,
        .data = (unsigned char *)ackdata,
        .data_len = ackdata ? ackdata_len : 0,
        .data_type = ANEDYA_DATATYPE_STRING,
    };
    memcpy(req.cmdId, cmd_id, sizeof(req.cmdId));

    anedya_txn_register_callback(&txn, txn_notify_task, &caller);
    anedya_err_t err = anedya_op_cmd_status_update(&s_client, &txn, &req);
    if (err != ANEDYA_OK) {
        ESP_LOGE(TAG, "cmd status update submit failed: %d", err);
        return false;
    }

    xTaskNotifyWait(0x00, ULONG_MAX, &notif, pdMS_TO_TICKS(10000));
    if (notif == 0x01 && txn.is_success) {
        ESP_LOGI(TAG, "Command status=%s set (%d bytes ackdata)",
                 status, (int)(ackdata ? ackdata_len : 0));
        return true;
    }

    ESP_LOGE(TAG, "cmd status update failed/timeout notif=0x%lx complete=%d success=%d op_err=%d",
             (unsigned long)notif, txn.is_complete, txn.is_success, txn._op_err);

    /*
     * If the SDK transaction never completed, release its slot before retrying.
     * This mirrors the SDK's normal cleanup path and prevents transaction leaks.
     */
    if (!txn.is_complete && txn.desc > 0) {
        txn.callback = NULL;
        txn.ctx = NULL;
        _anedya_txn_store_release_slot(&s_client.txn_store, &txn);
    }
    return false;
}

static void sig_tx_task(void *arg)
{
    sig_tx_item_t item;

    for (;;) {
        if (xQueueReceive(s_tx_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        xEventGroupWaitBits(
            s_conn_events, MQTT_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

        bool ok = false;
        for (int attempt = 1; attempt <= SIG_TX_RETRIES; attempt++) {
            ESP_LOGI(TAG, "Status TX status=%s len=%d attempt=%d",
                     item.status, (int)item.ackdata_len, attempt);
            ok = cmd_status_blocking(item.cmd_id, item.status, item.ackdata,
                                     item.ackdata_len);
            if (ok) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        if (!ok) {
            ESP_LOGE(TAG, "Status TX failed after retries (status=%s)", item.status);
        }
        free(item.ackdata);
    }
}

// Queue a command status update for the TX task. Takes ownership of ackdata
// (may be NULL).
static void queue_status(const anedya_uuid_t cmd_id, anedya_cmd_status_t status,
                         char *ackdata, size_t ackdata_len)
{
    if (!s_tx_queue) {
        ESP_LOGE(TAG, "Status TX queue not ready");
        free(ackdata);
        return;
    }

    sig_tx_item_t item = {0};
    memcpy(item.cmd_id, cmd_id, sizeof(item.cmd_id));
    item.status = status;
    item.ackdata = ackdata;
    item.ackdata_len = ackdata_len;

    if (xQueueSend(s_tx_queue, &item, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Status TX queue full, dropping");
        free(ackdata);
    }
}

// Conclude the in-flight command as failed with a short reason, and clear it so
// no later answer/outcome update tries to touch the now-terminal command.
static void fail_active_command(const char *reason)
{
    if (!s_have_active_cmd) {
        return;
    }
    char *r = reason ? strdup(reason) : NULL;
    queue_status(s_active_cmd_id, ANEDYA_CMD_STATUS_FAILED, r, r ? strlen(r) : 0);
    s_have_active_cmd = false;
}

static void handle_command(const anedya_command_obj_t *cmd)
{
    /*
     * The offer command is named exactly "webrtc_offer". Its command-id is the
     * correlation key for the answer (see s_active_cmd_id). Payload is
     * base64(deflate(offer JSON)) sent as a string datatype.
     */
    if (strcmp(cmd->command, "webrtc_offer") != 0) {
        ESP_LOGD(TAG, "Ignoring command: %s", cmd->command);
        return;
    }
    ESP_LOGI(TAG, "Command offer: datatype=%u data_len=%u", cmd->cmd_data_type, cmd->data_len);

    /*
     * Record the command id up front so any failure below can conclude *this*
     * command as failed (and so esp_peer's later answer/outcome callbacks, on the
     * peer-loop task, know which command to update). Single active connection.
     */
    memcpy(s_active_cmd_id, cmd->cmdId, sizeof(s_active_cmd_id));
    s_have_active_cmd = true;

    // Acknowledge receipt before doing any work. Status flow:
    // received -> processing(+answer) -> success | failure.
    queue_status(s_active_cmd_id, ANEDYA_CMD_STATUS_RECEIVED, NULL, 0);

    if (cmd->cmd_data_type != ANEDYA_DATATYPE_STRING) {
        ESP_LOGE(TAG, "webrtc_offer command has unexpected datatype %u", cmd->cmd_data_type);
        fail_active_command("bad datatype");
        return;
    }

    /* base64-decode the deflate payload — heap-allocated to stay off MQTT task stack */
    char *deflate_buf = malloc(800);
    if (!deflate_buf) {
        ESP_LOGE(TAG, "OOM allocating deflate_buf");
        fail_active_command("oom");
        return;
    }
    unsigned int deflate_len = _anedya_base64_decode((unsigned char *)cmd->data, deflate_buf);
    if (deflate_len == 0) {
        ESP_LOGE(TAG, "base64 decode failed (data_len=%u)", cmd->data_len);
        free(deflate_buf);
        fail_active_command("base64 decode failed");
        return;
    }
    ESP_LOGI(TAG, "base64 decoded: %u bytes", deflate_len);

    /*
     * tinfl_decompress_mem_to_mem allocates tinfl_decompressor (~11KB) on its
     * own stack frame — fatal on the MQTT task. Use the low-level tinfl_decompress
     * API instead and heap-allocate the decompressor state ourselves.
     */
    tinfl_decompressor *decomp = malloc(sizeof(tinfl_decompressor));
    char *offer_buf = malloc(2048);
    if (!decomp || !offer_buf) {
        ESP_LOGE(TAG, "OOM allocating decompressor/offer_buf");
        free(decomp);
        free(offer_buf);
        free(deflate_buf);
        fail_active_command("oom");
        return;
    }

    tinfl_init(decomp);
    size_t in_bytes = deflate_len;
    size_t out_bytes = 2047;
    tinfl_status status = tinfl_decompress(
        decomp,
        (const mz_uint8 *)deflate_buf, &in_bytes,
        (mz_uint8 *)offer_buf, (mz_uint8 *)offer_buf, &out_bytes,
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(decomp);
    free(deflate_buf);

    size_t out_len = out_bytes;
    if (status < 0) {
        ESP_LOGE(TAG, "tinfl decompression failed: status=%d", (int)status);
        free(offer_buf);
        fail_active_command("decompress failed");
        return;
    }
    offer_buf[out_len] = '\0';
    ESP_LOGI(TAG, "Decompressed offer JSON: %d bytes", (int)out_len);

    json_t mem[12];
    json_t const *json = json_create(offer_buf, mem, sizeof(mem) / sizeof(*mem));
    const char *sdp = json ? json_getPropertyValue(json, "sdp") : NULL;
    if (!sdp) {
        ESP_LOGE(TAG, "webrtc_offer command JSON missing 'sdp' field");
        free(offer_buf);
        fail_active_command("missing sdp");
        return;
    }

    json_t const *turn = json_getProperty(json, "turn");
    bool turn_is_object = turn && json_getType(turn) == JSON_OBJ;
    const char *turn_user = turn_is_object ? json_getPropertyValue(turn, "username") : NULL;
    const char *turn_cred = turn_is_object ? json_getPropertyValue(turn, "credential") : NULL;
    webrtc_peer_set_turn_credentials(turn_user, turn_cred);

    webrtc_peer_on_offer(sdp, strlen(sdp));
    free(offer_buf);
}

static void event_handler(anedya_client_t *client, anedya_event_t event, void *event_data)
{
    if (event == ANEDYA_EVENT_COMMAND) {
        anedya_command_obj_t *cmd = (anedya_command_obj_t *)event_data;
        ESP_LOGI(TAG, "RAW command: name='%s' datatype=%u data_len=%u data='%.40s'",
                 cmd->command, cmd->cmd_data_type, cmd->data_len, cmd->data);
        handle_command(cmd);
    } else {
        ESP_LOGD(TAG, "Ignoring Anedya event type: %u", (unsigned)event);
    }
}

static void on_connect(anedya_context_t ctx)
{
    ESP_LOGI(TAG, "MQTT connected");
    EventGroupHandle_t *events = (EventGroupHandle_t *)ctx;
    xEventGroupSetBits(*events, MQTT_CONNECTED_BIT);
}

static void on_disconnect(anedya_context_t ctx)
{
    ESP_LOGW(TAG, "MQTT disconnected");
    EventGroupHandle_t *events = (EventGroupHandle_t *)ctx;
    xEventGroupClearBits(*events, MQTT_CONNECTED_BIT);
}

void anedya_sig_init(void)
{
    s_conn_events = xEventGroupCreate();
    s_tx_queue = xQueueCreate(SIG_TX_QUEUE_DEPTH, sizeof(sig_tx_item_t));
    if (!s_conn_events || !s_tx_queue) {
        ESP_LOGE(TAG, "Failed to create Anedya signaling primitives");
        return;
    }
    xTaskCreate(sig_tx_task, "anedya_sig_tx", 6144, NULL, 4, NULL);

    char dev_id_str[37];
    strncpy(dev_id_str, CONFIG_ANEDYA_DEVICE_ID, sizeof(dev_id_str) - 1);
    dev_id_str[sizeof(dev_id_str) - 1] = '\0';

    anedya_device_id_t dev_id;
    anedya_err_t err = anedya_parse_device_id(dev_id_str, dev_id);
    if (err != ANEDYA_OK) {
        ESP_LOGE(TAG, "Invalid Anedya device ID in Kconfig");
        return;
    }

    anedya_config_init(
        &s_config, dev_id, CONFIG_ANEDYA_CONNECTION_KEY,
        strlen(CONFIG_ANEDYA_CONNECTION_KEY));
    anedya_config_set_region(&s_config, ANEDYA_REGION_AP_IN_1);
    anedya_config_set_timeout(&s_config, 30000);
    anedya_config_set_connect_cb(&s_config, on_connect, &s_conn_events);
    anedya_config_set_disconnect_cb(&s_config, on_disconnect, &s_conn_events);
    anedya_config_register_event_handler(&s_config, event_handler, NULL);

    anedya_client_init(&s_config, &s_client);
    err = anedya_client_connect(&s_client);
    if (err != ANEDYA_OK) {
        ESP_LOGE(TAG, "anedya_client_connect failed: %d", err);
        return;
    }

    ESP_LOGI(TAG, "Waiting for MQTT connection...");
    EventBits_t bits = xEventGroupWaitBits(
        s_conn_events,
        MQTT_CONNECTED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(30000));
    if ((bits & MQTT_CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "Anedya MQTT did not connect within 30 seconds");
        return;
    }

    ESP_LOGI(TAG, "Anedya signaling ready");
}

void anedya_sig_write_answer(const char *sdp, size_t sdp_len)
{
    if (!s_have_active_cmd) {
        ESP_LOGE(TAG, "Answer ready but no active command to acknowledge");
        return;
    }

    /*
     * Compress then base64-encode the answer SDP so it fits the ~1KB command
     * ackdata / TX buffer, mirroring the offer path. tdefl_compress' state is
     * large (~? KB); heap-allocate it to stay off this caller's stack (the
     * esp_peer peer-loop task). We base64 ourselves (mbedtls) and send as a
     * string datatype because the SDK's strlen-based base64 would NUL-truncate
     * raw deflate bytes.
     */
    tdefl_compressor *comp = malloc(sizeof(tdefl_compressor));
    unsigned char *deflate_buf = malloc(1024);
    if (!comp || !deflate_buf) {
        ESP_LOGE(TAG, "OOM allocating compressor/deflate_buf");
        free(comp);
        free(deflate_buf);
        return;
    }

    // raw deflate (no zlib header), matching the browser's "deflate-raw" and the
    // raw tinfl inflate used for the offer. Omitting TDEFL_WRITE_ZLIB_HEADER = raw.
    tdefl_init(comp, NULL, NULL, TDEFL_DEFAULT_MAX_PROBES);
    size_t in_bytes = sdp_len;
    size_t out_bytes = 1024;
    tdefl_status st = tdefl_compress(comp, sdp, &in_bytes, deflate_buf, &out_bytes,
                                     TDEFL_FINISH);
    free(comp);
    if (st != TDEFL_STATUS_DONE) {
        ESP_LOGE(TAG, "deflate failed: st=%d in=%d out=%d", (int)st, (int)in_bytes, (int)out_bytes);
        free(deflate_buf);
        fail_active_command("answer deflate failed");
        return;
    }
    size_t deflate_len = out_bytes;

    // base64 the deflate bytes (length-aware, binary-safe)
    size_t b64_cap = ((deflate_len + 2) / 3) * 4 + 1;
    char *b64 = malloc(b64_cap);
    if (!b64) {
        ESP_LOGE(TAG, "OOM allocating b64");
        free(deflate_buf);
        fail_active_command("oom");
        return;
    }
    size_t b64_len = 0;
    int rc = mbedtls_base64_encode((unsigned char *)b64, b64_cap, &b64_len,
                                   deflate_buf, deflate_len);
    free(deflate_buf);
    if (rc != 0) {
        ESP_LOGE(TAG, "base64 encode failed: -0x%x", -rc);
        free(b64);
        fail_active_command("answer base64 failed");
        return;
    }
    b64[b64_len] = '\0';

    ESP_LOGI(TAG, "Answer: raw=%dB -> deflate=%dB -> base64=%dB",
             (int)sdp_len, (int)deflate_len, (int)b64_len);

    /*
     * Send the answer as "processing" (not "success"): success/failure are
     * terminal in Anedya, so we only conclude once the connection actually
     * succeeds or fails (anedya_sig_command_conclude). The browser reads the
     * answer ackdata while status is "processing". queue_status takes b64.
     * Keep s_have_active_cmd set so the conclusion can reference this command.
     */
    queue_status(s_active_cmd_id, ANEDYA_CMD_STATUS_PROCESSING, b64, b64_len);
}

void anedya_sig_command_conclude(bool success, const char *reason)
{
    if (!s_have_active_cmd) {
        ESP_LOGW(TAG, "Conclude requested but no active command");
        return;
    }
    if (success) {
        queue_status(s_active_cmd_id, ANEDYA_CMD_STATUS_SUCCESS, NULL, 0);
    } else {
        char *r = reason ? strdup(reason) : NULL;
        queue_status(s_active_cmd_id, ANEDYA_CMD_STATUS_FAILED, r, r ? strlen(r) : 0);
    }
    s_have_active_cmd = false;
}
