#include "audio_pipeline.h"
#include "board.h"
#include <esp_log.h>
#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include <cmath>
#include <cstring>

static const char* TAG = "AudioPipeline";

// I2S handles (file-scope so both tasks can access)
static i2s_chan_handle_t s_tx_chan = nullptr;  // speaker
static i2s_chan_handle_t s_rx_chan = nullptr;  // mic

// Playback queue: chunks of PCM data
#define PLAY_QUEUE_DEPTH 8
#define PLAY_CHUNK_BYTES (AUDIO_DMA_BUF_SAMPLES * 2)  // 16-bit samples

typedef struct {
    uint8_t data[PLAY_CHUNK_BYTES];
    size_t  len;
} play_chunk_t;

static QueueHandle_t s_play_queue = nullptr;

// VAD silence counter (units of 20ms frames)
static const int VAD_SILENCE_FRAMES = AUDIO_SAMPLE_RATE / 1000
                                      * 700 / AUDIO_DMA_BUF_SAMPLES;

// ── Init ──────────────────────────────────────────────────────────────────────

void AudioPipeline::init() {
    s_play_queue = xQueueCreate(PLAY_QUEUE_DEPTH, sizeof(play_chunk_t));

    // Speaker I2S (standard simplex TX)
    i2s_chan_config_t tx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_SPEAKER_PORT,
                                                           I2S_ROLE_MASTER);
    i2s_new_channel(&tx_cfg, &s_tx_chan, nullptr);
    i2s_std_config_t tx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                     I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)PIN_I2S_BCLK,
            .ws   = (gpio_num_t)PIN_I2S_LCLK,
            .dout = (gpio_num_t)PIN_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    i2s_channel_init_std_mode(s_tx_chan, &tx_std);
    i2s_channel_enable(s_tx_chan);

    // Microphone I2S (standard simplex RX)
    i2s_chan_config_t rx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_MIC_PORT,
                                                           I2S_ROLE_MASTER);
    i2s_new_channel(&rx_cfg, nullptr, &s_rx_chan);
    i2s_std_config_t rx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                     I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)PIN_MIC_BCLK,
            .ws   = (gpio_num_t)PIN_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)PIN_MIC_DIN,
        },
    };
    i2s_channel_init_std_mode(s_rx_chan, &rx_std);
    i2s_channel_enable(s_rx_chan);

    // Spawn tasks (pinned to core 1 to leave core 0 for LVGL)
    xTaskCreatePinnedToCore(mic_task, "mic",  4096, this, 5, nullptr, 1);
    xTaskCreatePinnedToCore(spk_task, "spk",  2048, this, 5, nullptr, 1);

    ESP_LOGI(TAG, "init done (sample_rate=%d)", AUDIO_SAMPLE_RATE);
}

// ── Public: play TTS PCM ──────────────────────────────────────────────────────

void AudioPipeline::play_pcm(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;
    size_t offset = 0;
    while (offset < len) {
        play_chunk_t chunk;
        size_t chunk_len = std::min((size_t)PLAY_CHUNK_BYTES, len - offset);
        memcpy(chunk.data, data + offset, chunk_len);
        chunk.len = chunk_len;
        xQueueSend(s_play_queue, &chunk, pdMS_TO_TICKS(20));
        offset += chunk_len;
    }
}

// ── Public: manual wake ───────────────────────────────────────────────────────

void AudioPipeline::trigger_wake() {
    on_wake_word();
}

// ── Mic task (core 1) ─────────────────────────────────────────────────────────

void AudioPipeline::mic_task(void* arg) {
    AudioPipeline* self = static_cast<AudioPipeline*>(arg);

    const size_t buf_bytes = AUDIO_DMA_BUF_SAMPLES * sizeof(int16_t);
    int16_t* buf = (int16_t*)heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to alloc mic buffer");
        vTaskDelete(nullptr);
    }

    int silence_frames = 0;

    // TODO: initialise ESP-SR AFE + wake word handle here.
    // Reference: esp-sr component, esp_afe_sr_iface_t + esp_wn_iface_t.
    // For now the loop just reads audio and does VAD by RMS threshold.

    while (true) {
        size_t bytes_read = 0;
        i2s_channel_read(s_rx_chan, buf, buf_bytes, &bytes_read, pdMS_TO_TICKS(100));
        int samples = bytes_read / sizeof(int16_t);

        float rms = self->compute_rms(buf, samples);

        if (self->streaming_) {
            // Send frame to orchestrator
            if (self->send_cb_) {
                self->send_cb_((const uint8_t*)buf, bytes_read);
            }
            // RMS for mouth animation
            if (self->rms_cb_) self->rms_cb_(rms);

            // VAD: count silent frames
            if (rms < 0.01f) {
                if (++silence_frames >= VAD_SILENCE_FRAMES) {
                    self->on_vad_silence();
                    silence_frames = 0;
                }
            } else {
                silence_frames = 0;
            }
        }
        // TODO: feed buf to ESP-SR AFE; call on_wake_word() on detection
    }
}

// ── Speaker task (core 1) ─────────────────────────────────────────────────────

void AudioPipeline::spk_task(void* arg) {
    AudioPipeline* self = static_cast<AudioPipeline*>(arg);
    play_chunk_t chunk;

    while (true) {
        if (xQueueReceive(s_play_queue, &chunk, pdMS_TO_TICKS(50)) == pdTRUE) {
            size_t written = 0;
            i2s_channel_write(s_tx_chan, chunk.data, chunk.len,
                              &written, pdMS_TO_TICKS(100));
            // Feed RMS to mouth sync
            if (self->rms_cb_) {
                float rms = self->compute_rms((const int16_t*)chunk.data,
                                              chunk.len / sizeof(int16_t));
                self->rms_cb_(rms);
            }
        }
    }
}

// ── Internal ──────────────────────────────────────────────────────────────────

void AudioPipeline::on_wake_word() {
    if (streaming_) return;
    streaming_ = true;
    ESP_LOGI(TAG, "wake word detected → LISTENING");
    if (state_cb_) state_cb_(DEVICE_STATE_LISTENING);
}

void AudioPipeline::on_vad_silence() {
    if (!streaming_) return;
    streaming_ = false;
    ESP_LOGI(TAG, "VAD silence → THINKING");
    if (state_cb_) state_cb_(DEVICE_STATE_THINKING);
}

float AudioPipeline::compute_rms(const int16_t* samples, int count) {
    if (count <= 0) return 0.0f;
    float sum = 0.0f;
    for (int i = 0; i < count; i++) {
        float s = samples[i] / 32768.0f;
        sum += s * s;
    }
    return sqrtf(sum / count);
}
