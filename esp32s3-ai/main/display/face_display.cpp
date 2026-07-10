#include "face_display.h"
#include "board.h"
#include <esp_log.h>
#include <cstdlib>   // rand()

static const char* TAG = "FaceDisplay";

// ── Palette ──────────────────────────────────────────────────────────────────
static const lv_color_t COLOR_BG        = LV_COLOR_MAKE(0x00, 0x00, 0x00);
static const lv_color_t COLOR_SCLERA    = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF);
static const lv_color_t COLOR_PUPIL     = LV_COLOR_MAKE(0x22, 0x22, 0x22);
static const lv_color_t COLOR_BLUSH     = LV_COLOR_MAKE(0xFF, 0x88, 0xAA);
static const lv_color_t COLOR_MOUTH     = LV_COLOR_MAKE(0xEE, 0x44, 0x44);

static const lv_color_t ACCENT_IDLE     = LV_COLOR_MAKE(0x44, 0x88, 0xFF);
static const lv_color_t ACCENT_LISTEN   = LV_COLOR_MAKE(0x44, 0xFF, 0x88);
static const lv_color_t ACCENT_THINK    = LV_COLOR_MAKE(0xFF, 0xAA, 0x00);
static const lv_color_t ACCENT_SPEAK    = LV_COLOR_MAKE(0x00, 0xCC, 0xFF);
static const lv_color_t ACCENT_ERROR    = LV_COLOR_MAKE(0xFF, 0x44, 0x44);
static const lv_color_t ACCENT_CONNECT  = LV_COLOR_MAKE(0x88, 0x88, 0x88);

// ── Geometry ─────────────────────────────────────────────────────────────────
#define EYE_SIZE         56
#define PUPIL_SIZE       20
#define LID_H_HALF      (EYE_SIZE / 2)
#define EYE_LEFT_X      105
#define EYE_RIGHT_X     199
#define EYE_Y           115
#define MOUTH_X         140
#define MOUTH_Y         205
#define MOUTH_W          80
#define MOUTH_H          60
#define BLUSH_W          36
#define BLUSH_H          18
#define BLUSH_LEFT_X     88
#define BLUSH_RIGHT_X   236
#define BLUSH_Y         178

// ── Animation helpers ─────────────────────────────────────────────────────────
static void anim_height_cb(void* obj, int32_t v) {
    lv_obj_set_height((lv_obj_t*)obj, v);
}
static void anim_x_offset_cb(void* obj, int32_t v) {
    lv_obj_align((lv_obj_t*)obj, LV_ALIGN_CENTER, v, 0);
}

// ── Public API ────────────────────────────────────────────────────────────────

void FaceDisplay::init(lv_obj_t* screen) {
    // Face container — sits above background, no own bg/border
    lv_obj_t* face = lv_obj_create(screen);
    lv_obj_set_size(face, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(face, 0, 0);
    lv_obj_set_style_bg_opa(face, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(face, 0, LV_PART_MAIN);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_SCROLLABLE);

    create_eye(face, EYE_LEFT_X,  EYE_Y, &eye_left_bg_,  &pupil_left_,  &lid_left_);
    create_eye(face, EYE_RIGHT_X, EYE_Y, &eye_right_bg_, &pupil_right_, &lid_right_);
    create_mouth(face);
    create_blush(face);
    create_accent_ring(screen);   // accent ring on top of everything

    // Start blink timer
    blink_timer_ = lv_timer_create(blink_timer_cb, blink_interval_ms_, this);

    set_state(DEVICE_STATE_IDLE);
    ESP_LOGI(TAG, "init done");
}

void FaceDisplay::set_state(device_state_t state) {
    if (state == current_state_) return;
    current_state_ = state;
    stop_pupil_animation();

    switch (state) {
        case DEVICE_STATE_IDLE:        apply_idle_expression();       break;
        case DEVICE_STATE_LISTENING:   apply_listening_expression();  break;
        case DEVICE_STATE_THINKING:    apply_thinking_expression();   break;
        case DEVICE_STATE_SPEAKING:    apply_speaking_expression();   break;
        case DEVICE_STATE_ERROR:       apply_error_expression();      break;
        case DEVICE_STATE_CONNECTING:  apply_connecting_expression(); break;
        default: break;
    }
}

void FaceDisplay::set_mouth_rms(float rms) {
    if (current_state_ != DEVICE_STATE_SPEAKING) return;
    rms = std::max(0.0f, std::min(1.0f, rms));
    // Mouth arc: quiet=100° span, loud=220° span, centered at bottom (270°)
    int span  = 100 + (int)(rms * 120.0f);
    int start = 270 - span / 2;
    int end   = 270 + span / 2;
    lv_arc_set_angles(mouth_arc_, (uint16_t)start, (uint16_t)end);
}

void FaceDisplay::set_accent_color(lv_color_t color) {
    lv_obj_set_style_arc_color(accent_ring_, color, LV_PART_INDICATOR);
    lv_arc_set_value(accent_ring_, 100); // full ring
}

void FaceDisplay::tick_100ms() {
    // Randomise blink interval every cycle to feel natural
    if (current_state_ == DEVICE_STATE_IDLE) {
        uint32_t jitter = (uint32_t)(rand() % 1000);
        lv_timer_set_period(blink_timer_, 3000 + jitter);
    }
}

// ── Widget constructors ───────────────────────────────────────────────────────

void FaceDisplay::create_eye(lv_obj_t* parent, lv_coord_t x, lv_coord_t y,
                              lv_obj_t** bg, lv_obj_t** pupil, lv_obj_t** lid) {
    // Sclera (white circle)
    *bg = lv_obj_create(parent);
    lv_obj_set_size(*bg, EYE_SIZE, EYE_SIZE);
    lv_obj_set_pos(*bg, x, y);
    lv_obj_set_style_radius(*bg, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(*bg, COLOR_SCLERA, LV_PART_MAIN);
    lv_obj_set_style_border_width(*bg, 0, LV_PART_MAIN);
    lv_obj_clear_flag(*bg, LV_OBJ_FLAG_SCROLLABLE);

    // Pupil (dark circle, centered inside sclera)
    *pupil = lv_obj_create(*bg);
    lv_obj_set_size(*pupil, PUPIL_SIZE, PUPIL_SIZE);
    lv_obj_set_style_radius(*pupil, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(*pupil, COLOR_PUPIL, LV_PART_MAIN);
    lv_obj_set_style_border_width(*pupil, 0, LV_PART_MAIN);
    lv_obj_align(*pupil, LV_ALIGN_CENTER, 0, 0);

    // Eyelid overlay — same colour as screen bg, animated height for blink
    *lid = lv_obj_create(*bg);
    lv_obj_set_size(*lid, EYE_SIZE, 0);
    lv_obj_set_pos(*lid, 0, 0);
    lv_obj_set_style_bg_color(*lid, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(*lid, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(*lid, 0, LV_PART_MAIN);
    lv_obj_move_foreground(*lid);
}

void FaceDisplay::create_mouth(lv_obj_t* parent) {
    mouth_arc_ = lv_arc_create(parent);
    lv_obj_set_size(mouth_arc_, MOUTH_W, MOUTH_H);
    lv_obj_set_pos(mouth_arc_, MOUTH_X, MOUTH_Y);
    lv_arc_set_mode(mouth_arc_, LV_ARC_MODE_NORMAL);
    lv_arc_set_range(mouth_arc_, 0, 100);
    lv_arc_set_value(mouth_arc_, 100);
    lv_arc_set_rotation(mouth_arc_, 180);           // arc starts at bottom
    lv_arc_set_bg_angles(mouth_arc_, 0, 360);

    // Style: thick red arc, no knob, no bg arc visible
    lv_obj_set_style_arc_color(mouth_arc_, COLOR_MOUTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(mouth_arc_, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(mouth_arc_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_style(mouth_arc_, NULL, LV_PART_KNOB);
    lv_obj_set_style_opa(mouth_arc_, LV_OPA_TRANSP, LV_PART_KNOB);
}

void FaceDisplay::create_blush(lv_obj_t* parent) {
    // Left blush
    blush_left_ = lv_obj_create(parent);
    lv_obj_set_size(blush_left_, BLUSH_W, BLUSH_H);
    lv_obj_set_pos(blush_left_, BLUSH_LEFT_X, BLUSH_Y);
    lv_obj_set_style_radius(blush_left_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(blush_left_, COLOR_BLUSH, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(blush_left_, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_border_width(blush_left_, 0, LV_PART_MAIN);

    // Right blush
    blush_right_ = lv_obj_create(parent);
    lv_obj_set_size(blush_right_, BLUSH_W, BLUSH_H);
    lv_obj_set_pos(blush_right_, BLUSH_RIGHT_X, BLUSH_Y);
    lv_obj_set_style_radius(blush_right_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(blush_right_, COLOR_BLUSH, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(blush_right_, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_border_width(blush_right_, 0, LV_PART_MAIN);
}

void FaceDisplay::create_accent_ring(lv_obj_t* parent) {
    accent_ring_ = lv_arc_create(parent);
    lv_obj_set_size(accent_ring_, LCD_WIDTH - 4, LCD_HEIGHT - 4);
    lv_obj_align(accent_ring_, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_bg_angles(accent_ring_, 0, 360);
    lv_arc_set_range(accent_ring_, 0, 100);
    lv_arc_set_value(accent_ring_, 100);

    lv_obj_set_style_arc_width(accent_ring_, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(accent_ring_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_opa(accent_ring_, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_arc_color(accent_ring_, ACCENT_IDLE, LV_PART_INDICATOR);
}

// ── Expression helpers ────────────────────────────────────────────────────────

void FaceDisplay::apply_idle_expression() {
    // Eyes: normal size, centred pupils
    lv_obj_set_size(eye_left_bg_,  EYE_SIZE, EYE_SIZE);
    lv_obj_set_size(eye_right_bg_, EYE_SIZE, EYE_SIZE);
    lv_obj_align(pupil_left_,  LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(pupil_right_, LV_ALIGN_CENTER, 0, 0);

    // Blush visible
    lv_obj_set_style_bg_opa(blush_left_,  LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(blush_right_, LV_OPA_40, LV_PART_MAIN);

    // Mouth: gentle smile arc 200°→340° centred at bottom
    lv_arc_set_angles(mouth_arc_, 200, 340);
    lv_obj_set_style_arc_color(mouth_arc_, COLOR_MOUTH, LV_PART_INDICATOR);

    set_accent_color(ACCENT_IDLE);
    lv_timer_resume(blink_timer_);
}

void FaceDisplay::apply_listening_expression() {
    // Eyes wider
    lv_obj_set_size(eye_left_bg_,  EYE_SIZE + 8, EYE_SIZE + 8);
    lv_obj_set_size(eye_right_bg_, EYE_SIZE + 8, EYE_SIZE + 8);
    lv_obj_align(pupil_left_,  LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(pupil_right_, LV_ALIGN_CENTER, 0, 0);

    // Mouth: small 'O' circle (short arc span = near full circle tiny)
    lv_arc_set_angles(mouth_arc_, 0, 355);
    lv_obj_set_style_arc_color(mouth_arc_, LV_COLOR_MAKE(0xFF, 0xFF, 0xFF), LV_PART_INDICATOR);

    lv_obj_set_style_bg_opa(blush_left_,  LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(blush_right_, LV_OPA_50, LV_PART_MAIN);

    set_accent_color(ACCENT_LISTEN);
    lv_timer_pause(blink_timer_);
}

void FaceDisplay::apply_thinking_expression() {
    lv_obj_set_size(eye_left_bg_,  EYE_SIZE, EYE_SIZE);
    lv_obj_set_size(eye_right_bg_, EYE_SIZE, EYE_SIZE);

    // Mouth: nearly closed line
    lv_arc_set_angles(mouth_arc_, 255, 285);
    lv_obj_set_style_arc_color(mouth_arc_, COLOR_MOUTH, LV_PART_INDICATOR);

    lv_obj_set_style_bg_opa(blush_left_,  LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(blush_right_, LV_OPA_20, LV_PART_MAIN);

    set_accent_color(ACCENT_THINK);
    lv_timer_pause(blink_timer_);
    start_pupil_scan();
}

void FaceDisplay::apply_speaking_expression() {
    lv_obj_set_size(eye_left_bg_,  EYE_SIZE, EYE_SIZE);
    lv_obj_set_size(eye_right_bg_, EYE_SIZE, EYE_SIZE);
    lv_obj_align(pupil_left_,  LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(pupil_right_, LV_ALIGN_CENTER, 0, 0);

    // Mouth: will be driven by set_mouth_rms(); start at medium
    lv_arc_set_angles(mouth_arc_, 220, 320);
    lv_obj_set_style_arc_color(mouth_arc_, COLOR_MOUTH, LV_PART_INDICATOR);

    lv_obj_set_style_bg_opa(blush_left_,  LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(blush_right_, LV_OPA_40, LV_PART_MAIN);

    set_accent_color(ACCENT_SPEAK);
    lv_timer_resume(blink_timer_);
}

void FaceDisplay::apply_error_expression() {
    // Left eye squinted (smaller height), right normal
    lv_obj_set_height(eye_left_bg_,  EYE_SIZE / 2);
    lv_obj_set_height(eye_right_bg_, EYE_SIZE);

    // Frown: inverted arc
    lv_arc_set_angles(mouth_arc_, 20, 160);
    lv_obj_set_style_arc_color(mouth_arc_, LV_COLOR_MAKE(0x88, 0x44, 0xFF), LV_PART_INDICATOR);

    lv_obj_set_style_bg_opa(blush_left_,  LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(blush_right_, LV_OPA_TRANSP, LV_PART_MAIN);

    set_accent_color(ACCENT_ERROR);
    lv_timer_pause(blink_timer_);
}

void FaceDisplay::apply_connecting_expression() {
    lv_obj_set_size(eye_left_bg_,  EYE_SIZE, EYE_SIZE / 2);  // half-closed
    lv_obj_set_size(eye_right_bg_, EYE_SIZE, EYE_SIZE / 2);

    lv_arc_set_angles(mouth_arc_, 220, 320);
    lv_obj_set_style_arc_color(mouth_arc_, LV_COLOR_MAKE(0xAA, 0xAA, 0xAA), LV_PART_INDICATOR);

    lv_obj_set_style_bg_opa(blush_left_,  LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(blush_right_, LV_OPA_TRANSP, LV_PART_MAIN);

    set_accent_color(ACCENT_CONNECT);
    lv_timer_pause(blink_timer_);
}

// ── Animations ────────────────────────────────────────────────────────────────

void FaceDisplay::trigger_blink() {
    // Animate both lids: height 0 → EYE_SIZE → 0 in 160ms
    for (lv_obj_t* lid : {lid_left_, lid_right_}) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, lid);
        lv_anim_set_exec_cb(&a, anim_height_cb);
        lv_anim_set_values(&a, 0, EYE_SIZE);
        lv_anim_set_time(&a, 80);
        lv_anim_set_playback_time(&a, 80);
        lv_anim_set_repeat_count(&a, 1);
        lv_anim_start(&a);
    }
}

void FaceDisplay::stop_pupil_animation() {
    lv_anim_del(pupil_left_,  anim_x_offset_cb);
    lv_anim_del(pupil_right_, anim_x_offset_cb);
    lv_obj_align(pupil_left_,  LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(pupil_right_, LV_ALIGN_CENTER, 0, 0);
}

void FaceDisplay::start_pupil_scan() {
    // Pupils scan left ↔ right every 800ms — thinking animation
    for (lv_obj_t* pupil : {pupil_left_, pupil_right_}) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, pupil);
        lv_anim_set_exec_cb(&a, anim_x_offset_cb);
        lv_anim_set_values(&a, -10, 10);
        lv_anim_set_time(&a, 800);
        lv_anim_set_playback_time(&a, 800);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }
}

void FaceDisplay::blink_timer_cb(lv_timer_t* timer) {
    FaceDisplay* self = (FaceDisplay*)lv_timer_get_user_data(timer);
    if (self && self->current_state_ == DEVICE_STATE_IDLE) {
        self->trigger_blink();
    }
}
