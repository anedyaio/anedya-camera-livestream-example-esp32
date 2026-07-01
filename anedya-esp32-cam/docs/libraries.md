# ESP32 Camera Livestream — Library Reference

Quick reference for every library in this ecosystem and why it exists.

---

## 1. `esp32-camera` — Hardware Sensor Driver

**Version used:** 2.1.6  
**Registry:** espressif/esp32-camera

### Problem it solves
Need raw pixel data out of a physical camera sensor (OV3660, OV2640, etc.). Someone has to talk to the chip over DVP + I2C, configure its registers, and DMA pixels into RAM. That's this library.

### What it does
- Initializes camera sensor over I2C (SCCB)
- Drives DVP parallel interface + DMA to fill frame buffer
- Supports 15+ sensors: OV3660, OV2640, OV5640, OV7670, GC2145, etc.
- Output formats: JPEG, YUV422, RGB565, RGB888, Grayscale, Raw Bayer

### Key API
```c
esp_camera_init(&config);       // init sensor + DMA
esp_camera_fb_get();            // get pointer to filled frame buffer
esp_camera_fb_return(fb);       // release buffer back to pool
esp_camera_sensor_get();        // get sensor handle (read PID, adjust settings)
```

### Does NOT do
- Encoding, streaming, WebRTC, pipelines
- Displays nothing, sends nothing
- Just gives you a buffer of bytes

### Constraints
- Requires PSRAM for anything above CIF resolution in non-JPEG mode
- PSRAM DMA mode disabled on ESP32 (only S2/S3 support it)
- First 2–3 frames after init are garbage (sensor PLL settling) — discard them

### Your project
**Used.** Gets JPEG frames from OV3660. Entry point for all video data.

---

## 2. `esp_jpeg` — Software JPEG Decoder

**Version used:** 1.3.1  
**Registry:** espressif/esp_jpeg

### Problem it solves
You have a JPEG byte buffer and need raw pixels (RGB565/RGB888) — to draw on a display or feed into an AI model.

### What it does
- Decodes JPEG → raw pixel formats
- Based on TJpgDec (Tiny JPEG Decompressor)
- Supports output scaling (1/2, 1/4, 1/8)
- Blocking call, minimal RAM usage

### Key API
```c
esp_jpeg_image_cfg_t cfg = {
    .indata      = jpeg_buf,
    .indata_size = jpeg_size,
    .outbuf      = rgb_buf,
    .out_format  = JPEG_IMAGE_OUT_FORMAT_RGB565,
    .out_scale   = JPEG_IMAGE_SCALE_0,
};
esp_jpeg_decode(&cfg, &outimg);
```

### Does NOT do
- Encoding (camera hardware does that)
- Streaming, display driving, WebRTC

### Constraints
- ESP32 ROM has a fixed TJpgDec (512-byte buffer, RGB888 only) — this component lets you override it
- Blocking — no async/DMA decode

### Your project
**Not needed.** You send JPEG out to the browser. You don't decode locally.  
Would be needed if you added a local display or ran AI inference on frames.

---

## 3. `esp_capture` — Multimedia Pipeline Manager

**Version used:** 0.8.4  
**Registry:** espressif/esp_capture

### Problem it solves
You need audio + video captured together, processed (resize, AEC, rate-convert), synchronized, and output to a file or stream — without manually wiring each stage.

### What it does
- Pipeline framework: source → encoder → muxer → sink
- Video: V4L2 or DVP cameras → H264/MJPEG encode → TS/MP4
- Audio: I2S/USB codec → opus/AAC encode → synchronized with video
- Features: text overlay, video scaling/rotation, acoustic echo cancellation
- Use cases: MP4 recording, RTMP streaming, RTSP server, AI input pipelines

### Key API
```c
esp_capture_open(&cfg, &handle);
esp_capture_start(handle);
esp_capture_sink_acquire_frame(handle, sink_id, &frame);
esp_capture_stop(handle);
```

### Does NOT do
- WebRTC (no ICE/DTLS/SDP)
- Raw sensor init (delegates to esp32-camera or V4L2)

### Constraints
- 9 dependencies (ESP-GMF framework) — significant binary size
- Auto-negotiation can fail in complex pipelines

### Your project
**Not needed.** No audio, no MP4/RTMP. Wrong abstraction for WebRTC.  
Would be needed if adding audio, recording to SD card, or RTMP streaming.

---

## 4. `esp_peer` — WebRTC PeerConnection Engine

**Version used:** 1.4.1  
**Registry:** espressif/esp_peer

### Problem it solves
Need actual WebRTC on ESP32 to communicate directly with a browser — ICE negotiation, DTLS handshake, SRTP media encryption — without implementing RFC5764/RFC5245/RFC4347 yourself.

### What it does
- Full WebRTC protocol stack:
  - **ICE** — NAT traversal, STUN/TURN candidate gathering
  - **DTLS** — encrypted session setup (uses mbedTLS)
  - **SRTP** — encrypted RTP media transport
  - **SCTP** — data channel transport
- Supports: audio (G711/Opus), video (H264/MJPEG), data channels
- API mirrors browser `RTCPeerConnection` — familiar if you know JS WebRTC
- <60KB RAM in minimal config
- Derived from libpeer, optimized for Espressif hardware

### Key API
```c
esp_peer_open(&cfg, &handle);           // like new RTCPeerConnection()
esp_peer_new_connection(handle, offer); // process remote SDP offer
esp_peer_send_video(handle, data, len); // send encoded video frame
esp_peer_send_data(handle, data, len);  // send data channel message
esp_peer_close(handle);
```

### Does NOT do
- **Signaling** — you must exchange SDP offer/answer yourself
  (via HTTP, WebSocket, MQTT — your choice)
- Camera capture — you feed it frames from esp32-camera
- Encoding — you provide already-encoded data (JPEG or H264)

### Constraints
- Requires mbedTLS + libSRTP in sdkconfig
- No built-in signaling server

### Your project
**Core engine needed.** Anedya MQTT handles signaling. esp32-camera provides frames. esp_peer connects them to the browser.

---

## 5. `esp_webrtc` — Full-Stack WebRTC Solution

**Source:** github.com/espressif/esp-webrtc-solution  
**Component path:** components/esp_webrtc

### Problem it solves
Want a complete, opinionated WebRTC video call solution (doorbell, intercom, surveillance camera) without wiring all layers together yourself.

### What it does
Three-layer bundled stack:
```
[Signaling layer]   — SDP offer/answer exchange (pluggable)
      ↓
[esp_peer layer]    — ICE/DTLS/SRTP (WebRTC protocol)
      ↓
[esp_capture layer] — camera + mic capture pipeline
```
- `esp_webrtc_start/stop` manages the whole session
- Handles media negotiation, codec selection, A/V sync automatically
- Repo also includes: `av_render`, `codec_board`, `media_lib_utils`, `webrtc_utils`

### Key API
```c
esp_webrtc_cfg_t cfg = { .signaling = &my_signaling, ... };
esp_webrtc_start(&cfg, &handle);
esp_webrtc_stop(handle);
```

### Does NOT do
- Custom signaling easily — it's opinionated about the stack
- Work without esp_capture (audio+video pipeline required)

### Constraints
- Heavy — pulls in esp_capture (9 deps) + esp_peer + signaling
- Designed for specific demo topologies, harder to customize

### Your project
**Optional / not recommended.** You'd use only esp_peer from this ecosystem. esp_webrtc forces esp_capture (which you don't need) and makes custom Anedya MQTT signaling harder to integrate.

---

## Decision Summary for This Project

```
OV3660 sensor
    ↓
esp32-camera          ← grab JPEG frame buffer
    ↓
esp_peer              ← WebRTC: ICE + DTLS + SRTP → browser
    ↑
Anedya MQTT           ← signaling: SDP offer/answer exchange
```

| Library | Need it? | Reason |
|---|---|---|
| `esp32-camera` | Yes | Only way to get frames from OV3660 |
| `esp_jpeg` | No | Decode side only; we send JPEG out |
| `esp_capture` | No | No audio, no recording, no RTMP |
| `esp_peer` | Yes | The WebRTC engine |
| `esp_webrtc` | No | Bundled stack; Anedya replaces its signaling layer |
