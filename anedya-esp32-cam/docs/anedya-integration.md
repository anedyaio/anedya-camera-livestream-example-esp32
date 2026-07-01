# Anedya SDK — ESP-IDF Integration Research

How to add Anedya to an ESP-IDF project and use it for signaling in this WebRTC camera project.

---

## What Anedya Provides

- **MQTT broker** (managed, TLS, pre-configured certs bundled in SDK)
- **ValueStore** — cloud key-value store (string/float/bool/binary, up to 512KB per key)
- **Commands** — send instructions from cloud/browser to device
- **Heartbeat** — keep-alive mechanism
- **OTA** — firmware update over the air
- **WebRTC Relay (Beta)** — managed STUN + TURN servers at $0.05/GB

For this project we primarily need: **MQTT connection + ValueStore** for SDP signaling.

---

## Installation

### Add to `idf_component.yml`

```yaml
dependencies:
  anedya/anedya-esp: "^0.0.15"
  idf: ">=4.1.0"
```

Or via CLI:

```sh
idf.py add-dependency "anedya/anedya-esp^0.0.15"
```

The SDK is on the ESP Component Registry — downloads automatically on next build.  
No manual cloning needed.

---

## Credentials Required

From the Anedya console you need:

| Config Key | What it is |
|---|---|
| `CONFIG_PHYSICAL_DEVICE_ID` | Device UUID (assigned in Anedya console) |
| `CONFIG_CONNECTION_KEY` | Device connection key (secret) |
| `CONFIG_ANEDYA_REGION` | Region string e.g. `ap-in-1` |

Set via `idf.py menuconfig` or hardcode for dev.

---

## SDK Core API

### Init & Connect

```c
#include "anedya.h"

anedya_config_t anedya_client_config;
anedya_client_t anedya_client;

// 1. Parse device ID
anedya_device_id_t devid;
anedya_parse_device_id(CONFIG_PHYSICAL_DEVICE_ID, devid);

// 2. Init config
anedya_config_init(&anedya_client_config, devid, connkey, strlen(connkey));

// 3. Set callbacks
anedya_config_set_connect_cb(&anedya_client_config, on_connect, &event_group);
anedya_config_set_disconnect_cb(&anedya_client_config, on_disconnect, NULL);
anedya_config_register_event_handler(&anedya_client_config, event_handler, NULL);
anedya_config_set_region(&anedya_client_config, ANEDYA_REGION_AP_IN_1);
anedya_config_set_timeout(&anedya_client_config, 30000);

// 4. Connect (blocks until MQTT handshake done)
anedya_client_init(&anedya_client_config, &anedya_client);
anedya_err_t err = anedya_client_connect(&anedya_client);
```

### Event Handler (receives ValueStore updates + Commands)

```c
void event_handler(anedya_client_t *client, anedya_event_t event, void *event_data)
{
    switch (event) {
    case ANEDYA_EVENT_VS_UPDATE_STRING:
        anedya_valuestore_obj_string_t *vs = (anedya_valuestore_obj_string_t *)event_data;
        // vs->key   = "offer_abc123"
        // vs->value = "{...SDP JSON...}"
        break;

    case ANEDYA_EVENT_VS_UPDATE_FLOAT:
        anedya_valuestore_obj_float_t *vs_f = (anedya_valuestore_obj_float_t *)event_data;
        break;

    case ANEDYA_EVENT_VS_UPDATE_BOOL:   /* ... */ break;
    case ANEDYA_EVENT_VS_UPDATE_BIN:    /* ... */ break;
    case ANEDYA_EVENT_COMMAND:
        anedya_command_obj_t *cmd = (anedya_command_obj_t *)event_data;
        // cmd->cmdId = command identifier string
        break;
    }
}
```

### Send Heartbeat

```c
anedya_txn_t txn;
anedya_txn_register_callback(&txn, TXN_COMPLETE, &task_handle);
anedya_device_send_heartbeat(&anedya_client, &txn);
xTaskNotifyWait(0, ULONG_MAX, &notified, 30000 / portTICK_PERIOD_MS);
```

---

## ValueStore — Key for SDP Signaling

ValueStore is a cloud key-value store. Keys are strings, values can be string/float/bool/binary.  
Device can **write** to its own namespace. Device **receives push notifications** when a key is updated.

### Set a value (device → cloud)

```c
#include "anedya_op_valuestore.h"

anedya_txn_t txn;
anedya_txn_register_callback(&txn, TXN_COMPLETE, &task_handle);

anedya_req_set_valuestore_string_t req = {
    .key      = "answer_abc123",
    .value    = sdp_answer_json,
    .value_len = strlen(sdp_answer_json),
};
anedya_valuestore_set_string(&anedya_client, &txn, &req);
xTaskNotifyWait(0, ULONG_MAX, &notified, 30000 / portTICK_PERIOD_MS);
```

### Receive updates (cloud → device via push)

Handled automatically in `event_handler` via `ANEDYA_EVENT_VS_UPDATE_STRING`.  
SDK subscribes to valuestore update topics automatically on connect.

---

## Proposed Signaling Flow for WebRTC

```
Browser                     Anedya Cloud            ESP32-CAM
   │                              │                      │
   │── POST offer SDP ──────────→ │                      │
   │   key: "offer_<sessionId>"   │                      │
   │                              │── push VS update ──→ │
   │                              │   ANEDYA_EVENT_VS_   │
   │                              │   UPDATE_STRING      │
   │                              │                      │── parse SDP offer
   │                              │                      │── create WebRTC answer
   │                              │                      │── SET "answer_<sessionId>"
   │                              │ ←── VS set ──────────│
   │← poll GET answer_<sessionId> │                      │
   │                              │                      │
   │  WebRTC P2P connection established                   │
   │←══════════════ ICE/DTLS/SRTP video ════════════════→│
```

---

## FreeRTOS Task Structure (recommended)

```
wifi_task          — connect to WiFi, signal event group
syncTime_task      — NTP sync before Anedya connect
anedya_task        — init + connect + heartbeat loop
signaling_task     — watch for VS update events, handle SDP exchange
camera_task        — grab frames, feed into WebRTC
```

---

## Minimal `idf_component.yml` for This Project

```yaml
dependencies:
  idf: ">=5.0"
  anedya/anedya-esp: "^0.0.15"
  espressif/esp32-camera: ">=2.1.6"
  espressif/esp_peer: ">=1.4.1"
  protocol_examples_common:
    path: ${IDF_PATH}/examples/common_components/protocol_examples_common
```

---

## Notes

- SDK bundles TLS certs for Anedya broker — no manual cert embedding needed
- MQTT broker address is derived from region (e.g. `ap-in-1`) automatically
- ValueStore push arrives on ESP32 only while MQTT connected — heartbeat keeps connection alive
- ValueStore string values can hold up to 512KB — plenty for SDP JSON (~2–5KB)
- WebRTC Relay (STUN/TURN) is separate from signaling — need TURN credentials from Anedya console for relay
- TURN currently UDP only (TCP coming soon per docs)
