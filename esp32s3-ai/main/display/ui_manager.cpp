#include "ui_manager.h"
#include "board.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdio>
#include <cstring>

// esp_lcd_panel_io.h MUST be included here (outside extern "C") to avoid the
// conflicting-declaration error in IDF 5.5's esp_lcd_io_i2c.h.
// CST816.h → ST77916.h pulls it in; pre-including outside the C-linkage block
// lets the include guard suppress the re-entry, same pattern as main.cpp.
#include <esp_lcd_panel_io.h>

extern "C" {
#include "esp_lcd_touch.h"  // esp_lcd_touch_read_data(), esp_lcd_touch_get_coordinates()
#include "CST816.h"         // declares: esp_lcd_touch_handle_t tp
}

static const char* TAG = "UIManager";

// ── Init ──────────────────────────────────────────────────────────────────────

void UIManager::init(lv_obj_t* screen) {
    // Black background filling the round display
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    face_.init(screen);
    create_status_bar(screen);
    create_bottom_labels(screen);

    ESP_LOGI(TAG, "UI initialised");
}

// ── State change ──────────────────────────────────────────────────────────────

void UIManager::on_state_change(device_state_t new_state) {
    current_state_ = new_state;
    face_.set_state(new_state);

    lv_label_set_text(state_label_, state_text(new_state));
    lv_obj_set_style_text_color(state_label_, state_text_color(new_state), LV_PART_MAIN);
}

// ── Chat text ─────────────────────────────────────────────────────────────────

void UIManager::on_chat_text(const char* text) {
    if (!text) return;
    // Trim to 120 chars to fit the label
    char buf[121];
    strncpy(buf, text, 120);
    buf[120] = '\0';
    lv_label_set_text(chat_label_, buf);
    // Scroll to end so newest text is visible
    lv_obj_scroll_to_y(chat_label_, LV_COORD_MAX, LV_ANIM_ON);
}

// ── Audio RMS → mouth sync ────────────────────────────────────────────────────

void UIManager::on_audio_rms(float rms) {
    face_.set_mouth_rms(rms);
}

// ── Clock tick ────────────────────────────────────────────────────────────────

void UIManager::on_clock_tick(int hour, int minute) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
    lv_label_set_text(time_label_, buf);
}

// ── WiFi / battery ───────────────────────────────────────────────────────────

void UIManager::on_wifi_changed(bool connected, int rssi_dbm) {
    if (connected) {
        // Simple bar: 3 bars if rssi > -60, 2 if > -75, 1 otherwise
        if (rssi_dbm > -60)      lv_label_set_text(wifi_icon_, LV_SYMBOL_WIFI);
        else if (rssi_dbm > -75) lv_label_set_text(wifi_icon_, LV_SYMBOL_WIFI);
        else                     lv_label_set_text(wifi_icon_, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(wifi_icon_, lv_color_white(), LV_PART_MAIN);
    } else {
        lv_label_set_text(wifi_icon_, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(wifi_icon_, lv_color_hex(0xFF4444), LV_PART_MAIN);
    }
}

void UIManager::on_battery_level(int percent) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", percent);
    lv_label_set_text(bat_label_, buf);
}

void UIManager::on_touch(int16_t tx, int16_t ty) {
    face_.on_touch(tx, ty);
}

// ── LVGL task ─────────────────────────────────────────────────────────────────

void UIManager::lvgl_task(void* arg) {
    UIManager* self = static_cast<UIManager*>(arg);

    uint32_t tick_ms    = 0;
    uint32_t touch_poll = 0;          // poll touch every ~16 ms (~60 Hz)

    while (true) {
        lv_task_handler();
        vTaskDelay(pdMS_TO_TICKS(LVGL_REFRESH_MS));
        tick_ms    += LVGL_REFRESH_MS;
        touch_poll += LVGL_REFRESH_MS;

        // Poll CST816 touch controller
        if (touch_poll >= 16 && tp != NULL) {
            touch_poll = 0;
            esp_lcd_touch_read_data(tp);
            uint16_t tx = 0, ty = 0;
            uint8_t  point_num = 0;
            if (esp_lcd_touch_get_coordinates(tp, &tx, &ty, nullptr, &point_num, 1)
                    && point_num > 0) {
                self->on_touch((int16_t)tx, (int16_t)ty);
            }
        }

        if (tick_ms >= 100) {
            self->face_.tick_100ms();
            tick_ms = 0;
        }
    }
}

// ── Private helpers ───────────────────────────────────────────────────────────

void UIManager::create_status_bar(lv_obj_t* screen) {
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, 200, 30);
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(status_bar_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(status_bar_, 0, LV_PART_MAIN);
    lv_obj_clear_flag(status_bar_, LV_OBJ_FLAG_SCROLLABLE);

    // Time label (centre)
    time_label_ = lv_label_create(status_bar_);
    lv_label_set_text(time_label_, "00:00");
    lv_obj_align(time_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(time_label_, lv_color_white(), LV_PART_MAIN);

    // WiFi icon (left of time)
    wifi_icon_ = lv_label_create(status_bar_);
    lv_label_set_text(wifi_icon_, LV_SYMBOL_WIFI);
    lv_obj_align(wifi_icon_, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_text_color(wifi_icon_, lv_color_hex(0x888888), LV_PART_MAIN);

    // Battery label (right)
    bat_label_ = lv_label_create(status_bar_);
    lv_label_set_text(bat_label_, "--%%");
    lv_obj_align(bat_label_, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_text_color(bat_label_, lv_color_white(), LV_PART_MAIN);
}

void UIManager::create_bottom_labels(lv_obj_t* screen) {
    // State label — e.g. "Listening..." just below face
    state_label_ = lv_label_create(screen);
    lv_label_set_text(state_label_, "");
    lv_obj_align(state_label_, LV_ALIGN_BOTTOM_MID, 0, -52);
    lv_obj_set_style_text_color(state_label_, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_width(state_label_, 200);
    lv_label_set_long_mode(state_label_, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(state_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // Chat label — last response text, 2 lines, scrollable
    chat_label_ = lv_label_create(screen);
    lv_label_set_text(chat_label_, "");
    lv_obj_set_size(chat_label_, 260, 36);
    lv_obj_align(chat_label_, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_text_color(chat_label_, lv_color_hex(0xCCCCCC), LV_PART_MAIN);
    lv_label_set_long_mode(chat_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

const char* UIManager::state_text(device_state_t s) {
    switch (s) {
        case DEVICE_STATE_CONNECTING: return "Connecting...";
        case DEVICE_STATE_IDLE:       return "";
        case DEVICE_STATE_LISTENING:  return "Listening...";
        case DEVICE_STATE_THINKING:   return "Thinking...";
        case DEVICE_STATE_SPEAKING:   return "Speaking";
        case DEVICE_STATE_ERROR:      return "Connection lost";
        default:                      return "";
    }
}

lv_color_t UIManager::state_text_color(device_state_t s) {
    switch (s) {
        case DEVICE_STATE_LISTENING: return lv_color_hex(0x44FF88);
        case DEVICE_STATE_THINKING:  return lv_color_hex(0xFFAA00);
        case DEVICE_STATE_SPEAKING:  return lv_color_hex(0x00CCFF);
        case DEVICE_STATE_ERROR:     return lv_color_hex(0xFF4444);
        default:                     return lv_color_white();
    }
}
