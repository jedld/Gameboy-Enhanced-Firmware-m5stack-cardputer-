#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <driver/i2s.h>

#include "platform/tdeck/keyboard_layout.h"
#include "platform/tdeck/tdeck_pins.h"

class TDeckDisplay final {
 public:
  TDeckDisplay();

  void begin();
  void setRotation(uint8_t rotation);
  void initDMA();
  bool dmaBusy() const;
  void waitDMA();
  void startWrite();
  void endWrite();
  void setAddrWindow(int32_t x, int32_t y, int32_t w, int32_t h);
  void writePixelsDMA(const uint16_t *data, int32_t len, bool use_dma = true);
  void writePixels(const uint16_t *data, int32_t len);
  void writePixelPreclipped(int32_t x, int32_t y, uint16_t color);
  void clearDisplay();
  void fillScreen(uint16_t color);

  // Basic drawing helpers used throughout the firmware.
  void drawString(const char *string, int32_t x, int32_t y);
  void setTextFont(uint8_t font);
  void setTextSize(uint8_t size);
  void setTextColor(uint16_t fg);
  void setTextColor(uint16_t fg, uint16_t bg);
  void setTextWrap(bool wrap);
  int16_t textWidth(const char *string);
  int16_t fontHeight();
  void fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint16_t color);
  void drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint16_t color);
  void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color);
  void drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color);
  void drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t color);
  void drawCircle(int32_t x, int32_t y, int32_t r, uint16_t color);
  void fillCircle(int32_t x, int32_t y, int32_t r, uint16_t color);
  void setCursor(int32_t x, int32_t y);
  uint16_t color565(uint8_t r, uint8_t g, uint8_t b);
  int16_t width() const;
  int16_t height() const;
  void printf(const char *fmt, ...);

  template <typename T>
  void print(const T &value) {
    if(tft_) {
      tft_->print(value);
    }
  }

  template <typename T>
  void println(const T &value) {
    if(tft_) {
      tft_->println(value);
    }
  }

  void println();

  bool drawJpgFile(const char *path, int32_t x, int32_t y, int32_t max_width, int32_t max_height);
  bool drawPngFile(const char *path, int32_t x, int32_t y, int32_t max_width, int32_t max_height);
  void pushImageDMA(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *data);

 private:
  std::unique_ptr<TFT_eSPI> tft_;
};

class Keyboard_Class {
 public:
  struct KeysState {
    static constexpr size_t kMaxKeys = 10;
    std::array<char, kMaxKeys> word{};
    std::array<uint8_t, kMaxKeys> hid_keys{};
    bool fn = false;
    bool ctrl = false;
    bool enter = false;
    bool sym = false;
    bool alt = false;
  };

  void begin();
  void update();
  KeysState keysState() const;
  bool isPressed() const;
  bool isKeyPressed(char key) const;
  void setLayout(const tdeck::keyboard::KeyboardLayout &layout);
  void setDebugConfig(const tdeck::keyboard::KeyboardDebugConfig &debug);

 private:
  void requestRawMode(bool force = false);

  const tdeck::keyboard::KeyboardLayout *layout_ = &tdeck::keyboard::kDefaultLayout;
  tdeck::keyboard::KeyboardDebugConfig debug_config_ = tdeck::keyboard::kDefaultDebugConfig;
  KeysState state_{};
  bool pressed_ = false;
  std::array<uint8_t, tdeck::keyboard::kDefaultColumns> last_matrix_{};
  bool have_last_matrix_ = false;
  bool raw_mode_confirmed_ = false;
  unsigned long last_raw_mode_request_ms_ = 0;
};

class Speaker_Class {
 public:
  struct config_t {
    int pin_bck = BOARD_I2S_BCK;
    int pin_ws = BOARD_I2S_WS;
    int pin_data_out = BOARD_I2S_DOUT;
    i2s_port_t i2s_port = I2S_NUM_0;
    uint32_t sample_rate = 44100;
    bool stereo = true;
    size_t dma_buf_len = 256;
    size_t dma_buf_count = 8;
  };

  config_t config() const { return config_; }
  void config(const config_t &cfg);
  void setVolume(uint8_t volume) { volume_ = volume; }
  void setAllChannelVolume(uint8_t volume) { volume_ = volume; }
  uint8_t volume() const { return volume_; }
  size_t isPlaying(uint8_t) const { return queued_frames_.load() > 0 ? 1 : 0; }
  void end();
  bool begin();
  bool playRaw(const int16_t *samples,
               size_t sample_count,
               uint32_t sample_rate,
               bool stereo,
               uint32_t repeat,
               int channel,
               bool stop_current_sound);

 private:
  config_t config_{};
  uint8_t volume_ = 255;
  bool started_ = false;
  uint32_t current_sample_rate_ = 0;
  bool current_stereo_ = true;
  std::atomic<size_t> queued_frames_{0};
};

class TDeckCardputer_Class {
 public:
  void begin();
  void begin(bool enable_display);

  template <typename ConfigT>
  void begin(const ConfigT &, bool enable_display) {
    begin(enable_display);
  }
  void update();

  TDeckDisplay Display;
  Keyboard_Class Keyboard;
  Speaker_Class Speaker;
};

extern TDeckCardputer_Class M5Cardputer;

void tdeck_set_backlight(uint8_t level);
