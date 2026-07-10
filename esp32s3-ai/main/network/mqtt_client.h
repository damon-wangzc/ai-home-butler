#pragma once

#include "device_state.h"
#include <cstdint>
#include <cstddef>   // size_t

// ============================================================
// OrbMqttClient — MQTT integration with the butler broker.
//
// Subscribes:
//   butler/orb/state  → JSON {"state":"...","text":"..."}
//                        drives UIManager on_state_change + on_chat_text
//
// Publishes:
//   butler/user/context → {"user_id":"kid","source":"orb","action":"..."}
//   butler/orb/touch    → {"type":"tap","ts":...}
// ============================================================

class OrbMqttClient {
public:
    static OrbMqttClient& instance() {
        static OrbMqttClient inst;
        return inst;
    }

    // Connect to mqtt://host:1883
    void connect(const char* host, uint16_t port = 1883,
                 const char* user_id = "kid");

    // Publish wake / touch events
    void publish_wake();
    void publish_touch();

    // Publish device state (called on state transitions)
    void publish_state(device_state_t state);

private:
    OrbMqttClient() = default;

    void*       client_  = nullptr;  // esp_mqtt_client_handle_t
    const char* user_id_ = "kid";

    // esp_event_handler_t signature (4 params) required by esp_mqtt_client_register_event
    static void event_handler(void* arg, const char* event_base, int32_t event_id, void* event_data);
    void on_message(const char* topic, const char* payload, size_t len);
};
