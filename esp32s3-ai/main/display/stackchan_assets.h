/**
 * @file stackchan_assets.h
 * @brief C/C++-compatible declarations for face bitmaps and expression presets.
 *        Bitmap pixel data lives in stackchan_assets.c (pure-C compilation unit).
 */
#pragma once

#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Bitmap native dimensions (pixels) ─────────────────────────────────── */
#define STACKCHAN_EYE_W    55
#define STACKCHAN_EYE_H    65
#define STACKCHAN_MOUTH_W  60
#define STACKCHAN_MOUTH_H  12

/**
 * Pre-rendered alpha-8 image descriptors.
 * 0x00 = fully transparent, 0xFF = fully opaque.
 * Recolor with lv_obj_set_style_img_recolor() to tint white/red/etc.
 */
extern const lv_img_dsc_t g_eye_bitmap;
extern const lv_img_dsc_t g_mouth_bitmap;

/**
 * @brief Per-state expression geometry.
 * Array indexed by device_state_t (see device_state.h):
 *   [0] UNKNOWN    — idle fallback
 *   [1] CONNECTING — half-closed / sleepy eyes, small neutral mouth
 *   [2] IDLE       — open eyes, gentle smile
 *   [3] LISTENING  — wide eyes, "O" mouth
 *   [4] THINKING   — squinted eyes, flat mouth
 *   [5] SPEAKING   — open eyes, mouth animated by audio RMS
 *   [6] ERROR      — angry squint, frown
 */
typedef struct {
    int16_t  eye_width;       /**< Visible clip width for each eye (px)        */
    int16_t  eye_height;      /**< Visible clip height for each eye (px)       */
    int16_t  eye_spacing;     /**< Horizontal distance between eye centres (px)*/
    int16_t  eye_y_offset;    /**< Eye centre Y offset from display centre (px)*/
    int16_t  pupil_width;     /**< Pupil width (px)                           */
    int16_t  pupil_height;    /**< Pupil height (px)                          */
    int16_t  pupil_offset_x;  /**< Static pupil X offset from eye centre (px) */
    int16_t  pupil_offset_y;  /**< Static pupil Y offset from eye centre (px) */
    int16_t  mouth_width;     /**< Mouth widget reference width (px)          */
    int16_t  mouth_height;    /**< Mouth widget reference height (px)         */
    int16_t  mouth_y_offset;  /**< Mouth centre Y offset from display centre  */
    int16_t  mouth_curvature; /**< <0 smile, 0 flat, >0 sad, >=10 "O" circle  */
    uint32_t accent_color;    /**< RGB888 accent ring colour                  */
    uint8_t  ring_thickness;  /**< Accent ring stroke width (px)              */
} face_preset_t;

extern const face_preset_t g_face_presets[7];

#ifdef __cplusplus
}
#endif
