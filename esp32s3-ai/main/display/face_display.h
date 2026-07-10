#pragma once

#include "lvgl.h"
#include "device_state.h"
#include <algorithm>

// ============================================================
// FaceDisplay — StackChan-inspired LVGL face for a 360x360
// round display.  All coordinates are in display pixels.
//
// Face layout (centered on 360x360):
//   Eye left  : 60x60 white circle at (105, 115)
//   Eye right : 60x60 white circle at (195, 115)
//   Mouth arc : 80-wide lv_arc   at center (180, 210)
//   Blush L/R : 36x20 pink ovals at (92,175) / (232,175)
//   Eyelids   : animated height-0-to-60 overlays for blink
// ============================================================

class FaceDisplay {
public:
    FaceDisplay() = default;
    ~FaceDisplay() = default;

    // Call once with the parent LVGL screen object
    void init(lv_obj_t* screen);

    // Change expression; drives all LVGL animations
    void set_state(device_state_t state);

    // Drive mouth open angle from PCM RMS (0.0 = silent, 1.0 = peak)
    // Call at ~30fps from the audio DMA callback
    void set_mouth_rms(float rms);

    // Update the accent ring colour (called by UIManager when state changes)
    void set_accent_color(lv_color_t color);

    // Periodic tick — call from LVGL task every 100ms for autonomous animations
    void tick_100ms();

private:
    // Eye components
    lv_obj_t* eye_left_bg_   = nullptr;
    lv_obj_t* pupil_left_    = nullptr;
    lv_obj_t* lid_left_      = nullptr;

    lv_obj_t* eye_right_bg_  = nullptr;
    lv_obj_t* pupil_right_   = nullptr;
    lv_obj_t* lid_right_     = nullptr;

    // Mouth
    lv_obj_t* mouth_arc_     = nullptr;

    // Blush cheeks
    lv_obj_t* blush_left_    = nullptr;
    lv_obj_t* blush_right_   = nullptr;

    // Accent ring (thin arc spanning the whole screen edge)
    lv_obj_t* accent_ring_   = nullptr;

    device_state_t current_state_ = DEVICE_STATE_UNKNOWN;

    // Blink timer
    lv_timer_t* blink_timer_      = nullptr;
    uint32_t    blink_interval_ms_ = 3500;

    // Helpers
    void create_eye(lv_obj_t* parent, lv_coord_t x, lv_coord_t y,
                    lv_obj_t** bg, lv_obj_t** pupil, lv_obj_t** lid);
    void create_mouth(lv_obj_t* parent);
    void create_blush(lv_obj_t* parent);
    void create_accent_ring(lv_obj_t* parent);

    void apply_idle_expression();
    void apply_listening_expression();
    void apply_thinking_expression();
    void apply_speaking_expression();
    void apply_error_expression();
    void apply_connecting_expression();

    void trigger_blink();
    void stop_pupil_animation();
    void start_pupil_scan();   // THINKING: eyes drift left/right

    static void blink_timer_cb(lv_timer_t* timer);
};
