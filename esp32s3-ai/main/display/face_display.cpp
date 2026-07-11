#include "face_display.h"
#include "board.h"
#include <esp_log.h>
#include <cstdlib>
#include <algorithm>
#include <cmath>

static const char* TAG = "FaceDisplay";

// ── Palette ──────────────────────────────────────────────────────────────────
static const lv_color_t COLOR_BG     = LV_COLOR_MAKE(0x00, 0x00, 0x00);
static const lv_color_t COLOR_SCLERA = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF);
static const lv_color_t COLOR_PUPIL  = LV_COLOR_MAKE(0x18, 0x18, 0x2C);
static const lv_color_t COLOR_BLUSH  = LV_COLOR_MAKE(0xFF, 0x88, 0xAA);
static const lv_color_t COLOR_MOUTH  = LV_COLOR_MAKE(0xEE, 0x44, 0x44);

// ── Mouth arc rotation offset so that 0° is at the bottom of the arc widget ─
// LVGL arc: 0° = 3 o'clock, angles go clockwise.
// We want our "bottom-centre" to be at 270° in LVGL convention.
// Presets express mouth angles relative to a bottom-origin convention:
//   smile  → arc_start = 200, arc_end = 340  (140° span centred at 270°)
//   frown  → arc_start =  20, arc_end = 160  (140° span centred at  90°)
//   "O"    → arc_start =   0, arc_end = 355
//   flat   → arc_start = 255, arc_end = 285  ( 30° thin slice at bottom)
// ─────────────────────────────────────────────────────────────────────────────

// ── Animation callbacks ───────────────────────────────────────────────────────
void FaceDisplay::anim_height_cb(void* obj, int32_t v) {
    lv_obj_set_height((lv_obj_t*)obj, (lv_coord_t)v);
}
void FaceDisplay::anim_x_cb(void* obj, int32_t v) {
    lv_obj_set_x((lv_obj_t*)obj, (lv_coord_t)v);
}

// ── Public API ────────────────────────────────────────────────────────────────

void FaceDisplay::init(lv_obj_t* screen) {
    // Transparent face container spanning the full display
    lv_obj_t* face = lv_obj_create(screen);
    lv_obj_set_size(face, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(face, 0, 0);
    lv_obj_set_style_bg_opa(face, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(face, 0, LV_PART_MAIN);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_SCROLLABLE);

    build_eye(face, eye_l_);
    build_eye(face, eye_r_);
    build_mouth(face);
    build_blush(face);
    build_accent_ring(screen);   // on top of everything, on the screen itself

    blink_timer_ = lv_timer_create(blink_timer_cb, 3500, this);

    set_state(DEVICE_STATE_IDLE);
    ESP_LOGI(TAG, "init done");
}

void FaceDisplay::set_state(device_state_t state) {
    if (state == current_state_) return;
    current_state_ = state;
    stop_pupil_animations();

    const face_preset_t& p = g_face_presets[(int)state];

    apply_eye_preset(eye_l_, true,  p);
    apply_eye_preset(eye_r_, false, p);
    refresh_pupil_positions(p);
    apply_mouth_preset(state, p);
    apply_blush_opacity(state);

    // Accent ring
    lv_color_t ac = lv_color_hex(p.accent_color);
    set_accent_color(ac);
    lv_obj_set_style_arc_width(accent_ring_, p.ring_thickness, LV_PART_INDICATOR);

    // Blink timer: run only in IDLE / SPEAKING
    if (state == DEVICE_STATE_IDLE || state == DEVICE_STATE_SPEAKING) {
        lv_timer_resume(blink_timer_);
    } else {
        lv_timer_pause(blink_timer_);
    }

    // THINKING: pupils scan left-right autonomously
    if (state == DEVICE_STATE_THINKING) {
        start_thinking_scan();
    }
}

void FaceDisplay::set_mouth_rms(float rms) {
    if (current_state_ != DEVICE_STATE_SPEAKING) return;
    rms = std::max(0.0f, std::min(1.0f, rms));
    const face_preset_t& p = g_face_presets[(int)DEVICE_STATE_SPEAKING];
    // Map RMS 0..1 → mouth arc span 60°..180° centred at bottom (270°)
    int span  = 60 + (int)(rms * 120.0f);
    int start = 270 - span / 2;
    int end   = 270 + span / 2;
    lv_arc_set_angles(mouth_arc_, (uint16_t)start, (uint16_t)end);
    lv_obj_set_style_arc_width(mouth_arc_, p.mouth_height / 2, LV_PART_INDICATOR);
}

void FaceDisplay::set_accent_color(lv_color_t c) {
    lv_obj_set_style_arc_color(accent_ring_, c, LV_PART_INDICATOR);
    lv_arc_set_value(accent_ring_, 100);
}

void FaceDisplay::on_touch(int16_t tx, int16_t ty) {
    // Map raw display coordinates to normalised ±1 range
    float dx = (float)(tx - FACE_CX) / (float)FACE_CX;
    float dy = (float)(ty - FACE_CY) / (float)FACE_CY;
    if (dx >  1.0f) dx =  1.0f;
    if (dx < -1.0f) dx = -1.0f;
    if (dy >  1.0f) dy =  1.0f;
    if (dy < -1.0f) dy = -1.0f;

    touch_ox_ = dx * (float)TOUCH_PUPIL_X;
    touch_oy_ = dy * (float)TOUCH_PUPIL_Y;
    touch_ticks_ = 0;   // reset decay counter

    // Immediately update pupils (except THINKING which has its own scan anim)
    if (current_state_ != DEVICE_STATE_THINKING) {
        refresh_pupil_positions(g_face_presets[(int)current_state_]);
    }
}

void FaceDisplay::tick_100ms() {
    // Randomise blink interval for naturalistic blinking
    if (current_state_ == DEVICE_STATE_IDLE) {
        uint32_t jitter = (uint32_t)(rand() % 1200);
        lv_timer_set_period(blink_timer_, 2800 + jitter);
    }

    // Touch decay: after TOUCH_HOLD_TICKS ticks without contact, pupils drift
    // back to centre with a ~50% per-tick decay.
    touch_ticks_++;
    if (touch_ticks_ > TOUCH_HOLD_TICKS) {
        touch_ox_ *= 0.55f;
        touch_oy_ *= 0.55f;
        // Snap to zero below 0.5 px to avoid infinite approach
        if (fabsf(touch_ox_) < 0.5f) touch_ox_ = 0.0f;
        if (fabsf(touch_oy_) < 0.5f) touch_oy_ = 0.0f;

        if (current_state_ != DEVICE_STATE_THINKING) {
            refresh_pupil_positions(g_face_presets[(int)current_state_]);
        }
    }
}

// ── Widget builders ───────────────────────────────────────────────────────────

void FaceDisplay::build_eye(lv_obj_t* parent, EyeWidgets& w) {
    // Clip container — children outside its bounds are hidden
    w.clip = lv_obj_create(parent);
    lv_obj_add_flag(w.clip, LV_OBJ_FLAG_OVERFLOW_HIDDEN);
    lv_obj_set_style_bg_opa(w.clip, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(w.clip, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(w.clip, 0, LV_PART_MAIN);
    lv_obj_clear_flag(w.clip, LV_OBJ_FLAG_SCROLLABLE);

    // Eye bitmap image (alpha-8 mask, recoloured white = sclera)
    w.img = lv_img_create(w.clip);
    lv_img_set_src(w.img, &g_eye_bitmap);
    lv_obj_set_style_img_recolor(w.img, COLOR_SCLERA, LV_PART_MAIN);
    lv_obj_set_style_img_recolor_opa(w.img, LV_OPA_COVER, LV_PART_MAIN);
    // Pivot at image centre so zoom scales symmetrically
    lv_img_set_pivot(w.img, STACKCHAN_EYE_W / 2, STACKCHAN_EYE_H / 2);
    lv_img_set_zoom(w.img, 256);

    // Pupil — dark oval, positioned inside clip
    w.pupil = lv_obj_create(w.clip);
    lv_obj_set_style_radius(w.pupil, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(w.pupil, COLOR_PUPIL, LV_PART_MAIN);
    lv_obj_set_style_border_width(w.pupil, 0, LV_PART_MAIN);
    lv_obj_clear_flag(w.pupil, LV_OBJ_FLAG_SCROLLABLE);

    // Eyelid — black overlay at top of clip, height 0 when open
    w.lid = lv_obj_create(w.clip);
    lv_obj_set_style_bg_color(w.lid, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(w.lid, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(w.lid, 0, LV_PART_MAIN);
    lv_obj_set_size(w.lid, 200, 0);    // wide but zero height = invisible
    lv_obj_set_pos(w.lid, -70, 0);     // spans across clip top
    lv_obj_move_foreground(w.lid);     // always on top
}

void FaceDisplay::build_mouth(lv_obj_t* parent) {
    // Bitmap mouth image (used for IDLE and CONNECTING)
    mouth_img_ = lv_img_create(parent);
    lv_img_set_src(mouth_img_, &g_mouth_bitmap);
    lv_obj_set_style_img_recolor(mouth_img_, COLOR_MOUTH, LV_PART_MAIN);
    lv_obj_set_style_img_recolor_opa(mouth_img_, LV_OPA_COVER, LV_PART_MAIN);
    lv_img_set_pivot(mouth_img_, STACKCHAN_MOUTH_W / 2, STACKCHAN_MOUTH_H / 2);
    lv_img_set_zoom(mouth_img_, 256);
    lv_obj_add_flag(mouth_img_, LV_OBJ_FLAG_HIDDEN);

    // Arc mouth (LISTENING circle, THINKING flat, SPEAKING animated, ERROR frown)
    mouth_arc_ = lv_arc_create(parent);
    lv_arc_set_mode(mouth_arc_, LV_ARC_MODE_NORMAL);
    lv_arc_set_range(mouth_arc_, 0, 100);
    lv_arc_set_value(mouth_arc_, 100);
    lv_arc_set_bg_angles(mouth_arc_, 0, 360);
    lv_obj_set_style_arc_color(mouth_arc_, COLOR_MOUTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(mouth_arc_, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(mouth_arc_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_opa(mouth_arc_, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_add_flag(mouth_arc_, LV_OBJ_FLAG_HIDDEN);
}

void FaceDisplay::build_blush(lv_obj_t* parent) {
    auto mk_blush = [&](lv_obj_t* p, lv_coord_t x, lv_coord_t y) -> lv_obj_t* {
        lv_obj_t* b = lv_obj_create(p);
        lv_obj_set_size(b, 36, 18);
        lv_obj_set_pos(b, x, y);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, COLOR_BLUSH, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
        return b;
    };
    blush_l_ = mk_blush(parent,  86, 177);
    blush_r_ = mk_blush(parent, 238, 177);
}

void FaceDisplay::build_accent_ring(lv_obj_t* parent) {
    accent_ring_ = lv_arc_create(parent);
    lv_obj_set_size(accent_ring_, LCD_WIDTH - 4, LCD_HEIGHT - 4);
    lv_obj_align(accent_ring_, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_bg_angles(accent_ring_, 0, 360);
    lv_arc_set_range(accent_ring_, 0, 100);
    lv_arc_set_value(accent_ring_, 100);
    lv_obj_set_style_arc_width(accent_ring_, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(accent_ring_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_opa(accent_ring_, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_arc_color(accent_ring_, lv_color_hex(0x4488FF), LV_PART_INDICATOR);
}

// ── Per-state update helpers ──────────────────────────────────────────────────

void FaceDisplay::apply_eye_preset(EyeWidgets& w, bool is_left, const face_preset_t& p) {
    // Compute absolute eye centre
    int16_t half_sp = p.eye_spacing / 2;
    int16_t cx = (int16_t)(FACE_CX + (is_left ? -half_sp : half_sp));
    int16_t cy = (int16_t)(FACE_CY + p.eye_y_offset);

    // Clip container: exactly the desired visible eye size
    int16_t clip_w = p.eye_width;
    int16_t clip_h = p.eye_height;
    lv_obj_set_size(w.clip, clip_w, clip_h);
    lv_obj_set_pos(w.clip, cx - clip_w / 2, cy - clip_h / 2);

    // Scale the eye bitmap so its width matches eye_width.
    // The height scales proportionally; clipping handles any height overshoot.
    //   zoom = (eye_width / STACKCHAN_EYE_W) * 256  (fixed-point)
    uint16_t zoom = (uint16_t)((int32_t)p.eye_width * 256 / STACKCHAN_EYE_W);
    if (zoom < 64)  zoom = 64;
    if (zoom > 512) zoom = 512;
    lv_img_set_zoom(w.img, zoom);

    // Place the bitmap so it is centred inside the clip.
    // Scaled dimensions of the rendered image:
    int16_t img_w = (int16_t)((int32_t)STACKCHAN_EYE_W * zoom / 256);
    int16_t img_h = (int16_t)((int32_t)STACKCHAN_EYE_H * zoom / 256);
    lv_obj_set_pos(w.img, (clip_w - img_w) / 2, (clip_h - img_h) / 2);

    // Size lid to match clip width (height stays 0 while open)
    lv_obj_set_width(w.lid, clip_w + 4);

    // Pupil initial placement (touch offsets applied separately)
    lv_obj_set_size(w.pupil, p.pupil_width, p.pupil_height);
}

void FaceDisplay::refresh_pupil_positions(const face_preset_t& p) {
    if (!eye_l_.pupil || !eye_r_.pupil) return;

    // Static per-state offset: for ERROR the left pupil offsets left/down,
    // right pupil mirrors (looks inward / angry)
    auto place = [&](EyeWidgets& w, bool is_left) {
        int16_t clip_w = p.eye_width;
        int16_t clip_h = p.eye_height;
        int16_t static_ox = is_left ? p.pupil_offset_x : -(int16_t)p.pupil_offset_x;
        int16_t static_oy = p.pupil_offset_y;

        // Touch offsets: both eyes look toward the same touch point
        int16_t ox = static_ox + (int16_t)touch_ox_;
        int16_t oy = static_oy + (int16_t)touch_oy_;

        // Clamp so pupil stays within clip
        int16_t half_rng_x = (clip_w - p.pupil_width)  / 2;
        int16_t half_rng_y = (clip_h - p.pupil_height) / 2;
        if (half_rng_x < 0) half_rng_x = 0;
        if (half_rng_y < 0) half_rng_y = 0;
        if (ox >  half_rng_x) ox =  half_rng_x;
        if (ox < -half_rng_x) ox = -half_rng_x;
        if (oy >  half_rng_y) oy =  half_rng_y;
        if (oy < -half_rng_y) oy = -half_rng_y;

        lv_obj_set_pos(w.pupil,
            clip_w / 2 - p.pupil_width  / 2 + ox,
            clip_h / 2 - p.pupil_height / 2 + oy);
    };

    place(eye_l_, true);
    place(eye_r_, false);
}

void FaceDisplay::apply_mouth_preset(device_state_t s, const face_preset_t& p) {
    // Compute mouth centre in display coords
    int16_t mouth_cx = FACE_CX;
    int16_t mouth_cy = (int16_t)(FACE_CY + p.mouth_y_offset);

    bool use_bitmap = (s == DEVICE_STATE_IDLE     ||
                       s == DEVICE_STATE_UNKNOWN  ||
                       s == DEVICE_STATE_CONNECTING);

    if (use_bitmap) {
        lv_obj_clear_flag(mouth_img_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mouth_arc_,   LV_OBJ_FLAG_HIDDEN);

        // Scale bitmap to match mouth_width
        uint16_t zoom = (uint16_t)((int32_t)p.mouth_width * 256 / STACKCHAN_MOUTH_W);
        if (zoom < 64)  zoom = 64;
        if (zoom > 512) zoom = 512;
        lv_img_set_zoom(mouth_img_, zoom);
        lv_img_set_angle(mouth_img_, 0);   // ensure no leftover rotation

        int16_t img_w = (int16_t)((int32_t)STACKCHAN_MOUTH_W * zoom / 256);
        int16_t img_h = (int16_t)((int32_t)STACKCHAN_MOUTH_H * zoom / 256);
        lv_obj_set_pos(mouth_img_, mouth_cx - img_w / 2, mouth_cy - img_h / 2);

    } else {
        lv_obj_add_flag(mouth_img_,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(mouth_arc_, LV_OBJ_FLAG_HIDDEN);

        // Arc size based on mouth_width × mouth_width (square bounding box)
        int16_t arc_sz = p.mouth_width;
        lv_obj_set_size(mouth_arc_, arc_sz, arc_sz);
        lv_obj_set_pos(mouth_arc_, mouth_cx - arc_sz / 2, mouth_cy - arc_sz / 2);
        lv_obj_set_style_arc_width(mouth_arc_, p.mouth_height, LV_PART_INDICATOR);

        // Configure arc angles per state
        switch (s) {
            case DEVICE_STATE_LISTENING:
                // "O" mouth — full circle
                lv_arc_set_angles(mouth_arc_, 0, 355);
                lv_obj_set_style_arc_color(mouth_arc_, COLOR_SCLERA, LV_PART_INDICATOR);
                break;

            case DEVICE_STATE_THINKING:
                // Flat closed mouth — tiny arc slice at bottom
                lv_arc_set_angles(mouth_arc_, 255, 285);
                lv_obj_set_style_arc_color(mouth_arc_, COLOR_MOUTH, LV_PART_INDICATOR);
                break;

            case DEVICE_STATE_SPEAKING:
                // Animated open smile — starting angle; set_mouth_rms() updates it
                lv_arc_set_angles(mouth_arc_, 220, 320);
                lv_obj_set_style_arc_color(mouth_arc_, COLOR_MOUTH, LV_PART_INDICATOR);
                break;

            case DEVICE_STATE_ERROR:
                // Frown — inverted smile arc
                lv_arc_set_angles(mouth_arc_, 20, 160);
                lv_obj_set_style_arc_color(mouth_arc_,
                    lv_color_hex(0x9944FF), LV_PART_INDICATOR);
                break;

            default:
                lv_arc_set_angles(mouth_arc_, 200, 340);
                lv_obj_set_style_arc_color(mouth_arc_, COLOR_MOUTH, LV_PART_INDICATOR);
                break;
        }
    }
}

void FaceDisplay::apply_blush_opacity(device_state_t s) {
    lv_opa_t opa_l, opa_r;
    switch (s) {
        case DEVICE_STATE_IDLE:
        case DEVICE_STATE_SPEAKING:
            opa_l = opa_r = LV_OPA_40; break;
        case DEVICE_STATE_LISTENING:
            opa_l = opa_r = LV_OPA_50; break;
        case DEVICE_STATE_THINKING:
            opa_l = opa_r = LV_OPA_20; break;
        default:
            opa_l = opa_r = LV_OPA_TRANSP; break;
    }
    lv_obj_set_style_bg_opa(blush_l_, opa_l, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(blush_r_, opa_r, LV_PART_MAIN);
}

// ── Animations ────────────────────────────────────────────────────────────────

void FaceDisplay::trigger_blink() {
    for (EyeWidgets* ew : {&eye_l_, &eye_r_}) {
        int16_t eye_h = (int16_t)lv_obj_get_height(ew->clip);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ew->lid);
        lv_anim_set_exec_cb(&a, anim_height_cb);
        lv_anim_set_values(&a, 0, eye_h);
        lv_anim_set_time(&a, 80);
        lv_anim_set_playback_time(&a, 80);
        lv_anim_set_repeat_count(&a, 1);
        lv_anim_start(&a);
    }
}

void FaceDisplay::stop_pupil_animations() {
    if (eye_l_.pupil) lv_anim_del(eye_l_.pupil, anim_x_cb);
    if (eye_r_.pupil) lv_anim_del(eye_r_.pupil, anim_x_cb);
}

void FaceDisplay::start_thinking_scan() {
    const face_preset_t& p = g_face_presets[(int)DEVICE_STATE_THINKING];
    int16_t clip_w = p.eye_width;   // 55
    int16_t base_x = clip_w / 2 - p.pupil_width / 2;  // centre position X in clip

    for (EyeWidgets* ew : {&eye_l_, &eye_r_}) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ew->pupil);
        lv_anim_set_exec_cb(&a, anim_x_cb);
        lv_anim_set_values(&a, base_x - 10, base_x + 10);
        lv_anim_set_time(&a, 850);
        lv_anim_set_playback_time(&a, 850);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }
}

// ── Static callbacks ──────────────────────────────────────────────────────────

void FaceDisplay::blink_timer_cb(lv_timer_t* timer) {
    FaceDisplay* self = (FaceDisplay*)timer->user_data;
    if (self) self->trigger_blink();
}
