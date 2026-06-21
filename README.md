[<img src="https://img.shields.io/badge/Anedya-Documentation-blue?style=for-the-badge">](https://docs.anedya.io?utm_source=github&utm_medium=link&utm_campaign=github-examples&utm_content=esp-cam)


<p align="center">
    <img src="https://cdn.anedya.io/anedya_black_banner.png" alt="Logo">
</p>

# Anedya Camera Example ESP32 

![Camera View](./media/camera_view.png)

Turn an AI Thinker ESP32-CAM into a real-time camera livestream device using Anedya for signaling and TURN relay provisioning, and WebRTC DataChannel for low-latency video delivery.

## ✨ Features

- **Live JPEG streaming :** Camera frames sent over WebRTC DataChannel.
- **Anedya ValueStore signaling :** SDP offer/answer and ICE candidates exchanged via MQTT, no custom signaling server needed
- **Configurable stream profile :** frame size, JPEG quality, FPS, and frame buffer count tunable 
- **DataChannel test mode :** verify WebRTC signaling and connectivity without a camera attached
- **Zero-copy frame path :** camera frame buffer ownership is transferred through the send pipeline and released only after transmission

---

## 🏗 How It Works

### Signaling via Anedya ValueStore + MQTT

WebRTC requires both peers to exchange SDP offers and answers before media can flow. This example uses Anedya ValueStore as a signaling channel and Anedya MQTT as the notification mechanism.

```
Browser Viewer
  │  1. Fetch TURN credentials (Anedya REST API)
  │  2. Create WebRTC offer + write offer_<sessionId> to ValueStore (JSON with SDP + TURN creds)
  ▼
Anedya Cloud  (ValueStore + MQTT broker + TURN relay)
  │  3. Notify ESP32 over MQTT subscription
  ▼
ESP32-CAM
  │  4. Parse offer, extract SDP + TURN credentials
  │  5. Create WebRTC answer + write answer_<sessionId> to ValueStore
  ▼
Browser Viewer
  │  6. Poll ValueStore → read answer → apply remote description
  │  7. ICE negotiation completes
  │  8. JPEG frames flow over WebRTC DataChannel → rendered in <img>
```

### WebRTC Connectivity

When both peers are on the same network, ICE resolves a direct path using STUN address discovery:

<p align="center">
    <img src="media/webrtc_stun_dark.png" alt="STUN direct connection diagram">
</p>

When a firewall blocks direct peer-to-peer traffic, Anedya's managed TURN relay is used automatically:

<p align="center">
    <img src="media/webrtc_turn_dark.png" alt="TURN relay connection diagram">
</p>

### JPEG over DataChannel

This project does not use WebRTC RTP video tracks. Instead, camera JPEG frames are sent as binary messages over a WebRTC DataChannel labeled `jpeg-test`. The browser receives each frame and updates an `<img>` element. This approach is intentionally simple and easy to inspect in both C and JavaScript, a useful starting point for understanding WebRTC on embedded devices.

---

## 📁 Repository Layout

```
.
├── main/
│   ├── app_main.c          — camera init, JPEG stream task, FreeRTOS entry
│   ├── anedya_sig.c        — Anedya MQTT client, ValueStore signaling read/write
│   ├── webrtc_peer.c       — esp_peer WebRTC peer, DataChannel send pipeline
│   ├── Kconfig.projbuild   — menuconfig: Device ID, Connection Key, test mode
│   └── idf_component.yml   — IDF component dependencies
├── components/
│   └── anedya__anedya-esp/ — Anedya ESP-IDF SDK
└── managed_components/     — espressif/esp32-camera, espressif/esp_peer, etc.
```

---

## 🚀 Getting Started

### What You Need

**Hardware**
- AI Thinker ESP32-CAM (OV2640 or OV3660 camera module)
- USB-to-serial programmer (e.g. FTDI, CP2102) to flash the board

**Software / Accounts**
- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/) v5.2 or later
- An [Anedya](https://anedya.io?utm_source=github&utm_medium=link&utm_campaign=github-examples&utm_content=esp-cam) account

---

### Step 1: Create Your Anedya Project

1. Sign in at [Anedya Console](https://console.anedya.io).
2. Create a new project.
3. Create a node for your ESP32-CAM and pre-authorize it with a UUID.
4. Note down these values, you will need them in Step 3:

| Value | Where to find it |
|---|---|
| `ANEDYA_DEVICE_ID` | Node details → Device ID |
| `ANEDYA_NODE_ID` | Node details → Node ID (shown in viewer QR) |
| `ANEDYA_CONNECTION_KEY` | Node details → Connection Key |

5. Generate a **Platform API key** for the browser viewer.

> [!TIP]
> See [Anedya Project Setup](https://docs.anedya.io/getting-started/project-setup/) for a step-by-step walkthrough of the console.

---

### Step 2: Clone the Repository

```bash
git clone https://github.com/anedyaio/anedya-camera-livestream-example-esp-cam.git
cd anedya-camera-livestream-example-esp-cam
```

---

### Step 3: Configure the Firmware

Run menuconfig and fill in **Anedya WebRTC Camera**:

```bash
idf.py menuconfig
```

| Config key | Where to find it |
|---|---|
| `ANEDYA_DEVICE_ID` | Anedya console → Device → Settings |
| `ANEDYA_NODE_ID` | Shown in viewer QR code |
| `ANEDYA_CONNECTION_KEY` | Anedya console → Device → Connection Key |

Also configure WiFi credentials under **Example Connection Configuration**.

> [!IMPORTANT]
> Enable PSRAM: `Component config → ESP PSRAM → Support for external, SPI-connected RAM`
> Without PSRAM, camera frame buffer allocation fails (~266 KB DRAM is not enough).
> Also set flash size to **4 MB** to match the actual chip.

---

### Step 4: Build & Flash

```bash
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

---

### Step 5: Connect a Viewer

Open the browser viewer (included in the companion viewer repository or hosted version), then:

1. Click **Settings**
2. Enter your **Node ID** and **Platform API key**
3. Click **Start Stream**

The viewer fetches TURN credentials from Anedya, writes an offer to ValueStore, and waits for the ESP32 to answer. Once the DataChannel opens, JPEG frames appear in the browser.

---

## 🎛 Camera Stream Settings

The default profile targets balanced quality at 20 FPS:

| Setting | Value |
|---|---|
| Frame size | HVGA (480 × 320) |
| JPEG quality | 23 |
| Frame buffer count | 2 |
| Grab mode | `CAMERA_GRAB_LATEST` |
| XCLK | 20 MHz |
| Target FPS | 20 |

A max-FPS / lower-quality preset is documented in comments in [main/app_main.c](main/app_main.c):

```c
#define CAMERA_STREAM_FRAME_SIZE   FRAMESIZE_QVGA
#define CAMERA_STREAM_JPEG_QUALITY 20
#define CAMERA_STREAM_FB_COUNT     3
#define CAMERA_STREAM_GRAB_MODE    CAMERA_GRAB_LATEST
```

---

## 🔌 DataChannel Test Mode

Enable `CONFIG_DATACHANNEL_TEST_MODE` in menuconfig to skip camera init entirely and instead send a counter message (`ping NNN from esp32`) over the DataChannel at a configurable interval. Use this to verify WebRTC signaling and DataChannel connectivity without a working camera.

---

## 🔧 Hardware

### Board — AI Thinker ESP32-CAM

| Property | Value |
|---|---|
| Chip | ESP32 rev 3.1 |
| Cores | 2 (Xtensa LX6) |
| CPU freq | 160 MHz |
| Flash | 4 MB external (SPI) |
| PSRAM | 4 MB external (SPI) — must enable in sdkconfig |
| WiFi | 802.11 b/g/n |
| Bluetooth | BT Classic + BLE |

### Camera — OV3660

| Property | Value |
|---|---|
| Sensor | OmniVision OV3660 |
| PID | 0x3660 |
| I2C address | 0x3c |
| Max resolution | 2048 × 1536 (3MP / QXGA) |
| Interface | DVP (parallel 8-bit) |
| Output formats | JPEG, YUV422, RGB565, Grayscale |
| JPEG | Hardware encoder onboard |
| XCLK | 20 MHz |

### Pin Map (AI Thinker)

| Signal | GPIO |
|---|---|
| PWDN | 32 |
| RESET | — (not connected) |
| XCLK | 0 |
| SIOD (SDA) | 26 |
| SIOC (SCL) | 27 |
| D7–D0 | 35, 34, 39, 36, 21, 19, 18, 5 |
| VSYNC | 25 |
| HREF | 23 |
| PCLK | 22 |

### Where to Buy

| Board | Link |
|---|---|
| AI Thinker ESP32-CAM | [Official product page](https://vdoc.ai-thinker.com/en/esp32-cam) · [Amazon](https://www.amazon.com/esp32-cam-ai-thinker/s?k=esp32+cam+ai+thinker) · [DigiKey](https://www.digikey.com/en/products/detail/universal-solder-electronics-ltd/Ai-Thinker-ESP32-CAM-WiFi-BT-BLE/14319899) |
| DFRobot ESP32-S3 AI Camera (DFR1154) | [DFRobot store](https://www.dfrobot.com/product-2899.html) · [Wiki / docs](https://wiki.dfrobot.com/SKU_DFR1154_ESP32_S3_AI_CAM) |

---

## 📚 References

**Anedya**
- [Anedya Overview](https://docs.anedya.io/anedya-overview/)
- [Anedya Concepts](https://docs.anedya.io/essentials/concepts/)
- [Anedya Project Setup](https://docs.anedya.io/getting-started/project-setup/)
- [Anedya MQTT Endpoints](https://docs.anedya.io/device/mqtt-endpoints/)
- [Anedya ValueStore](https://docs.anedya.io/features/valuestore/valuestore-intro/)
- [Anedya Platform API](https://docs.anedya.io/platform-api/)

**WebRTC & ESP-IDF**
- [WebRTC Overview](https://webrtc.org/getting-started/overview)
- [WebRTC Peer Connections](https://webrtc.org/getting-started/peer-connections)
- [espressif/esp_peer](https://components.espressif.com/components/espressif/esp_peer)
- [espressif/esp32-camera](https://components.espressif.com/components/espressif/esp32-camera)
- [ESP-IDF Getting Started](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)

---

## License

This project is licensed under the [Apache License 2.0](LICENSE).
