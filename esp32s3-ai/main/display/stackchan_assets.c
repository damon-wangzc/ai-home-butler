/**
 * @file stackchan_assets.c
 * @brief Pure-C compilation unit.
 *        Includes the reference alpha-8 bitmap headers (C99 designated-init
 *        syntax) and exports non-static image descriptors plus the full
 *        expression preset table indexed by device_state_t.
 */
#include "stackchan_assets.h"

/*
 * The reference headers define:
 *   static const uint8_t STACKCHAN_EYE_MAP[3575]
 *   static const uint8_t STACKCHAN_MOUTH_MAP[720]
 * plus their own static lv_img_dsc_t wrappers.
 * We only need the raw pixel arrays — we create our own exported descriptors.
 */
#include "stackchan_bitmap_eye.h"
#include "stackchan_bitmap_mouth.h"

/* ── Exported image descriptors ────────────────────────────────────────── */

const lv_img_dsc_t g_eye_bitmap = {
    .header.cf          = LV_IMG_CF_ALPHA_8BIT,
    .header.always_zero = 0,
    .header.w           = STACKCHAN_EYE_WIDTH,    /* 55  */
    .header.h           = STACKCHAN_EYE_HEIGHT,   /* 65  */
    .data_size          = STACKCHAN_EYE_WIDTH * STACKCHAN_EYE_HEIGHT,
    .data               = STACKCHAN_EYE_MAP
};

const lv_img_dsc_t g_mouth_bitmap = {
    .header.cf          = LV_IMG_CF_ALPHA_8BIT,
    .header.always_zero = 0,
    .header.w           = STACKCHAN_MOUTH_WIDTH,  /* 60  */
    .header.h           = STACKCHAN_MOUTH_HEIGHT, /* 12  */
    .data_size          = STACKCHAN_MOUTH_WIDTH * STACKCHAN_MOUTH_HEIGHT,
    .data               = STACKCHAN_MOUTH_MAP
};

/* ── Expression presets indexed by device_state_t ──────────────────────── */
/*
 * Fields (in struct order):
 *   eye_w  eye_h  eye_spacing  eye_y_off
 *   pup_w  pup_h  pup_ox  pup_oy
 *   mouth_w  mouth_h  mouth_y_off  mouth_curv
 *   accent_rgb888  ring_thick
 */
const face_preset_t g_face_presets[7] = {
    /* [0] UNKNOWN — fall back to idle */
    { 55, 65, 120, -15,  32, 32,  0,  0,  60, 12, 45,  -6, 0x4488FF, 6 },
    /* [1] CONNECTING — half-closed sleepy eyes, small neutral mouth */
    { 55, 25, 120, -15,  30, 16,  0,  2,  40,  6, 45,  -2, 0x888888, 5 },
    /* [2] IDLE — relaxed open eyes, gentle smile */
    { 55, 65, 120, -15,  32, 32,  0,  0,  60, 12, 45,  -6, 0x4488FF, 6 },
    /* [3] LISTENING — wide eyes, open "O" mouth */
    { 65, 65, 120, -15,  35, 35,  0,  0,  30, 30, 45,  10, 0x44FF88, 8 },
    /* [4] THINKING — squinted eyes, flat closed mouth */
    { 55, 50, 120, -15,  26, 26,  0,  0,  50,  4, 45,   0, 0xFFAA00, 6 },
    /* [5] SPEAKING — open eyes, mouth driven by audio RMS */
    { 55, 65, 120, -15,  32, 32,  0,  0,  60, 25, 45,  -4, 0x00CCFF, 6 },
    /* [6] ERROR — small angry squint, downward frown */
    { 50, 45, 125, -10,  24, 24, -5,  5,  65, 14, 45,   8, 0xFF4444, 8 },
};
