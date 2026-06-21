#pragma once

#include <stddef.h>

// Called by the signaling module when the browser writes offer_<sessionId>.
// The payload is the raw SDP offer string that the browser generated.
void webrtc_peer_on_offer(const char *session_id, const char *sdp, size_t sdp_len);

// Called by the signaling module when the browser writes candidate_<sessionId>_<n>.
void webrtc_peer_on_remote_candidate(const char *candidate, size_t candidate_len);

// Start Anedya MQTT signaling. Must be called after WiFi is connected.
void anedya_sig_init(void);

// Queue the local SDP answer to ValueStore as answer_<session_id>.
// The actual write is done by a background task so esp_peer callbacks stay fast.
void anedya_sig_write_answer(const char *session_id, const char *sdp, size_t sdp_len);

// Queue one local ICE candidate as candidate_esp_<session_id>_<idx>.
void anedya_sig_write_candidate(const char *session_id, int idx,
                                const char *candidate, size_t candidate_len);
