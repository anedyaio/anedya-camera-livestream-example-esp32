#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// =============================================================================
// webrtc_peer — esp_peer WebRTC connection + the JPEG send pipeline.
// =============================================================================

// Open esp_peer and start its cooperative main-loop task. Call after WiFi is up.
void webrtcPeerBegin();

// TURN relay credentials for the next connection; the browser fetches them from
// Anedya and ships them alongside the offer. NULL/empty falls back to STUN only.
void webrtcPeerSetTurnCredentials(const char *username, const char *credential);

// Hand a remote SDP offer to the peer task, which starts a new connection.
void webrtcPeerOnOffer(const char *sdp, size_t sdpLength);

// True once the browser-created DataChannel (WEBRTC_DATA_CHANNEL_LABEL) is open.
bool webrtcPeerDataChannelReady();

// Send a short text string over the DataChannel (test mode). False if the
// channel is not ready or the send failed.
bool webrtcPeerSendText(const char *text, int length);

typedef void (*WebrtcPeerJpegRelease)(void *context);

// Queue one JPEG frame WITHOUT copying it. The caller transfers ownership of
// the buffer until releaseCallback(context) runs on the peer task, which is how
// the camera frame buffer gets returned to the driver (zero-copy path).
// Returns false if the frame was rejected — the caller still owns the buffer.
bool webrtcPeerSendJpegByReference(const uint8_t *data, size_t length,
                                   WebrtcPeerJpegRelease releaseCallback,
                                   void *context);
