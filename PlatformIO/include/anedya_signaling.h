#pragma once

#include <stdbool.h>
#include <stddef.h>

// =============================================================================
// anedya_signaling — WebRTC signaling over Anedya Commands (MQTT).
//
// Arduino port of the ESP-IDF example's anedya_signaling.c. Instead of the
// Anedya ESP-IDF SDK it speaks the same MQTT endpoints directly with
// PubSubClient + ArduinoJson, exactly like Anedya's own Arduino examples do.
// The wire format is unchanged, so the same browser viewer works with both.
// =============================================================================

// Connect to the Anedya MQTT broker and subscribe to the command topic.
// Call after WiFi is up. Non-fatal on failure — anedyaSignalingLoop() retries.
void anedyaSignalingBegin();

// Pump MQTT, publish queued command status updates, and heartbeat Anedya.
// Call every loop() iteration.
void anedyaSignalingLoop();

// True while the MQTT session to Anedya is up.
bool anedyaSignalingConnected();

// Publish the local SDP answer as the in-flight webrtc_offer command's ack
// data, marking the command "processing" (not yet concluded). The SDP is
// deflate+base64 encoded to fit the ~1 KB command payload budget.
//
// Safe to call from the esp_peer task: the work is queued and the actual MQTT
// publish happens on the loop() task.
void anedyaSignalingWriteAnswer(const char *sdp, size_t sdpLength);

// Conclude the in-flight command with a terminal status: data channel
// connected -> success, connect failed -> failure. `reason` is optional and is
// attached as ack data on failure. Also safe to call from the esp_peer task.
void anedyaSignalingConclude(bool success, const char *reason);
