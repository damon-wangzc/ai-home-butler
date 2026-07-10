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
        esp_wifi              # built-in
        esp_event             # built-in
        esp_netif             # built-in — esp_netif_init() in main.cpp
        nvs_flash             # built-in
        esp_driver_i2s        # built-in — replaces legacy 'driver' in IDF 5.x; <driver/i2s_std.h>
        esp_lcd               # built-in — LCD panel ops
        esp_timer             # built-in — esp_timer used in ui_manager
        lvgl                  # registry: lvgl/lvgl
        esp-sr                # registry: espressif/esp-sr (hyphen, not underscore)
        mqtt                  # built-in — mqtt_client.h (not 'esp_mqtt')
        esp_websocket_client  # registry: espressif/esp_websocket_client
        freertos              # built-in
        log                   # built-in — esp_log.h
        # cJSON is a transitive dep via esp-sr → espressif/cjson; no direct REQUIRES needed
)
