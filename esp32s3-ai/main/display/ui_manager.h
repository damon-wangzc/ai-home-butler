#pragma once

#include "lvgl.h"
#include "face_display.h"
#include "device_state.h"
#include <cstdint>

// ============================================================
// UIManager — owns all LVGL UI components and drives them
// from the device state machine.
//
// Components:
//   FaceDisplay  — animated face (center)
//   Status bar   — time (PCF85063), WiFi icon, battery
//   State label  — "Listening...", "Thinking..." below face
//   Chat label   — last response text, auto-scroll
// ============================================================

class UIManager {
public:
    static UIManager& instance() {
        static UIManager inst;
        return inst;
    }

    // Called once from main.cpp after LCD driver is ready
    void init(lv_obj_t* screen);

    // Called from device state machine on state transition
    void on_state_change(device_state_t new_state);

    // Called when orchestrator text arrives (chat response)
    void on_chat_text(const char* text);

    // Called from audio DMA callback at ~30fps
    void on_audio_rms(float rms);

    // Called from the LVGL tick task every 1 second (clock update)
    void on_clock_tick(int hour, int minute);

    // Called from WiFi event handler
    void on_wifi_changed(bool connected, int rssi_dbm);

    // Called from battery ADC read (0–100%)
    void on_battery_level(int percent);

    // LVGL task — call from a dedicated FreeRTOS task
    static void lvgl_task(void* arg);

private:
    UIManager() = default;

    FaceDisplay face_;

    // Status bar widgets
    lv_obj_t* status_bar_  = nullptr;
    lv_obj_t* time_label_  = nullptr;
    lv_obj_t* wifi_icon_   = nullptr;
    lv_obj_t* bat_label_   = nullptr;

    // State label (below face)
    lv_obj_t* state_label_ = nullptr;

    // Chat text label (bottom strip, auto-scroll)
    lv_obj_t* chat_label_  = nullptr;

    device_state_t current_state_ = DEVICE_STATE_UNKNOWN;

    void create_status_bar(lv_obj_t* screen);
    void create_bottom_labels(lv_obj_t* screen);

    static const char* state_text(device_state_t s);
    static lv_color_t  state_text_color(device_state_t s);
};
