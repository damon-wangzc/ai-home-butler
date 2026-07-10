#pragma once

// ============================================================
// Waveshare ESP32-S3-Touch-LCD-1.85 pin definitions
// ST77916 360x360 round display via QSPI
// ============================================================

// --- QSPI Display (ST77916) ---
#define PIN_LCD_SCK         40
#define PIN_LCD_DATA0       46
#define PIN_LCD_DATA1       45
#define PIN_LCD_DATA2       42
#define PIN_LCD_DATA3       41
#define PIN_LCD_CS          21
#define PIN_LCD_TE          18
#define PIN_LCD_BL           5   // backlight, PWM via LEDC

#define LCD_WIDTH           360
#define LCD_HEIGHT          360
#define LCD_COLOR_BITS       16
#define LCD_SPI_CLK_HZ      (80 * 1000 * 1000)
#define LCD_TRANS_QUEUE_SZ   10

// --- I2S Speaker (PCM5101) ---
#define PIN_I2S_BCLK        48
#define PIN_I2S_LCLK        38
#define PIN_I2S_DOUT        47
#define I2S_SPEAKER_PORT    I2S_NUM_0

// --- I2S Microphone (INMP441 or equivalent digital MEMS) ---
// If using on-board PDM mic, set PIN_MIC_DIN to the PDM data pin
// and initialise as PDM mode instead of STD mode.
#define PIN_MIC_BCLK        15
#define PIN_MIC_WS          16
#define PIN_MIC_DIN         17
#define I2S_MIC_PORT        I2S_NUM_1

// --- I2C shared bus (CST816 touch, QMI8658, PCF85063, TCA9554PWR) ---
#define PIN_I2C_SDA          1
#define PIN_I2C_SCL          2
#define I2C_FREQ_HZ         (400 * 1000)

// --- I2C device addresses ---
#define I2C_ADDR_CST816     0x15
#define I2C_ADDR_QMI8658    0x6B
#define I2C_ADDR_PCF85063   0x51
#define I2C_ADDR_TCA9554    0x20

// --- Audio parameters ---
#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_BITS          16
#define AUDIO_CHANNELS       1
#define AUDIO_DMA_BUF_COUNT  4
#define AUDIO_DMA_BUF_SAMPLES 320   // 20ms @ 16kHz

// --- LVGL display buffer (in PSRAM) ---
#define LVGL_BUF_LINES      80      // lines per DMA buffer (PSRAM-backed)
#define LVGL_TICK_MS         5
#define LVGL_REFRESH_MS     16      // ~60fps cap
