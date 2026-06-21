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
#include "anedya_operations.h"

#include "anedya_sig.h"
#include "webrtc_peer.h"

static const char *TAG = "anedya_sig";

/*
 * Anedya ValueStore is used as a tiny signaling channel:
 *
 *   Browser -> ESP:
 *     offer_<sessionId>           raw SDP offer
 *     candidate_<sessionId>_<n>   optional browser ICE candidates
 *
 *   ESP -> Browser:
 *     answer_<sessionId>          raw SDP answer
 *     candidate_esp_<sessionId>_<n>
 *
 * ValueStore updates arrive on the Anedya MQTT task. We keep that callback
 * short and push any outbound writes to a small background queue.
 */

#define MQTT_CONNECTED_BIT BIT0
#define SIG_TX_QUEUE_DEPTH 8
#define SIG_TX_RETRIES 3

typedef struct {
    char key[96];
    char *value;
    size_t value_len;
} sig_tx_item_t;

static anedya_config_t s_config;
static anedya_client_t s_client;
static EventGroupHandle_t s_conn_events;
static QueueHandle_t s_tx_queue;

static void txn_notify_task(anedya_txn_t *txn, anedya_context_t ctx)
{
    TaskHandle_t *task = (TaskHandle_t *)ctx;
    xTaskNotify(*task, 0x01, eSetValueWithOverwrite);
}

static bool vs_set_string_blocking(const char *key, const char *value, size_t value_len)
{
    TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    anedya_txn_t txn = {0};
    uint32_t notif = 0;

    anedya_txn_register_callback(&txn, txn_notify_task, &caller);
    anedya_err_t err = anedya_op_valuestore_set_string(
        &s_client, &txn, key, value, value_len);
    if (err != ANEDYA_OK) {
        ESP_LOGE(TAG, "VS set failed for key %s: %d", key, err);
        return false;
    }

    xTaskNotifyWait(0x00, ULONG_MAX, &notif, pdMS_TO_TICKS(10000));
    if (notif == 0x01 && txn.is_success) {
        ESP_LOGI(TAG, "VS set OK for key %s (%d bytes)", key, (int)value_len);
        return true;
    }

    ESP_LOGE(TAG, "VS set failed/timeout key=%s notif=0x%lx complete=%d success=%d op_err=%d",
             key, (unsigned long)notif, txn.is_complete, txn.is_success, txn._op_err);

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
            ESP_LOGI(TAG, "VS TX key=%s len=%d attempt=%d",
                     item.key, (int)item.value_len, attempt);
            ok = vs_set_string_blocking(item.key, item.value, item.value_len);
            if (ok) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        if (!ok) {
            ESP_LOGE(TAG, "VS TX failed after retries: %s", item.key);
        }
        free(item.value);
    }
}

static void queue_vs_string_write(const char *key, const char *value, size_t value_len)
{
    if (!s_tx_queue || !key || !value) {
        ESP_LOGE(TAG, "VS TX queue not ready");
        return;
    }

    sig_tx_item_t item = {0};
    strncpy(item.key, key, sizeof(item.key) - 1);

    item.value = malloc(value_len + 1);
    if (!item.value) {
        ESP_LOGE(TAG, "OOM queueing VS key %s", key);
        return;
    }
    memcpy(item.value, value, value_len);
    item.value[value_len] = '\0';
    item.value_len = value_len;

    if (xQueueSend(s_tx_queue, &item, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "VS TX queue full, dropping key %s", key);
        free(item.value);
    }
}

static void handle_valuestore_string(const anedya_valuestore_obj_string_t *update)
{
    size_t value_len = update->value ? strlen(update->value) : 0;
    ESP_LOGI(TAG, "VS string update: key=%s len=%d", update->key, (int)value_len);

    if (strncmp(update->key, "offer_", 6) == 0) {
        const char *session_id = update->key + 6;
        ESP_LOGI(TAG, "Browser offer received, session=%s", session_id);

        /*
         * Browser sends the offer wrapped as JSON:
         *   {"type":"offer","sdp":"...","turn":{"username":"...","credential":"..."}}
         * "turn" is the browser's already-fetched Anedya relay credentials,
         * piggybacked here so ESP can use the same TURN server without
         * needing a platform API key embedded in firmware. json_create()
         * parses in place, so update->value (owned by this callback) gets
         * mutated; that's fine, nothing else reads it after.
         */
        json_t mem[12];
        json_t const *json = json_create(update->value, mem, sizeof(mem) / sizeof(*mem));
        const char *sdp = json ? json_getPropertyValue(json, "sdp") : NULL;
        if (!sdp) {
            ESP_LOGE(TAG, "Offer payload is not valid JSON with an \"sdp\" field, ignoring");
            return;
        }

        json_t const *turn = json_getProperty(json, "turn");
        bool turn_is_object = turn && json_getType(turn) == JSON_OBJ;
        const char *turn_user = turn_is_object ? json_getPropertyValue(turn, "username") : NULL;
        const char *turn_cred = turn_is_object ? json_getPropertyValue(turn, "credential") : NULL;
        webrtc_peer_set_turn_credentials(turn_user, turn_cred);

        webrtc_peer_on_offer(session_id, sdp, strlen(sdp));
        return;
    }

    if (strncmp(update->key, "candidate_esp_", 14) == 0) {
        return;
    }

    if (strncmp(update->key, "candidate_", 10) == 0) {
        ESP_LOGI(TAG, "Browser ICE candidate received: key=%s", update->key);
        webrtc_peer_on_remote_candidate(update->value, value_len);
        return;
    }

    ESP_LOGD(TAG, "Ignoring ValueStore string key: %s", update->key);
}

static void event_handler(anedya_client_t *client, anedya_event_t event, void *event_data)
{
    if (event == ANEDYA_EVENT_VS_UPDATE_STRING) {
        handle_valuestore_string((anedya_valuestore_obj_string_t *)event_data);
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

void anedya_sig_write_answer(const char *session_id, const char *sdp, size_t sdp_len)
{
    char key[80];
    snprintf(key, sizeof(key), "answer_%s", session_id);
    ESP_LOGI(TAG, "Queue answer to ValueStore key=%s (%d bytes)", key, (int)sdp_len);
    queue_vs_string_write(key, sdp, sdp_len);
}

void anedya_sig_write_candidate(const char *session_id, int idx,
                                const char *candidate, size_t candidate_len)
{
    char key[96];
    snprintf(key, sizeof(key), "candidate_esp_%s_%d", session_id, idx);
    ESP_LOGD(TAG, "Queue local ICE candidate key=%s", key);
    queue_vs_string_write(key, candidate, candidate_len);
}
