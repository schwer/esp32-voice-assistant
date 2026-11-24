// SPDX-License-Identifier: MIT
// Board definition: Waveshare ESP32-S3-AUDIO-Board (ESP32-S3R8, 8 MB octal PSRAM)
// Sources: Waveshare wiki (codec pins), ESPHome device page (I2C addresses, expander).
// TODO: verify TCA9555 pin assignments against the official schematic.
#pragma once

// Shared I2C bus (codecs, IO expander, RTC)
#define BOARD_I2C_PORT          0
#define BOARD_I2C_SDA           11
#define BOARD_I2C_SCL           10

// Shared I2S bus: ES8311 (DAC -> speaker), ES7210 (4-ch ADC <- dual mics + AEC ref)
#define BOARD_I2S_PORT          0
#define BOARD_I2S_MCLK          12
#define BOARD_I2S_BCLK          13
#define BOARD_I2S_WS            14
#define BOARD_I2S_DOUT          16   // ES8311 DSDIN
#define BOARD_I2S_DIN           15   // ES7210 ASDOUT

// I2C addresses (7-bit)
#define BOARD_ES8311_ADDR       0x18
#define BOARD_ES7210_ADDR       0x40
#define BOARD_TCA9555_ADDR      0x20
#define BOARD_PCF85063_ADDR     0x51

// TCA9555 IO expander pins (unverified)
#define BOARD_EXP_PA_ENABLE     8    // speaker amplifier enable
#define BOARD_EXP_PA_ENABLE_MASK (1u << BOARD_EXP_PA_ENABLE)

// RGB LED ring: 7 x WS2812 (GPIO from the ESPHome device page; verify on schematic)
#define BOARD_LED_GPIO          38
#define BOARD_LED_COUNT         7
// Color byte order. Standard WS2812 is GRB, but red showed up as green on the
// reference board, so its LEDs take RGB.
#define BOARD_LED_COLOR_FORMAT  LED_STRIP_COLOR_COMPONENT_FMT_RGB

// Both codecs share MCLK/BCLK/WS, so capture and playback run at ONE rate.
// Mic audio goes to the AFE and Gemini at 16 kHz as-is; Gemini's 24 kHz
// output is resampled 3:2 down to 16 kHz before playback.
#define BOARD_I2S_SAMPLE_RATE   16000

// microSD slot: SDMMC, 1 data line. From a community pin map derived from Waveshare's
// demo code and schematic (github.com/jensenbox/waveshare-esp32-s3-audio); verify on
// the schematic. DAT3 is on the IO expander and must be high for SD mode.
#define BOARD_SD_CLK            40
#define BOARD_SD_CMD            42
#define BOARD_SD_D0             41
#define BOARD_EXP_SD_D3         3
