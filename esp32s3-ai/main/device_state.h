#pragma once

typedef enum {
    DEVICE_STATE_UNKNOWN = 0,
    DEVICE_STATE_CONNECTING,    // WiFi + WS handshake
    DEVICE_STATE_IDLE,          // waiting for wake word / touch
    DEVICE_STATE_LISTENING,     // mic streaming to server
    DEVICE_STATE_THINKING,      // waiting for LLM response
    DEVICE_STATE_SPEAKING,      // playing TTS audio
    DEVICE_STATE_ERROR,         // connection lost / fatal
} device_state_t;

// String representation for logging and MQTT
static inline const char* device_state_str(device_state_t s) {
    switch (s) {
        case DEVICE_STATE_CONNECTING: return "connecting";
        case DEVICE_STATE_IDLE:       return "idle";
        case DEVICE_STATE_LISTENING:  return "listening";
        case DEVICE_STATE_THINKING:   return "thinking";
        case DEVICE_STATE_SPEAKING:   return "speaking";
        case DEVICE_STATE_ERROR:      return "error";
        default:                      return "unknown";
    }
}
