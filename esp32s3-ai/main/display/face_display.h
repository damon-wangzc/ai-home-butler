#pragma once

#include "lvgl.h"
#include "device_state.h"
#include "stackchan_assets.h"
#include <cstdint>

// ============================================================
// FaceDisplay — StackChan bitmap-driven face for the ST77916
// 360×360 round display.
//
// Rendering pipeline:
//   Each eye = overflow-hidden clip container
//              + lv_img (alpha-8 eye bitmap, recoloured white)
//              + dark pupil oval (follows touch point)
//              + black eyelid overlay (animated for blink)
//
//   Mouth uses:
//     lv_img  (alpha-8 mouth bitmap) for IDLE / CONNECTING
//     lv_arc  (circle / arc)          for LISTENING / THINKING /
//                                          SPEAKING / ERROR
//
// Touch tracking:
//   Call on_touch(tx, ty) whenever the CST816 reports a contact.
//   Pupils smoothly follow the touch position and decay back to
//   the preset centre over ~1.5 s after the finger lifts.
// ============================================================

// Display geometry
#define FACE_CX    180          // centre X of the 360×360 display
#define FACE_CY    180          // centre Y
#define FACE_R     176          // inner radius for the accent ring arc

// Maximum pupil travel due to touch (pixels from eye centre)
#define TOUCH_PUPIL_X  12
#define TOUCH_PUPIL_Y   8

// Ticks (each tick = 100 ms) before touch decay begins
#define TOUCH_HOLD_TICKS  5

struct EyeWidgets {
    lv_obj_t* clip  = nullptr;   // overflow-hidden container sized to eye_w×eye_h
    lv_obj_t* img   = nullptr;   // lv_img: alpha-8 eye bitmap, recoloured white
    lv_obj_t* pupil = nullptr;   // dark rounded oval
    lv_obj_t* lid   = nullptr;   // black top-overlay, animated for blink
};

class FaceDisplay {
public:
    FaceDisplay()  = default;
    ~FaceDisplay() = default;

    // Call once after lv_init and LCD driver registration
    void init(lv_obj_t* screen);

    // Drive state machine (expression + accent ring colour)
    void set_state(device_state_t state);

    // Drive mouth open/close from audio RMS (0.0 = silent, 1.0 = peak)
    // Only effective in SPEAKING state; call at ~30 fps
    void set_mouth_rms(float rms);

    // Override accent ring colour from UIManager
    void set_accent_color(lv_color_t c);

    // Feed raw touch coordinates (0..359) from the CST816 driver.
    // Call from the LVGL task each time esp_lcd_touch_get_xy returns true.
    void on_touch(int16_t tx, int16_t ty);

    // Periodic housekeeping — call every 100 ms from the LVGL task
    void tick_100ms();

private:
    EyeWidgets  eye_l_, eye_r_;

    lv_obj_t*  mouth_img_   = nullptr;  // bitmap smile (IDLE / CONNECTING)
    lv_obj_t*  mouth_arc_   = nullptr;  // arc (LISTENING, THINKING, SPEAKING, ERROR)

    lv_obj_t*  blush_l_     = nullptr;
    lv_obj_t*  blush_r_     = nullptr;
    lv_obj_t*  accent_ring_ = nullptr;

    device_state_t  current_state_  = DEVICE_STATE_UNKNOWN;

    // Touch tracking state
    float   touch_ox_     = 0.0f;   // current pupil X offset (px, ±TOUCH_PUPIL_X)
    float   touch_oy_     = 0.0f;   // current pupil Y offset (px, ±TOUCH_PUPIL_Y)
    int32_t touch_ticks_  = 0;      // ticks since last on_touch() call

    // Blink timer
    lv_timer_t* blink_timer_ = nullptr;

    // ── Builders (called once from init) ─────────────────────────────
    void build_eye(lv_obj_t* parent, EyeWidgets& w);
    void build_mouth(lv_obj_t* parent);
    void build_blush(lv_obj_t* parent);
    void build_accent_ring(lv_obj_t* parent);

    // ── Per-state update helpers ──────────────────────────────────────
    // Resize/reposition a single eye according to the given preset.
    void apply_eye_preset(EyeWidgets& w, bool is_left, const face_preset_t& p);

    // Update both pupils with combined static + touch offset.
    void refresh_pupil_positions(const face_preset_t& p);

    // Configure the mouth widget(s) for the given state.
    void apply_mouth_preset(device_state_t s, const face_preset_t& p);

    // Set blush opacity for the given state.
    void apply_blush_opacity(device_state_t s);

    // ── Animations ───────────────────────────────────────────────────
    void trigger_blink();
    void stop_pupil_animations();     // kills THINKING scan anims
    void start_thinking_scan();       // THINKING: pupils drift left↔right

    // ── Static callbacks ─────────────────────────────────────────────
    static void blink_timer_cb(lv_timer_t* t);
    static void anim_height_cb(void* obj, int32_t v);
    static void anim_x_cb(void* obj, int32_t v);
};
