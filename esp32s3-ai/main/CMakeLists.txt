idf_component_register(
    SRCS
        "main.cpp"
        "display/face_display.cpp"
        "display/ui_manager.cpp"
        "audio/audio_pipeline.cpp"
        "network/ws_client.cpp"
        "network/mqtt_client.cpp"
        # Hardware drivers copied from reference project
        "LCD_Driver/esp_lcd_st77916/esp_lcd_st77916.c"
        "LCD_Driver/ST77916.c"
        "Touch_Driver/esp_lcd_touch/esp_lcd_touch.c"
        "Touch_Driver/CST816.c"
        "LVGL_Driver/LVGL_Driver.c"
        "I2C_Driver/I2C_Driver.c"
        "PCF85063/PCF85063.c"
        "BAT_Driver/BAT_Driver.c"
        "PWR_Key/PWR_Key.c"
        "EXIO/TCA9554PWR.c"
    INCLUDE_DIRS
        "."
        "hal"
        "display"
        "audio"
        # network/ intentionally omitted: avoids shadowing IDF's mqtt_client.h
        # (main.cpp uses "network/ws_client.h" prefix; network/ files use same-dir includes)
        # Hardware driver include dirs
        "LCD_Driver/esp_lcd_st77916"
        "LCD_Driver"
        "Touch_Driver/esp_lcd_touch"
        "Touch_Driver"
        "LVGL_Driver"
        "I2C_Driver"
        "PCF85063"
        "BAT_Driver"
        "PWR_Key"
        "EXIO"
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
        esp_adc               # built-in — esp_adc/adc_oneshot.h used by BAT_Driver
        # cJSON is a transitive dep via esp-sr → espressif/cjson; no direct REQUIRES needed
)
