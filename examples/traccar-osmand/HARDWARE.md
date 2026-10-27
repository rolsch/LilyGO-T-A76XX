# HARDWARE.md — T-SIM767XG-S3 (SIM7670G / SIM7672G)

> **Hinweis / Note:** SIM7672G and SIM7670G are exactly the same module, except for the name.
> This project refers to both models as **SIM767X**. All pin definitions below apply to both.

## Overview

| Item | Value |
|---|---|
| Board | LilyGO T-SIM767XG-S3 |
| MCU | ESP32-S3 |
| Modem | SIM7670G / SIM7672G (LTE Cat-1 / NB-IoT) |
| GNSS | GPS, GLONASS, Galileo, BeiDou (integrated in modem) |
| Board macro | `LILYGO_T_SIM7670G_S3` (see [`utilities.h`](utilities.h:197)) |
| Modem macro | `TINY_GSM_MODEM_SIM7670G` |
| Product page | https://lilygo.cc/products/t-sim-7670g-s3 |

## Pin map (ESP32-S3 GPIO)

### Modem interface (SerialAT = Serial1, 115200 8N1)

| Function | GPIO | Notes |
|---|---|---|
| `MODEM_TX_PIN` | 11 | ESP TX → modem RX |
| `MODEM_RX_PIN` | 10 | ESP RX ← modem TX |
| `MODEM_DTR_PIN` | 9 | Pulled HIGH to put modem into sleep (used together with `modem.sleepEnable(true)`) |
| `MODEM_RING_PIN` | 3 | Ring indicator |
| `MODEM_RESET_PIN` | 17 | Active LOW (`MODEM_RESET_LEVEL = LOW`) |
| `BOARD_PWRKEY_PIN` | 18 | Power key, pulse sequence boots the modem |
| `BOARD_LED_PIN` | 12 | Status LED, active LOW (`LED_ON = LOW`) |

### Analog / power

| Function | GPIO | Notes |
|---|---|---|
| `BOARD_BAT_ADC_PIN` | 4 | Battery voltage divider |
| `BOARD_SOLAR_ADC_PIN` | 5 | Solar input voltage divider |

### SD card (SPI)

| Function | GPIO |
|---|---|
| `BOARD_MISO_PIN` | 47 |
| `BOARD_MOSI_PIN` | 14 |
| `BOARD_SCK_PIN` | 21 |
| `BOARD_SD_CS_PIN` | 13 |

### GPS / GNSS

| Function | Value | Notes |
|---|---|---|
| `MODEM_GPS_ENABLE_GPIO` | 4 | Active HIGH, passed to `modem.enableGPS()` (modem-side GPS antenna power control) |
| GNSS mode | GPS + GLONASS + Galileo + BDS | `GNSS_MODE_GPS_GLONASS_GALILEO_BDS` (see [`traccar-osmand.ino`](traccar-osmand.ino:449)) |
| AGPS | **not supported** | SIM767X does not support the GPS acceleration function (see [`traccar-osmand.ino`](traccar-osmand.ino:458)) |

### I2C

| Function | GPIO |
|---|---|
| `BOARD_SDA_PIN` | 2 |
| `BOARD_SCL_PIN` | 1 |

### SimShield pins (only valid when a SimShield is attached)

| Function | GPIO |
|---|---|
| `SIMSHIELD_MOSI` | 15 |
| `SIMSHIELD_MISO` | 7 |
| `SIMSHIELD_SCK` | 16 |
| `SIMSHIELD_SD_CS` | 46 |
| `SIMSHIELD_RADIO_BUSY` | 38 |
| `SIMSHIELD_RADIO_CS` | 39 |
| `SIMSHIELD_RADIO_IRQ` | 6 |
| `SIMSHIELD_RADIO_RST` | 40 |
| `SIMSHIELD_RS_RX` | 41 |
| `SIMSHIELD_RS_TX` | 42 |
| `SIMSHIELD_SDA` | 2 |
| `SIMSHIELD_SCL` | 1 |
| `SerialRS485` | Serial2 |

### SIM-DC-Shield pins (only valid when a SIM-DC-Shield is attached)

| Function | GPIO |
|---|---|
| `SIM_DC_SHIELD_A0` | 7 |
| `SIM_DC_SHIELD_A1` | 6 |
| `SIM_DC_SHIELD_D0` | 15 |
| `SIM_DC_SHIELD_D1` | 16 |
| `SIM_DC_SHIELD_SENSOR_IRQ` | 8 |

## Power-on / power-off sequence

| Parameter | Value |
|---|---|
| `MODEM_POWERON_PULSE_WIDTH_MS` | 100 |
| `MODEM_POWEROFF_PULSE_WIDTH_MS` | 3000 |
| `MODEM_START_WAIT_MS` | 3000 |

Boot sequence (see [`traccar-osmand.ino`](traccar-osmand.ino:260)):
`PWRKEY` (GPIO 18) LOW → 100 ms → HIGH → 100 ms → LOW, then wait for AT response (up to 30 retries).

## Modem sleep

- Sleep: pull `MODEM_DTR_PIN` (GPIO 9) HIGH + `modem.sleepEnable(true)`
- Wake: pull `MODEM_DTR_PIN` LOW, wait ~500 ms
- The ESP32-S3 enters light sleep (`esp_light_sleep_start()`) while waiting
- See [`traccar-osmand.ino`](traccar-osmand.ino:166) `modem_enter_sleep()`

## Power consumption (measured, see [`README.MD`](README.MD:1))

| State | Current |
|---|---|
| Startup (instantaneous) | > 200 mA |
| Modem wake (instantaneous) | > 120 mA |
| Sleep (ESP + modem, after successful location upload) | 2–3 mA |

## Notes

- `MODEM_REG_SMS_ONLY` is defined for this modem: the firmware detects "SMS-only" network registration and reports it (see [`traccar-osmand.ino`](traccar-osmand.ino:389)).
- WiFi and BLE are not used; the ESP32-S3 acts only as MCU for the modem.
- Board selection: uncomment `#define LILYGO_T_SIM7670G_S3` in [`utilities.h`](utilities.h:33).
