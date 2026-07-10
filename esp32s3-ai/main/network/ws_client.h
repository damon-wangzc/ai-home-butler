#pragma once

#include "device_state.h"
#include <functional>
#include <cstdint>
#include <cstddef>
#include <string>

// ============================================================
// WSClient — WebSocket connection to the butler orchestrator.
//
// Audio protocol:
//   → send: binary PCM frames (16-bit mono 16kHz)
//   ← recv: binary TTS PCM  OR  JSON text frame
//
// JSON frames from server:
//   {"type":"state",   "state":"thinking"}
//   {"type":"emotion", "emotion":"happy", "text":"..."}
//   {"type":"end"}
// ============================================================

class WSClient {
public:
    static WSClient& instance() {
        static WSClient inst;
        return inst;
    }

    // Callbacks — set before connect()
    using AudioCb  = std::function<void(const uint8_t*, size_t)>;
    using StateCb  = std::function<void(device_state_t)>;
    using TextCb   = std::function<void(const char*)>;

    void set_audio_cb(AudioCb cb) { audio_cb_ = cb; }
    void set_state_cb(StateCb cb) { state_cb_ = cb; }
    void set_text_cb(TextCb cb)   { text_cb_  = cb; }

    // Connect to ws://host:port/audio
    // Returns immediately; connection happens in background task
    void connect(const char* host, uint16_t port);

    // Send raw PCM frame (called from audio pipeline mic task)
    bool send_audio(const uint8_t* data, size_t len);

    bool is_connected() const { return connected_; }

    void disconnect();

private:
    WSClient() = default;

    AudioCb audio_cb_;
    StateCb state_cb_;
    TextCb  text_cb_;

    bool connected_ = false;
    void* ws_handle_ = nullptr;   // esp_websocket_client_handle_t

    static void event_handler(void* arg, int32_t event_id, void* event_data);
    void on_connected();
    void on_disconnected();
    void on_data(const char* data, size_t len, bool is_binary);
    void parse_json_frame(const char* json, size_t len);
};
