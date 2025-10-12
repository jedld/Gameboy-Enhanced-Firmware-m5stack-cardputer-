#pragma once

#include <stdint.h>

// GPIO assignments for LilyGO T-Deck peripherals.
// Values mirror the reference definitions from T-Deck/examples/UnitTest/utilities.h
// so the firmware can drive the same hardware blocks without the upstream helper.

static constexpr uint8_t BOARD_POWERON = 10;
static constexpr uint8_t BOARD_I2C_SDA = 18;
static constexpr uint8_t BOARD_I2C_SCL = 8;
static constexpr uint8_t BOARD_SDCARD_CS = 39;
static constexpr uint8_t BOARD_TFT_CS = 12;
static constexpr uint8_t BOARD_TFT_DC = 11;
static constexpr uint8_t BOARD_TFT_BACKLIGHT = 42;
static constexpr uint8_t BOARD_BL_PIN = BOARD_TFT_BACKLIGHT;
static constexpr uint8_t BOARD_SPI_MOSI = 41;
static constexpr uint8_t BOARD_SPI_MISO = 38;
static constexpr uint8_t BOARD_SPI_SCK = 40;
static constexpr uint8_t BOARD_KEYBOARD_INT = 46;

static constexpr uint8_t BOARD_TOUCH_INT = 16;
static constexpr uint8_t BOARD_BOOT_PIN = 0;
static constexpr uint8_t RADIO_CS_PIN = 9;
static constexpr uint8_t RADIO_BUSY_PIN = 13;
static constexpr uint8_t RADIO_RST_PIN = 17;

static constexpr uint8_t BOARD_TBOX_G02 = 2;
static constexpr uint8_t BOARD_TBOX_G01 = 3;
static constexpr uint8_t BOARD_TBOX_G04 = 1;
static constexpr uint8_t BOARD_TBOX_G03 = 15;

static constexpr uint8_t BOARD_I2S_DOUT = 6;
static constexpr uint8_t BOARD_I2S_BCK = 7;
static constexpr uint8_t BOARD_I2S_WS = 5;
static constexpr uint8_t BOARD_I2S_MCLK = 2;

static constexpr uint8_t BOARD_GPS_RX = 18;  // shared with SDA; left for completeness
static constexpr uint8_t BOARD_GPS_TX = 17;

// Keyboard controller (LILYGO KB) I2C address and command set.
static constexpr uint8_t LILYGO_KB_ADDRESS = 0x55;
static constexpr uint8_t LILYGO_KB_BRIGHTNESS_CMD = 0x01;
static constexpr uint8_t LILYGO_KB_DEFAULT_BRIGHTNESS_CMD = 0x02;
static constexpr uint8_t LILYGO_KB_MODE_RAW_CMD = 0x03;
static constexpr uint8_t LILYGO_KB_MODE_KEY_CMD = 0x04;
