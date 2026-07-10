# ESP32-S3 AI Voice Orb — Firmware

Firmware for the [Waveshare ESP32-S3-Touch-LCD-1.85](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.85) running as the **AI Home Butler Voice Orb**.

## Features
- StackChan-style animated face with 6 expressions (IDLE / LISTENING / THINKING / SPEAKING / ERROR / CONNECTING)
- Wake word detection via ESP-SR (`wn9_hilexin` or custom model)
- Touch-to-wake (tap the round screen)
- Voice streaming to butler orchestrator over WebSocket (`ws://host:8000/audio`)
- TTS audio playback via PCM5101 I2S DAC
- Mouth-sync animation driven by PCM RMS amplitude
- Status bar: real-time clock (PCF85063 RTC), WiFi signal, battery level
- MQTT integration: subscribes `butler/orb/state`, publishes `butler/user/context`

## Hardware
- **MCU**: ESP32-S3 (240 MHz dual-core, 16MB flash, 8MB PSRAM)
- **Display**: ST77916 360×360 round (QSPI), with CST816 capacitive touch
- **Speaker**: PCM5101 I2S DAC
- **Mic**: PDM MEMS (on-board) or INMP441 (external I2S, configure in `board.h`)
- **IMU**: QMI8658 (tap-to-wake)
- **RTC**: PCF85063 (battery-backed clock)

## Build

```bash
cd esp32s3-ai
. $HOME/esp/esp-idf/export.sh      # ESP-IDF v5.3+
idf.py set-target esp32s3
idf.py menuconfig                   # set WiFi, orchestrator IP
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## menuconfig Keys

| Key | Default | Description |
|-----|---------|-------------|
| `AI_ORB_WIFI_SSID` | — | WiFi network name |
| `AI_ORB_WIFI_PASSWORD` | — | WiFi password |
| `AI_ORB_ORCHESTRATOR_HOST` | `192.168.1.10` | Butler server IP |
| `AI_ORB_ORCHESTRATOR_WS_PORT` | `8000` | WebSocket port |
| `AI_ORB_MQTT_HOST` | same as above | MQTT broker IP |
| `AI_ORB_WAKE_WORD` | `wn9_hilexin` | ESP-SR model name |
| `AI_ORB_USER_ID` | `kid` | User ID in MQTT context |

## Reference
See `.github/instructions/esp32s3-orb.md` for full design documentation.
