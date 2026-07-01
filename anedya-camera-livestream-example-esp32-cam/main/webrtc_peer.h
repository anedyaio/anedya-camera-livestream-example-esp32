#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Initialize esp_peer and start the cooperative WebRTC main-loop task.
// Must be called after WiFi is connected.
void webrtc_peer_init(void);

// Set TURN relay credentials to use for the next connection, sent alongside
// the offer by the browser (it already fetches them from Anedya's relay API).
// Pass NULL for either argument to fall back to STUN-only.
void webrtc_peer_set_turn_credentials(const char *username, const char *credential);

// Called by Anedya signaling when a "webrtc_offer" command arrives.
// The payload is the raw SDP offer string.
void webrtc_peer_on_offer(const char *sdp, size_t sdp_len);

// True after the browser-created "jpeg-test" DataChannel has opened.
bool webrtc_peer_data_channel_ready(void);

// Send a text string over the DataChannel (for test/debug use).
// Returns false if the channel is not ready or the send fails.
bool webrtc_peer_send_text(const char *text, int len);

// Queue one JPEG frame for transmission.
// This copies the frame and drops any older unsent frame to keep latency low.
bool webrtc_peer_send_jpeg(const uint8_t *data, size_t len);

typedef void (*webrtc_peer_jpeg_release_cb_t)(void *ctx);

// Queue one JPEG frame without copying it.
// The caller transfers temporary ownership until release_cb(ctx) is called.
bool webrtc_peer_send_jpeg_ref(const uint8_t *data, size_t len,
                               webrtc_peer_jpeg_release_cb_t release_cb,
                               void *ctx);
