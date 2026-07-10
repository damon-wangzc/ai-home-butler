#pragma once

#include "device_state.h"
#include <functional>
#include <cstddef>
#include <cstdint>

// ============================================================
// AudioPipeline — I2S mic capture, VAD, wake word detection,
// streaming to WebSocket client, I2S speaker playback.
//
// Usage:
//   AudioPipeline& ap = AudioPipeline::instance();
//   ap.set_state_cb([](device_state_t s){ ... });
//   ap.set_rms_cb([](float rms){ ... });
//   ap.set_send_frame_cb([](uint8_t* buf, size_t len){ ... });
//   ap.init();
//   // Feed incoming TTS audio:
//   ap.play_pcm(buf, len);
// ============================================================

class AudioPipeline {
public:
    static AudioPipeline& instance() {
        static AudioPipeline inst;
        return inst;
    }

    // Callbacks — set before init()
    using StateCb = std::function<void(device_state_t)>;
    using RmsCb   = std::function<void(float)>;
    using SendCb  = std::function<void(const uint8_t*, size_t)>;

    void set_state_cb(StateCb cb)      { state_cb_  = cb; }
    void set_rms_cb(RmsCb cb)          { rms_cb_    = cb; }
    void set_send_frame_cb(SendCb cb)  { send_cb_   = cb; }

    // Initialise I2S driver, ESP-SR model, VAD
    void init();

    // Feed incoming TTS PCM (16-bit LE mono 16kHz) for playback
    // Thread-safe: can be called from WebSocket receive callback
    void play_pcm(const uint8_t* data, size_t len);

    // Manual trigger (touch / button) — same as wake word
    void trigger_wake();

private:
    AudioPipeline() = default;

    StateCb state_cb_;
    RmsCb   rms_cb_;
    SendCb  send_cb_;

    bool streaming_ = false;

    // FreeRTOS tasks
    static void mic_task(void* arg);    // reads I2S mic, runs VAD + wake word
    static void spk_task(void* arg);    // drains play_queue_ to I2S speaker

    // Internal helpers
    void on_wake_word();
    void on_vad_silence();
    float compute_rms(const int16_t* samples, int count);
};
