#include "hal/board.h"
#include "device_state.h"
#include "display/ui_manager.h"
#include "audio/audio_pipeline.h"
#include "network/ws_client.h"
#include "network/mqtt_client.h"

#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

// esp_lcd_panel_io.h MUST be included here (outside extern "C") to avoid a
// conflicting-declaration error in IDF 5.5's esp_lcd_io_i2c.h: that header
// defines both uint32_t and i2c_master_bus_handle_t overloads of
// esp_lcd_new_panel_io_i2c. In C++ mode they are valid overloads; inside
// extern "C" they become C functions and the duplicate name conflicts.
// The include guard then blocks re-processing when ST77916.h pulls it in again.
#include <esp_lcd_panel_io.h>

// LCD + LVGL drivers (from reference project)
extern "C" {
    #include "ST77916.h"     // LCD init
    #include "LVGL_Driver.h" // lv_init, lv_disp_drv_register, tick task
    #include "PCF85063.h"    // RTC
    #include "BAT_Driver.h"  // battery ADC
    #include "PWR_Key.h"     // power key
    #include "CST816.h"      // touch
}

static const char* TAG = "main";

// ── Config (override via menuconfig) ─────────────────────────────────────────
#ifndef CONFIG_AI_ORB_WIFI_SSID
#define CONFIG_AI_ORB_WIFI_SSID     ""
#endif
#ifndef CONFIG_AI_ORB_WIFI_PASSWORD
#define CONFIG_AI_ORB_WIFI_PASSWORD ""
#endif
#ifndef CONFIG_AI_ORB_ORCHESTRATOR_HOST
#define CONFIG_AI_ORB_ORCHESTRATOR_HOST "192.168.1.10"
#endif
#ifndef CONFIG_AI_ORB_ORCHESTRATOR_WS_PORT
#define CONFIG_AI_ORB_ORCHESTRATOR_WS_PORT 8000
#endif
#ifndef CONFIG_AI_ORB_MQTT_HOST
#define CONFIG_AI_ORB_MQTT_HOST CONFIG_AI_ORB_ORCHESTRATOR_HOST
#endif
#ifndef CONFIG_AI_ORB_USER_ID
#define CONFIG_AI_ORB_USER_ID "kid"
#endif

// ── WiFi event group ──────────────────────────────────────────────────────────
static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static void wifi_event_handler(void*, esp_event_base_t base,
                                int32_t id, void*) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        UIManager::instance().on_wifi_changed(false, 0);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        UIManager::instance().on_wifi_changed(true, -50);
    }
}

static void wifi_init() {
    s_wifi_events = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_handler, nullptr, nullptr);
    wifi_config_t wcfg = {};
    strncpy((char*)wcfg.sta.ssid,     CONFIG_AI_ORB_WIFI_SSID,     31);
    strncpy((char*)wcfg.sta.password, CONFIG_AI_ORB_WIFI_PASSWORD, 63);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    esp_wifi_start();
    esp_wifi_connect();
    // Wait for connection (10s timeout)
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, false, true,
                        pdMS_TO_TICKS(10000));
}

// ── Touch callback (tap → wake) ───────────────────────────────────────────────
// Called when touch tap is detected (wire to touch polling task in Phase 5b)
__attribute__((unused))
static void touch_tap_cb() {
    AudioPipeline::instance().trigger_wake();
    OrbMqttClient::instance().publish_touch();
}

// ── Clock task (1Hz) ─────────────────────────────────────────────────────────
static void clock_task(void*) {
    while (true) {
        datetime_t rtc;
        PCF85063_Read_Time(&rtc);                // actual API (not PCF85063_Get_Time)
        UIManager::instance().on_clock_tick(rtc.hour, rtc.minute);

        // BAT_Get_Volts() returns float; convert LiPo 3.0V–4.2V range to 0–100%
        float volts = BAT_Get_Volts();
        int bat = (int)((volts - 3.0f) / 1.2f * 100.0f);
        if (bat < 0)   bat = 0;
        if (bat > 100) bat = 100;
        UIManager::instance().on_battery_level(bat);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ── app_main ──────────────────────────────────────────────────────────────────
extern "C" void app_main() {
    // NVS + event loop
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    // Hardware peripherals
    PWR_Init();
    BAT_Init();
    I2C_Init();
    PCF85063_Init();

    // LCD + LVGL init (from reference project drivers)
    LCD_Init();           // ST77916 QSPI init
    LVGL_Init();          // lv_init + display driver registration + tick task

    // UI init (inside LVGL lock)
    lv_obj_t* screen = lv_scr_act();
    UIManager::instance().init(screen);
    UIManager::instance().on_state_change(DEVICE_STATE_CONNECTING);

    // Touch: already initialised inside LCD_Init() → ST77916.c calls Touch_Init()
    // Calling it again here would double-install I2C port 1 and abort().

    // WiFi
    wifi_init();

    // Wire up audio → WebSocket → UI
    AudioPipeline& ap = AudioPipeline::instance();
    WSClient&      ws = WSClient::instance();

    ws.set_state_cb([](device_state_t s) {
        UIManager::instance().on_state_change(s);
    });
    ws.set_audio_cb([](const uint8_t* d, size_t l) {
        AudioPipeline::instance().play_pcm(d, l);
    });
    ws.set_text_cb([](const char* t) {
        UIManager::instance().on_chat_text(t);
    });

    ap.set_state_cb([](device_state_t s) {
        UIManager::instance().on_state_change(s);
        OrbMqttClient::instance().publish_state(s);
    });
    ap.set_rms_cb([](float rms) {
        UIManager::instance().on_audio_rms(rms);
    });
    ap.set_send_frame_cb([](const uint8_t* d, size_t l) {
        WSClient::instance().send_audio(d, l);
    });

    // Connect to orchestrator
    ws.connect(CONFIG_AI_ORB_ORCHESTRATOR_HOST, CONFIG_AI_ORB_ORCHESTRATOR_WS_PORT);
    // MQTT host: use dedicated broker if set, otherwise share orchestrator host
    const char* mqtt_host = (CONFIG_AI_ORB_MQTT_HOST[0] != '\0')
        ? CONFIG_AI_ORB_MQTT_HOST
        : CONFIG_AI_ORB_ORCHESTRATOR_HOST;
    OrbMqttClient::instance().connect(mqtt_host, 1883, CONFIG_AI_ORB_USER_ID);

    // Audio pipeline (I2S + wake word + VAD)
    ap.init();

    // Clock + battery task
    xTaskCreate(clock_task, "clock", 2048, nullptr, 2, nullptr);

    // LVGL task (core 0)
    xTaskCreatePinnedToCore(UIManager::lvgl_task, "lvgl", 8192,
                            &UIManager::instance(), 4, nullptr, 0);

    ESP_LOGI(TAG, "AI Orb started");
}
