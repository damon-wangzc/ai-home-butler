#include "ws_client.h"
#include "board.h"
#include <esp_log.h>
#include <esp_websocket_client.h>
#include <cJSON.h>
#include <cstring>

static const char* TAG = "WSClient";

// ── Connect ───────────────────────────────────────────────────────────────────

void WSClient::connect(const char* host, uint16_t port) {
    char uri[128];
    snprintf(uri, sizeof(uri), "ws://%s:%u/audio", host, port);

    esp_websocket_client_config_t cfg = {};
    cfg.uri                  = uri;
    cfg.buffer_size          = 8192;
    cfg.reconnect_timeout_ms = 5000;
    cfg.network_timeout_ms   = 10000;  // suppress "not set" warning

    ws_handle_ = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(
        (esp_websocket_client_handle_t)ws_handle_,
        WEBSOCKET_EVENT_ANY,
        event_handler,
        this
    );
    esp_websocket_client_start((esp_websocket_client_handle_t)ws_handle_);
    ESP_LOGI(TAG, "connecting to %s", uri);
}

// ── Send audio ────────────────────────────────────────────────────────────────

bool WSClient::send_audio(const uint8_t* data, size_t len) {
    if (!connected_ || !ws_handle_) return false;
    int ret = esp_websocket_client_send_bin(
        (esp_websocket_client_handle_t)ws_handle_,
        (const char*)data, (int)len, pdMS_TO_TICKS(50));
    return ret == ESP_OK;
}

// ── Send wake notification ───────────────────────────────────────────────────────

void WSClient::send_wake(const char* user_id) {
    if (!connected_ || !ws_handle_) return;
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"type\":\"wake\",\"user_id\":\"%s\"}",
             user_id ? user_id : "default");
    esp_websocket_client_send_text(
        (esp_websocket_client_handle_t)ws_handle_,
        buf, (int)strlen(buf), pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "wake sent user_id=%s", user_id);
}

// ── Send VAD end ──────────────────────────────────────────────────────────────

void WSClient::send_vad_end() {
    if (!connected_ || !ws_handle_) return;
    static const char* msg = "{\"type\":\"vad_end\"}";
    esp_websocket_client_send_text(
        (esp_websocket_client_handle_t)ws_handle_,
        msg, (int)strlen(msg), pdMS_TO_TICKS(200));
    ESP_LOGD(TAG, "vad_end sent");
}

// ── Disconnect ────────────────────────────────────────────────────────────────

void WSClient::disconnect() {
    if (ws_handle_) {
        esp_websocket_client_stop((esp_websocket_client_handle_t)ws_handle_);
        esp_websocket_client_destroy((esp_websocket_client_handle_t)ws_handle_);
        ws_handle_  = nullptr;
        connected_  = false;
    }
}

// ── Event handler ─────────────────────────────────────────────────────────────

void WSClient::event_handler(void* arg, const char* /*event_base*/, int32_t event_id, void* event_data) {
    WSClient* self = static_cast<WSClient*>(arg);
    esp_websocket_event_data_t* data = (esp_websocket_event_data_t*)event_data;

    switch ((esp_websocket_event_id_t)event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            self->on_connected();
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            self->on_disconnected();
            break;

        case WEBSOCKET_EVENT_DATA:
            if (data && data->data_len > 0) {
                bool is_binary = (data->op_code == 0x02);
                self->on_data(data->data_ptr, data->data_len, is_binary);
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGW(TAG, "WebSocket error");
            self->on_disconnected();
            break;

        default:
            break;
    }
}

// ── Internal callbacks ────────────────────────────────────────────────────────

void WSClient::on_connected() {
    connected_ = true;
    ESP_LOGI(TAG, "connected");
    if (state_cb_) state_cb_(DEVICE_STATE_IDLE);
}

void WSClient::on_disconnected() {
    connected_ = false;
    ESP_LOGW(TAG, "disconnected");
    if (state_cb_) state_cb_(DEVICE_STATE_ERROR);
}

void WSClient::on_data(const char* data, size_t len, bool is_binary) {
    if (is_binary) {
        // TTS PCM audio — forward to audio pipeline for playback
        if (audio_cb_) audio_cb_((const uint8_t*)data, len);
    } else {
        // JSON text frame — parse for state/emotion/text
        parse_json_frame(data, len);
    }
}

void WSClient::parse_json_frame(const char* json, size_t len) {
    // Null-terminate for cJSON (safe since we copy)
    char buf[512];
    size_t copy_len = std::min(len, sizeof(buf) - 1);
    memcpy(buf, json, copy_len);
    buf[copy_len] = '\0';

    cJSON* root = cJSON_Parse(buf);
    if (!root) return;

    cJSON* type_j = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char* type = cJSON_IsString(type_j) ? type_j->valuestring : "";

    if (strcmp(type, "state") == 0) {
        cJSON* state_j = cJSON_GetObjectItemCaseSensitive(root, "state");
        if (cJSON_IsString(state_j)) {
            const char* s = state_j->valuestring;
            device_state_t ds = DEVICE_STATE_IDLE;
            if      (strcmp(s, "listening") == 0) ds = DEVICE_STATE_LISTENING;
            else if (strcmp(s, "thinking")  == 0) ds = DEVICE_STATE_THINKING;
            else if (strcmp(s, "speaking")  == 0) ds = DEVICE_STATE_SPEAKING;
            else if (strcmp(s, "idle")      == 0) ds = DEVICE_STATE_IDLE;
            else if (strcmp(s, "error")     == 0) ds = DEVICE_STATE_ERROR;
            if (state_cb_) state_cb_(ds);
        }
    } else if (strcmp(type, "emotion") == 0 || strcmp(type, "end") == 0) {
        cJSON* text_j = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text_j) && text_cb_) {
            text_cb_(text_j->valuestring);
        }
        if (strcmp(type, "end") == 0 && state_cb_) {
            state_cb_(DEVICE_STATE_IDLE);
        }
    }

    cJSON_Delete(root);
}
