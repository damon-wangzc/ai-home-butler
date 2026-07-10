#include "mqtt_client.h"
#include "ui_manager.h"
#include <esp_log.h>
#include <mqtt_client.h>  // IDF 'mqtt' component
#include <cJSON.h>
#include <cstring>
#include <ctime>

static const char* TAG = "OrbMQTT";

void OrbMqttClient::connect(const char* host, uint16_t port,
                             const char* user_id) {
    user_id_ = user_id;

    char uri[64];
    snprintf(uri, sizeof(uri), "mqtt://%s:%u", host, port);

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = uri;

    client_ = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(
        (esp_mqtt_client_handle_t)client_,
        (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
        event_handler,
        this
    );
    esp_mqtt_client_start((esp_mqtt_client_handle_t)client_);
    ESP_LOGI(TAG, "connecting to %s", uri);
}

void OrbMqttClient::publish_wake() {
    if (!client_) return;
    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"user_id\":\"%s\",\"source\":\"orb\",\"action\":\"wake\"}", user_id_);
    esp_mqtt_client_publish(
        (esp_mqtt_client_handle_t)client_,
        "butler/user/context", payload, 0, 0, 0);
}

void OrbMqttClient::publish_touch() {
    if (!client_) return;
    char payload[64];
    snprintf(payload, sizeof(payload),
             "{\"type\":\"tap\",\"ts\":%lld}", (long long)time(nullptr));
    esp_mqtt_client_publish(
        (esp_mqtt_client_handle_t)client_,
        "butler/orb/touch", payload, 0, 0, 0);
}

void OrbMqttClient::publish_state(device_state_t state) {
    if (!client_) return;
    char payload[64];
    snprintf(payload, sizeof(payload),
             "{\"state\":\"%s\"}", device_state_str(state));
    esp_mqtt_client_publish(
        (esp_mqtt_client_handle_t)client_,
        "butler/orb/state", payload, 0, 0, 0);
}

void OrbMqttClient::event_handler(void* arg, int32_t event_id, void* event_data) {
    OrbMqttClient* self = static_cast<OrbMqttClient*>(arg);
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "connected");
            esp_mqtt_client_subscribe(
                (esp_mqtt_client_handle_t)self->client_,
                "butler/orb/state", 0);
            break;

        case MQTT_EVENT_DATA:
            if (event->topic && event->data) {
                self->on_message(event->topic, event->data, event->data_len);
            }
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "disconnected");
            break;

        default:
            break;
    }
}

void OrbMqttClient::on_message(const char* topic, const char* payload, size_t len) {
    if (strncmp(topic, "butler/orb/state", 16) != 0) return;

    char buf[256];
    size_t copy_len = std::min(len, sizeof(buf) - 1);
    memcpy(buf, payload, copy_len);
    buf[copy_len] = '\0';

    cJSON* root = cJSON_Parse(buf);
    if (!root) return;

    cJSON* state_j = cJSON_GetObjectItemCaseSensitive(root, "state");
    cJSON* text_j  = cJSON_GetObjectItemCaseSensitive(root, "text");

    if (cJSON_IsString(state_j)) {
        const char* s = state_j->valuestring;
        device_state_t ds = DEVICE_STATE_IDLE;
        if      (strcmp(s, "listening") == 0) ds = DEVICE_STATE_LISTENING;
        else if (strcmp(s, "thinking")  == 0) ds = DEVICE_STATE_THINKING;
        else if (strcmp(s, "speaking")  == 0) ds = DEVICE_STATE_SPEAKING;
        else if (strcmp(s, "error")     == 0) ds = DEVICE_STATE_ERROR;
        UIManager::instance().on_state_change(ds);
    }

    if (cJSON_IsString(text_j)) {
        UIManager::instance().on_chat_text(text_j->valuestring);
    }

    cJSON_Delete(root);
}
