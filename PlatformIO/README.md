[<img src="https://img.shields.io/badge/Anedya-Documentation-blue?style=for-the-badge">](https://docs.anedya.io?utm_source=github&utm_medium=link&utm_campaign=github-examples&utm_content=esp-cam)
[<img src="https://img.shields.io/badge/Peer-Live-blue?style=for-the-badge">](https://anedyaio.github.io/anedya-camera-livestream-example-esp32/)

<p align="center">
    <img src="https://cdn.anedya.io/anedya_black_banner.png" alt="Logo">
</p>


# ESP32- WebRTC Camera Livestream with Anedya (PlatformIO)

![Camera View](./media/DFrobot_camera_view.png)
Turn an ESP32-Camera board into a real-time camera livestream device with Anedya (Commands and TURN relay).

## ✨ Features

- **Live JPEG streaming** — camera frames sent over a WebRTC DataChannel
- **Anedya Commands signaling** — SDP offer/answer over MQTT, no signaling server
- **Anedya TURN relay** — works through firewalls, credentials come with the offer
- **Configurable profile** — frame size, JPEG quality, FPS, buffer count in one header
- **DataChannel test mode** — prove signaling works without a camera attached
- **Zero-copy frame path** — the camera frame buffer is handed straight to the
  send pipeline and released only after transmission

---

## ⚠️ Read this first: why PlatformIO and not the Arduino IDE

**This is Arduino code, but it does not build in the Arduino IDE.** That is not a
style choice — it is a hard limitation:

| Requirement | Arduino IDE | This project |
|---|---|---|
| Pull `espressif/esp_peer` (WebRTC) | ✗ no ESP-IDF Component Manager | ✓ `src/idf_component.yml` |
| `CONFIG_MBEDTLS_SSL_DTLS_SRTP=y` | ✗ core libs are precompiled, and this option is **off** | ✓ `sdkconfig.defaults` |
| `CONFIG_MBEDTLS_X509_CREATE_C=y` (esp_peer self-signs its DTLS cert) | ✗ | ✓ |
| Octal PSRAM + custom partition table | ~ partly | ✓ |

The Arduino core ships as prebuilt `.a` libraries with a fixed `sdkconfig`. It
enables `MBEDTLS_SSL_PROTO_DTLS` but **not** `MBEDTLS_SSL_DTLS_SRTP`, so
esp_peer cannot link there at all.

PlatformIO's hybrid mode — `framework = arduino, espidf` — builds ESP-IDF from
source **with Arduino as a component**. You write ordinary Arduino code and
still get managed components and `sdkconfig` control. Sketch files are `.cpp`
rather than `.ino`; that is the only difference in how you write code.

> If you ever want a genuine `.ino` in the Arduino IDE, the only route is to
> rebuild the core yourself with
> [esp32-arduino-lib-builder](https://github.com/espressif/esp32-arduino-lib-builder)
> and `CONFIG_MBEDTLS_SSL_DTLS_SRTP=y` added to `configs/defconfig.common`.

---

## 🏗 How It Works

### Signaling via Anedya Commands + MQTT

WebRTC peers must exchange SDP offers and answers before media can flow. This
example uses Anedya Commands as the signaling channel and Anedya MQTT as the
delivery mechanism.

```
Browser Viewer
  │  1. Fetch TURN credentials (Anedya REST API)
  │  2. POST /commands/send  command="webrtc_offer"
  │     data = base64(deflate-raw({sdp, turn}))
  ▼
Anedya Cloud  (Commands + MQTT broker + TURN relay)
  │  3. Push to $anedya/device/<id>/commands
  ▼
XIAO ESP32S3
  │  4. Decode offer, extract SDP + TURN credentials
  │  5. Publish status "processing" with ackdata = base64(deflate-raw(answer SDP))
  ▼
Browser Viewer
  │  6. Poll /commands/getDetails → read ackdata → setRemoteDescription
  │  7. ICE negotiation completes
  │  8. JPEG frames flow over the DataChannel → rendered in <img>
```

Command status flow: `received` → `processing` (carries the answer) →
`success` | `failure`. `success`/`failure` are terminal in Anedya, so the
firmware only sends them once WebRTC has actually connected or failed.

### JPEG over DataChannel

This project does not use WebRTC RTP video tracks. Camera JPEG frames are sent
as binary messages on a DataChannel labeled `jpeg-test`; the browser updates an
`<img>` per frame. Simple to inspect from both C++ and JavaScript.

---

## 📁 Repository Layout

```
.
├── platformio.ini            — pioarduino platform, framework = arduino, espidf
├── sdkconfig.defaults        — DTLS-SRTP, octal PSRAM, 1 kHz tick, partitions
├── partitions.csv            — 8 MB flash, single factory app
├── include/
│   ├── app_config.h          — ★ WiFi + Anedya credentials + all tuning
│   ├── camera_pins.h         — camera pin map (XIAO ESP32S3 Sense / AI Thinker)
│   ├── anedya_signaling.h
│   └── webrtc_peer.h
├── src/
│   ├── idf_component.yml     — esp_peer, esp32-camera (ESP-IDF managed components)
│   ├── CMakeLists.txt
│   ├── main.cpp              — setup()/loop(): camera capture + MQTT pump
│   ├── anedya_signaling.cpp  — PubSubClient MQTT, Commands-based signaling
│   └── webrtc_peer.cpp       — esp_peer peer connection, DataChannel send pipeline
└── test-peer/
    └── index.html            — browser viewer (open directly, no server needed)
```

---

## 🚀 Getting Started

### What You Need

**Hardware** (either board)
- Seeed Studio XIAO ESP32S3 Sense (built-in OV2640 camera, native USB — no programmer needed), or
- AI Thinker ESP32-CAM (OV2640 or OV3660 camera module) + USB-to-serial programmer (e.g. FTDI, CP2102)
- DFrobot ESP32 S3 AI Thinker Camera (built-in OV3660 camera, native USB — no programmer needed)

**Software / Accounts**
- [PlatformIO](https://platformio.org/install) (VS Code extension or `pip install platformio`)
- An [Anedya](https://anedya.io?utm_source=github&utm_medium=link&utm_campaign=github-examples&utm_content=esp-cam) account

---

### Step 1: Create Your Anedya Project

1. Sign in at the [Anedya Console](https://accounts.anedya.io/ui/login).
2. Create a project.
3. Create a node for your camera and pre-authorize it with a UUID.
4. Note these values:

| Value | Where to find it | Used by |
|---|---|---|
| Device ID | Node details → Device ID | firmware (`app_config.h`) |
| Connection Key | Node details → Connection Key | firmware (`app_config.h`) |
| Node ID | Node details → Node ID | browser viewer |
| Platform API key | Project → API keys | browser viewer |

> [!TIP]
> See [Anedya Project Setup](https://docs.anedya.io/getting-started/project-setup/)
> for a walkthrough of the console.

---

### Step 2: Configure the Firmware

Everything lives in [include/app_config.h](include/app_config.h):

```c
#define WIFI_SSID     "your-ssid"
#define WIFI_PASSWORD "your-password"

#define ANEDYA_DEVICE_ID      "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
#define ANEDYA_CONNECTION_KEY "your-connection-key"
#define ANEDYA_REGION_CODE    "ap-in-1"
```

The Node ID is **not** needed in firmware — it goes in the browser viewer.

---

### Step 4: Build & Flash
 
The build environment selects the board. Open your terminal in the project directory and run:
 
**Seeed XIAO ESP32S3 Sense:**
```bash
pio run -e seeed_xiao_esp32s3 -t upload -t monitor
```
 
**DFRobot ESP32-S3 AI Camera:**
```bash
pio run -e dfrobot_ai_camera -t upload -t monitor
```
 
**AI Thinker ESP32-CAM:**
```bash
pio run -e esp32cam -t upload -t monitor
```
 
> [!WARNING]
> **For ESP32-CAM Users:** Flashing requires connecting `IO0` to `GND`. After PlatformIO finishes uploading, you **must remove the IO0 jumper** and press the RESET button.
>
> **Using an `esp32cam-mb` shield?** Our `platformio.ini` is already configured to release the DTR/RTS lines so your board does not get stuck in reset!
 
---

### Step 4: Connect a Viewer

Open [peer/index.html](https://anedyaio.github.io/anedya-camera-livestream-example-esp32/) in a browser, then:

1. Click **Settings**
2. Enter your **Node ID** and **Platform API key**
3. Click **Start Stream**

The viewer fetches TURN credentials from Anedya, sends the offer as a command,
and waits for the answer. Once the DataChannel opens, frames appear.

---

## 🎛 Camera Stream Settings

Defaults target balanced quality at 20 FPS ([app_config.h](include/app_config.h)):

| Setting | Value |
|---|---|
| Frame size | HVGA (480 × 320) |
| JPEG quality | 25 |
| Frame buffer count | 2 |
| Target FPS | 20 |

Max-FPS / lower-quality preset:

```c
#define CAMERA_STREAM_FRAME_SIZE   FRAMESIZE_QVGA
#define CAMERA_STREAM_FRAME_NAME   "QVGA"
#define CAMERA_STREAM_JPEG_QUALITY 20
#define CAMERA_STREAM_FB_COUNT     3
```

Delivered FPS in the browser is bounded by DataChannel bandwidth
(≈ 600 kbps ÷ frame bytes), not by this setting alone.

---

## 🔌 DataChannel Test Mode

Uncomment `#define DATACHANNEL_TEST_MODE` in `app_config.h` to skip camera init and
send `ping N from esp32` over the DataChannel instead. Use it to separate
"signaling/WebRTC is broken" from "the camera is broken".

---

## 🧵 Threading Model

| Task | Core | Priority | Does |
|---|---|---|---|
| `loopTask` (Arduino) | 1 | 1 | `loop()`: camera capture + MQTT/TLS |
| `peer_loop` | 1 | 18 | every `esp_peer_*` call, 10 ms tick |
| WiFi / lwIP | 0 | high | radio + TCP/IP |

esp_peer is **cooperative and not thread-safe**, so every `esp_peer_*` call has
to happen on one task, and that task must tick tightly at high priority or the
ICE/DTLS handshake gets preempted mid-flight. That is why the WebRTC layer is
its own pinned task rather than part of `loop()`.

Consequences, both handled in the code:
- `anedyaSignalingWriteAnswer()` and `anedyaSignalingConclude()` are called from
  the peer task, so they **queue** the MQTT publish for `loop()` instead of
  publishing inline (PubSubClient is not thread-safe, and a blocking TLS write
  from a priority-18 task would stall the handshake).
- The active command id is shared between the two tasks behind a mutex.

---


<!-- ## 🩺 Troubleshooting

| Symptom | Cause / fix |
|---|---|
| No serial output at all | The XIAO has no UART bridge. `sdkconfig.defaults` sets `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` so IDF logs and `Serial` share the USB port. If the port vanishes after flashing, double-tap the reset button to enter bootloader. |
| `Camera init failed: 0x105` | PSRAM not up, or the Sense expansion board is not seated. Check the boot log says `PSRAM: yes (8192 KB)`. |
| `Anedya broker connect failed, rc=-2` | TLS/DNS. Check WiFi, and check the NTP sync line — an unset clock makes the broker certificate look expired. |
| Offer arrives but nothing happens | Raise the log level: `esp_log_level_set("anedya_signaling", ESP_LOG_DEBUG);` in `setup()`. |
| `Offer too large` | The compressed offer exceeded `OFFER_DEFLATE_MAX_BYTES`. The viewer already refuses offers over ~950 base64 bytes. |
| Stream connects then dies after ~90 s | The relay-path tuning in `app_config.h` (`WEBRTC_AGENT_RECV_TIMEOUT_MS`, `WEBRTC_SEND_CACHE_SIZE`) was lowered. See the comments there — the defaults exist for the TURN relay's 200–1700 ms RTT. |
| `JPEG send would block` warnings | Normal back pressure when the link cannot keep up. Lower `CAMERA_STREAM_FPS` or raise `CAMERA_STREAM_JPEG_QUALITY`. | -->

---

## 📚 References

**Anedya**
- [Anedya Overview](https://docs.anedya.io/anedya-overview/)
- [Anedya MQTT Endpoints](https://docs.anedya.io/device/mqtt-endpoints/)
- [Anedya Commands](https://docs.anedya.io/features/commands/commands-intro/)
- [Update status of a command](https://docs.anedya.io/device/api/commands-update-status/)
- [Anedya ESP32 Arduino examples](https://github.com/anedyaio/anedya-example-esp32)

**Arduino / ESP32 / WebRTC**
- [pioarduino platform-espressif32](https://github.com/pioarduino/platform-espressif32)
- [Arduino as an ESP-IDF component](https://docs.espressif.com/projects/arduino-esp32/en/latest/esp-idf_component.html)
- [espressif/esp_peer](https://components.espressif.com/components/espressif/esp_peer)
- [espressif/esp32-camera](https://components.espressif.com/components/espressif/esp32-camera)
- [WebRTC Overview](https://webrtc.org/getting-started/overview)

---
## 📑 Other examples
- [ESP-IDF version of this example](./../ESP-IDF/)
- [Anedya Camera Livestream with Raspberry Pi](https://github.com/anedyaio/anedya-camera-livestream-example)
