# Video Pipeline Options — ESP32-CAM WebRTC

Research findings on camera output formats and WebRTC video transport options.

---

## esp32-camera Output Formats

Set via `pixel_format` in `camera_config_t`.

| Format | Constant | Notes |
|---|---|---|
| JPEG | `PIXFORMAT_JPEG` | Hardware encoder on OV3660, smallest output, ready to send |
| YUV422 | `PIXFORMAT_YUV422` | Raw YUV, needed as input for software H.264 encoders |
| RGB565 | `PIXFORMAT_RGB565` | Raw RGB, large frames, fills RAM fast |
| Grayscale | `PIXFORMAT_GRAYSCALE` | 8-bit luma only |
| Raw Bayer | `PIXFORMAT_RAW` | Unprocessed sensor data |

OV3660 has a **hardware JPEG encoder onboard** — use `PIXFORMAT_JPEG` whenever possible.  
YUV/RGB modes strain DMA and heap; only use if you need raw pixels for AI or encoding.

---

## WebRTC Browser Codec Support (verified via MDN RFC 7742)

Browsers mandate only two video codecs for WebRTC:

| Codec | Status | Browser support |
|---|---|---|
| **VP8** | Mandatory | All browsers |
| **H.264** (Constrained Baseline) | Mandatory | All browsers |
| VP9 | Optional | Chrome, Firefox |
| AV1 | Optional | Chrome 113+, Firefox 136+ |
| **MJPEG** | **NOT supported** | None |

MJPEG is not in the WebRTC spec. Browsers will not accept `video/JPEG` in SDP negotiation.

---

## esp_peer Video Codec Enums

From `esp_peer.h`:

```c
typedef enum {
    ESP_PEER_VIDEO_CODEC_NONE  = 0,
    ESP_PEER_VIDEO_CODEC_H264  = 1,
    ESP_PEER_VIDEO_CODEC_MJPEG = 2,
} esp_peer_video_codec_t;
```

Send a video frame:

```c
esp_peer_video_frame_t frame = {
    .pts  = timestamp_ms,
    .data = fb->buf,
    .size = fb->len,
};
esp_peer_send_video(peer_handle, &frame);
```

---

## The 3 Options

### Option 1: MJPEG over Data Channel ✅ CHOSEN

```
OV3660 → JPEG frame (hardware) → esp_peer data channel → browser JS → canvas
```

- No encoding on ESP32 — raw JPEG straight from sensor
- Not a native `<video>` track — JS draws each frame to `<canvas>`
- Works on all browsers, zero extra CPU load
- Proven approach, matches our 13.8 FPS baseline
- **This is what we implement**

**ESP32 side:**
```c
// config: no video codec, just enable data channel
.enable_data_channel = true,

// send each frame
camera_fb_t *fb = esp_camera_fb_get();
esp_peer_send_data(peer_handle, fb->buf, fb->len);
esp_camera_fb_return(fb);
```

**Browser side:**
```js
dataChannel.onmessage = (event) => {
    const blob = new Blob([event.data], { type: 'image/jpeg' });
    const url = URL.createObjectURL(blob);
    img.onload = () => URL.revokeObjectURL(url);
    img.src = url;
};
```

---

### Option 2: MJPEG over RTP video track ❌ AVOID

```
OV3660 → JPEG → esp_peer RTP (RFC 2435, ESP_PEER_VIDEO_CODEC_MJPEG) → browser
```

- esp_peer supports this codec enum
- **Browsers won't negotiate it** — Chrome dropped MJPEG RTP support years ago
- SDP negotiation will fail — browser rejects `video/JPEG` mime type
- Do not use unless targeting a non-browser receiver

---

### Option 3: H.264 over RTP ❌ NOT PRACTICAL ON ESP32

```
OV3660 → YUV frame → software H.264 encode → esp_peer RTP → browser <video>
```

- Real WebRTC video track, plays natively in browser `<video>` element
- **ESP32 has NO hardware H.264 encoder**
- Software encoding at QVGA = ~2–5 FPS, 100% CPU, unstable
- ESP32-S3 also has no H.264 hardware
- Only ESP32-P4 has hardware H.264 encoder
- Not viable on this hardware

---

## Decision

| | Option 1 (DataChannel JPEG) | Option 2 (RTP MJPEG) | Option 3 (RTP H.264) |
|---|---|---|---|
| Encoding needed | None | None | Yes (software) |
| Browser compatible | Yes | No | Yes |
| CPU cost | Near zero | Near zero | Very high |
| Native `<video>` tag | No (canvas) | No | Yes |
| Practical on ESP32 | **Yes** | No | No |

**Use Option 1.** Camera already delivers 13.8 FPS of hardware JPEG — pipe it straight into the WebRTC data channel.
