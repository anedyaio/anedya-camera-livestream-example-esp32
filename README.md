[<img src="https://img.shields.io/badge/Anedya-Documentation-blue?style=for-the-badge">](https://docs.anedya.io?utm_source=github&utm_medium=link&utm_campaign=github-examples&utm_content=esp-cam)
[<img src="https://img.shields.io/badge/Peer-Live-blue?style=for-the-badge">](https://anedyaio.github.io/anedya-camera-livestream-example-esp32/)


<p align="center">
    <img src="https://cdn.anedya.io/anedya_black_banner.png" alt="Logo">
</p>

# ESP32- WebRTC Camera Livestream with Anedya

![Camera View](./media/camera_view.png)

Turn an ESP32-Camera board into a real-time camera livestream device with Anedya (Commands and TURN relay).

## ✨ Features

- **Signaling :** SDP offer/answer and ICE candidates exchanged via MQTT, no custom signaling server needed
- **TURN Relay :** TURN server provided by Anedya to relay media streams.
- **Live-Remote Video streaming :** Camera frames sent over WebRTC DataChannel. [View Here](https://anedyaio.github.io/anedya-camera-livestream-example-esp32/)
- **Realtime Audio Support :** Support for audio streaming over WebRTC DataChannel.

---

## 📷 Anedya - Camera Board Support

| Board | Support Status | Example Project | Product Link |
|---|---|--|--|
| ESP32-CAM | Supported | [Link](./anedya-esp32-cam/)| [Link](https://vdoc.ai-thinker.com/en/esp32-cam) |
| DFRobot ESP32-S3 AI Camera  | under testing | Link | [Link](https://www.dfrobot.com/product-2899.html) |
| Seed Studio XIAO ESP32S3 Sense  | under testing | Link | [Link](https://www.seeedstudio.com/XIAO-ESP32S3-Sense-p-5639.html) |

---

## 🏗 How It Works

### Signaling via Anedya Commands + MQTT

WebRTC requires both peers to exchange SDP offers and answers before media can flow. This example uses Anedya Commands as a signaling channel and Anedya MQTT as the notification mechanism.

```
Browser Viewer
  │  1. Fetch TURN credentials (Anedya REST API)
  │  2. Create WebRTC offer to Commands (JSON with SDP + TURN creds)
  ▼
Anedya Cloud  (Commands + MQTT broker + TURN relay)
  │  3. Notify ESP32 over MQTT subscription
  ▼
ESP32-CAMARA
  │  4. Parse offer, extract SDP + TURN credentials
  │  5. Create WebRTC answer ackowledgement to Commands
  ▼
Browser Viewer
  │  6. Poll Commands status → read answer ackowledgement → apply remote description
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

## 📚 References

**Looking for other examples**

- [Anedya Camera Livestream with Raspberry Pi](https://github.com/anedyaio/anedya-camera-livestream-example)

---


## License

This project is licensed under the [MIT](LICENSE).
