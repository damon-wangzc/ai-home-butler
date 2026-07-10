idf_component_register(
    SRCS
        "main.cpp"
        "display/face_display.cpp"
        "display/ui_manager.cpp"
        "audio/audio_pipeline.cpp"
        "network/ws_client.cpp"
        "network/mqtt_client.cpp"
    INCLUDE_DIRS
        "."
        "hal"
        "display"
        "audio"
        "network"
    REQUIRES
        esp_wifi
        esp_event
        nvs_flash
        driver
        esp_lcd                  # built-in IDF — panel IO, ops
        lvgl                     # from component registry
        esp_sr                   # from component registry — wake word + AFE
        esp_mqtt                 # built-in IDF — MQTT client
        esp_websocket_client     # from component registry (espressif/esp_websocket_client)
        json                     # built-in IDF — cJSON
        freertos
        log
)
