#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "webrtc_peer.h"

// Start Anedya MQTT signaling. Must be called after WiFi is connected.
void anedya_sig_init(void);

// Publish the local SDP answer as the ackdata of the in-flight "webrtc_offer"
// command, marking it "processing" (not yet concluded — see
// anedya_sig_command_conclude). deflate+base64 encoded. The actual MQTT publish
// is done by a background task so the esp_peer callback that calls this stays fast.
void anedya_sig_write_answer(const char *sdp, size_t sdp_len);

// Conclude the in-flight "webrtc_offer" command as success or failure. Call when
// the WebRTC connection reaches a terminal outcome (data channel connected ->
// success; connect failed -> failure). success/failure are terminal in Anedya, so
// this is the final status update for the command. `reason` (optional, may be
// NULL) is a short human-readable string attached as ackdata on failure.
void anedya_sig_command_conclude(bool success, const char *reason);
