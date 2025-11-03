#include "platform/tdeck/tdeck_cardputer.h"

#include <SPI.h>
#include <Wire.h>
#include <esp_err.h>
#include <esp_idf_version.h>
#include <esp_intr_alloc.h>

#include <array>
#include <cstdarg>
#include <vector>

#include <algorithm>
#include <cctype>

extern bool g_display_ready;

namespace {
constexpr uint8_t kBacklightSteps = 16;

constexpr auto &kKeyboardLayout = tdeck::keyboard::kDefaultLayout;
constexpr std::size_t kMatrixCols = kKeyboardLayout.columns;
constexpr std::size_t kMatrixRows = kKeyboardLayout.rows;

void writeKeyboardCommand(uint8_t command) {
  Wire.beginTransmission(LILYGO_KB_ADDRESS);
  Wire.write(command);
  Wire.endTransmission(true);
}

void writeKeyboardCommand(uint8_t command, uint8_t value) {
  Wire.beginTransmission(LILYGO_KB_ADDRESS);
  Wire.write(command);
  Wire.write(value);
  Wire.endTransmission(true);
}
}  // namespace

static uint8_t g_backlight_level = 0;

void tdeck_set_backlight(uint8_t level) {
  level = std::min<uint8_t>(level, kBacklightSteps);
  if(level == 0) {
    digitalWrite(BOARD_TFT_BACKLIGHT, LOW);
    delay(3);
    g_backlight_level = 0;
    return;
  }

  if(g_backlight_level == 0) {
    digitalWrite(BOARD_TFT_BACKLIGHT, HIGH);
    g_backlight_level = kBacklightSteps;
    delayMicroseconds(30);
  }

  int from = kBacklightSteps - g_backlight_level;
  int to = kBacklightSteps - level;
  int num = (kBacklightSteps + to - from) % kBacklightSteps;
  for(int i = 0; i < num; ++i) {
    digitalWrite(BOARD_TFT_BACKLIGHT, LOW);
    digitalWrite(BOARD_TFT_BACKLIGHT, HIGH);
  }
  g_backlight_level = level;
}

TDeckDisplay::TDeckDisplay() : tft_(std::unique_ptr<TFT_eSPI>(new TFT_eSPI())) {}

void TDeckDisplay::begin() {
  if(!tft_) {
    tft_ = std::unique_ptr<TFT_eSPI>(new TFT_eSPI());
  }
  tft_->begin();
  tft_->setSwapBytes(true);
  tft_->fillScreen(TFT_BLACK);
}

void TDeckDisplay::setRotation(uint8_t rotation) {
  if(tft_) {
    tft_->setRotation(rotation);
  }
}

void TDeckDisplay::initDMA() {}

bool TDeckDisplay::dmaBusy() const {
  return false;
}

void TDeckDisplay::waitDMA() {}

void TDeckDisplay::startWrite() {
  if(tft_) {
    tft_->startWrite();
  }
}

void TDeckDisplay::endWrite() {
  if(tft_) {
    tft_->endWrite();
  }
}

void TDeckDisplay::setAddrWindow(int32_t x, int32_t y, int32_t w, int32_t h) {
  if(tft_) {
    tft_->setAddrWindow(x, y, w, h);
  }
}

void TDeckDisplay::writePixelsDMA(const uint16_t *data, int32_t len, bool) {
  if(tft_ && data && len > 0) {
    tft_->pushPixels(const_cast<uint16_t *>(data), len);
  }
}

void TDeckDisplay::writePixels(const uint16_t *data, int32_t len) {
  writePixelsDMA(data, len, false);
}

void TDeckDisplay::writePixelPreclipped(int32_t x, int32_t y, uint16_t color) {
  if(tft_) {
    tft_->drawPixel(x, y, color);
  }
}

void TDeckDisplay::clearDisplay() {
  if(tft_) {
    tft_->fillScreen(TFT_BLACK);
  }
}

void TDeckDisplay::fillScreen(uint16_t color) {
  if(tft_) {
    tft_->fillScreen(color);
  }
}

void TDeckDisplay::drawString(const char *string, int32_t x, int32_t y) {
  if(tft_) {
    tft_->drawString(string, x, y);
  }
}

void TDeckDisplay::setTextFont(uint8_t font) {
  if(tft_) {
    tft_->setTextFont(font);
  }
}

void TDeckDisplay::setTextSize(uint8_t size) {
  if(tft_) {
    tft_->setTextSize(size);
  }
}

void TDeckDisplay::setTextColor(uint16_t fg) {
  if(tft_) {
    tft_->setTextColor(fg);
  }
}

void TDeckDisplay::setTextColor(uint16_t fg, uint16_t bg) {
  if(tft_) {
    tft_->setTextColor(fg, bg);
  }
}

void TDeckDisplay::setTextWrap(bool wrap) {
  if(tft_) {
    tft_->setTextWrap(wrap);
  }
}

int16_t TDeckDisplay::textWidth(const char *string) {
  return tft_ ? tft_->textWidth(string) : 0;
}

int16_t TDeckDisplay::fontHeight() {
  return tft_ ? tft_->fontHeight() : 0;
}

void TDeckDisplay::fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint16_t color) {
  if(tft_) {
    tft_->fillRoundRect(x, y, w, h, radius, color);
  }
}

void TDeckDisplay::drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint16_t color) {
  if(tft_) {
    tft_->drawRoundRect(x, y, w, h, radius, color);
  }
}

void TDeckDisplay::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
  if(tft_) {
    tft_->fillRect(x, y, w, h, color);
  }
}

void TDeckDisplay::drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
  if(tft_) {
    tft_->drawRect(x, y, w, h, color);
  }
}

void TDeckDisplay::drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t color) {
  if(tft_) {
    tft_->drawLine(x0, y0, x1, y1, color);
  }
}

void TDeckDisplay::drawCircle(int32_t x, int32_t y, int32_t r, uint16_t color) {
  if(tft_) {
    tft_->drawCircle(x, y, r, color);
  }
}

void TDeckDisplay::fillCircle(int32_t x, int32_t y, int32_t r, uint16_t color) {
  if(tft_) {
    tft_->fillCircle(x, y, r, color);
  }
}

void TDeckDisplay::setCursor(int32_t x, int32_t y) {
  if(tft_) {
    tft_->setCursor(x, y);
  }
}

uint16_t TDeckDisplay::color565(uint8_t r, uint8_t g, uint8_t b) {
  return tft_ ? tft_->color565(r, g, b) : 0;
}

int16_t TDeckDisplay::width() const {
  return tft_ ? tft_->width() : 0;
}

int16_t TDeckDisplay::height() const {
  return tft_ ? tft_->height() : 0;
}

void TDeckDisplay::printf(const char *fmt, ...) {
  if(!tft_ || fmt == nullptr) {
    return;
  }

  va_list args;
  va_start(args, fmt);
  int length = vsnprintf(nullptr, 0, fmt, args);
  va_end(args);

  if(length <= 0) {
    return;
  }

  std::vector<char> buffer(static_cast<size_t>(length) + 1);
  va_start(args, fmt);
  vsnprintf(buffer.data(), buffer.size(), fmt, args);
  va_end(args);

  tft_->print(buffer.data());
}

void TDeckDisplay::println() {
  if(tft_) {
    tft_->println();
  }
}

bool TDeckDisplay::drawJpgFile(const char *, int32_t, int32_t, int32_t, int32_t) {
  return false;
}

bool TDeckDisplay::drawPngFile(const char *, int32_t, int32_t, int32_t, int32_t) {
  return false;
}

void TDeckDisplay::pushImageDMA(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *data) {
  if(tft_ && data) {
    tft_->pushImage(x, y, w, h, data);
  }
}

void Speaker_Class::config(const config_t &cfg) {
  config_ = cfg;
}

bool Speaker_Class::begin() {
  if(started_) {
    return true;
  }

  i2s_config_t i2s_config{};
  i2s_config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  i2s_config.sample_rate = static_cast<int>(config_.sample_rate);
  i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2s_config.channel_format = config_.stereo ? I2S_CHANNEL_FMT_RIGHT_LEFT : I2S_CHANNEL_FMT_ONLY_LEFT;
  i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2s_config.dma_buf_count = static_cast<int>(config_.dma_buf_count);
  i2s_config.dma_buf_len = static_cast<int>(config_.dma_buf_len);
  i2s_config.use_apll = false;
  i2s_config.tx_desc_auto_clear = true;
  i2s_config.fixed_mclk = 0;
#if ESP_IDF_VERSION_MAJOR >= 5
  i2s_config.mclk_multiple = I2S_MCLK_MULTIPLE_DEFAULT;
  i2s_config.bits_per_chan = I2S_BITS_PER_CHAN_DEFAULT;
#endif

  if(i2s_driver_install(config_.i2s_port, &i2s_config, 0, nullptr) != ESP_OK) {
    return false;
  }

  i2s_pin_config_t pin_config{};
  pin_config.mck_io_num = I2S_PIN_NO_CHANGE;
  pin_config.bck_io_num = config_.pin_bck;
  pin_config.ws_io_num = config_.pin_ws;
  pin_config.data_out_num = config_.pin_data_out;
  pin_config.data_in_num = I2S_PIN_NO_CHANGE;

  if(i2s_set_pin(config_.i2s_port, &pin_config) != ESP_OK) {
    i2s_driver_uninstall(config_.i2s_port);
    return false;
  }

  i2s_zero_dma_buffer(config_.i2s_port);
  if(i2s_set_clk(config_.i2s_port,
                 config_.sample_rate,
                 I2S_BITS_PER_SAMPLE_16BIT,
                 config_.stereo ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO) != ESP_OK) {
    i2s_driver_uninstall(config_.i2s_port);
    return false;
  }

  current_sample_rate_ = config_.sample_rate;
  current_stereo_ = config_.stereo;
  queued_frames_.store(0);
  started_ = true;
  return true;
}

void Speaker_Class::end() {
  if(!started_) {
    return;
  }

  i2s_driver_uninstall(config_.i2s_port);
  started_ = false;
  queued_frames_.store(0);
}

bool Speaker_Class::playRaw(const int16_t *samples,
                            size_t sample_count,
                            uint32_t sample_rate,
                            bool stereo,
                            uint32_t repeat,
                            int,
                            bool) {
  if(samples == nullptr || sample_count == 0) {
    return false;
  }

  if(!started_ && !begin()) {
    return false;
  }

  if(sample_rate == 0) {
    sample_rate = config_.sample_rate;
  }

  if(sample_rate != current_sample_rate_ || stereo != current_stereo_) {
    if(i2s_set_clk(config_.i2s_port,
                   sample_rate,
                   I2S_BITS_PER_SAMPLE_16BIT,
                   stereo ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO) != ESP_OK) {
      return false;
    }
    current_sample_rate_ = sample_rate;
    current_stereo_ = stereo;
  }

  const uint32_t iterations = std::max<uint32_t>(1, repeat);
  const size_t bytes = sample_count * sizeof(int16_t);

  for(uint32_t i = 0; i < iterations; ++i) {
    size_t written = 0;
    queued_frames_.fetch_add(1, std::memory_order_relaxed);
    const esp_err_t err = i2s_write(config_.i2s_port, samples, bytes, &written, portMAX_DELAY);
    queued_frames_.fetch_sub(1, std::memory_order_relaxed);
    if(err != ESP_OK || written != bytes) {
      return false;
    }
  }

  return true;
}

void Keyboard_Class::begin() {
  writeKeyboardCommand(LILYGO_KB_DEFAULT_BRIGHTNESS_CMD, 127);
  last_matrix_.fill(0);
  have_last_matrix_ = false;
  raw_mode_confirmed_ = false;
  requestRawMode(true);
  delay(1);
  update();
}

void Keyboard_Class::update() {
  if(layout_ == nullptr) {
    layout_ = &kKeyboardLayout;
  }

  if(!raw_mode_confirmed_) {
    requestRawMode(false);
  }

  KeysState next;
  next.word.fill(0);
  next.hid_keys.fill(0);
  next.fn = false;
  next.ctrl = false;
  next.enter = false;

  const int available = Wire.requestFrom(LILYGO_KB_ADDRESS, static_cast<uint8_t>(KeysState::kMaxKeys));
  if(available >= static_cast<int>(kMatrixCols)) {
    std::array<uint8_t, kMatrixCols> column_state{};
    size_t col_index = 0;
    while(Wire.available() && col_index < kMatrixCols) {
      column_state[col_index++] = static_cast<uint8_t>(Wire.read());
    }
    while(Wire.available()) {
      Wire.read();
    }

    const bool matrix_changed = !have_last_matrix_ || column_state != last_matrix_;
    if(debug_config_.log_raw_matrix && matrix_changed) {
      Serial.print("[T-Deck][Keyboard] matrix:");
      for(size_t col = 0; col < col_index; ++col) {
        Serial.printf(" c%u=0x%02X", static_cast<unsigned>(col), column_state[col]);
      }
      Serial.println();
    }

    last_matrix_ = column_state;
    have_last_matrix_ = true;

    const bool uniform_payload = (col_index > 0) && std::all_of(column_state.begin() + 1,
                                                                column_state.begin() + col_index,
                                                                [&](uint8_t value) {
                                                                  return value == column_state[0];
                                                                });

    size_t write_index = 0;
    auto append_char = [&](char value) {
      if(value == 0 || write_index >= KeysState::kMaxKeys) {
        return;
      }
      next.word[write_index] = value;
      next.hid_keys[write_index] = static_cast<uint8_t>(value);
      ++write_index;
    };

    if(uniform_payload) {
      const uint8_t ascii_value = column_state[0];
      if(ascii_value != 0) {
        char ch = static_cast<char>(ascii_value);
        if(ch == '\r') {
          ch = '\n';
        }
        append_char(ch);
        if(ch == '\n' || ch == '\r') {
          next.enter = true;
        }
        pressed_ = true;
        if(debug_config_.log_decoded_keys) {
          const bool printable = (ch >= 32 && ch <= 126);
          // Serial.printf("[T-Deck][Keyboard] key: ascii 0x%02X", ascii_value);
          if(printable) {
            Serial.printf(" ('%c')", ch);
          }
          Serial.println();
        }
        have_last_matrix_ = false;
        requestRawMode(true);
        raw_mode_confirmed_ = false;
      } else {
        pressed_ = false;
        if(!raw_mode_confirmed_) {
          requestRawMode(false);
        }
      }
      state_ = next;
      return;
    }

    if(layout_->columns != kMatrixCols || layout_->rows != kMatrixRows) {
      // Layout mismatch – avoid using stale buffers.
      have_last_matrix_ = false;
      state_ = next;
      pressed_ = false;
      return;
    }

    raw_mode_confirmed_ = true;

    bool pressed_matrix[kMatrixCols][kMatrixRows] = {};
    bool any_pressed = false;
    for(size_t col = 0; col < kMatrixCols; ++col) {
      const uint8_t mask = column_state[col];
      for(size_t row = 0; row < kMatrixRows; ++row) {
        const bool pressed = ((mask >> row) & 0x01u) != 0;
        pressed_matrix[col][row] = pressed;
        if(pressed) {
          any_pressed = true;
        }
      }
    }

    const auto is_active = [&](const tdeck::keyboard::KeyPosition &pos) {
      return pos.column < kMatrixCols && pos.row < kMatrixRows && pressed_matrix[pos.column][pos.row];
    };

    const bool symbol_layer = is_active(layout_->symbol_toggle);
    const bool alt_active = is_active(layout_->alt_modifier);
    const bool shift_active = is_active(layout_->shift_left) || is_active(layout_->shift_right);
    const bool enter_active = is_active(layout_->enter_key);
    const bool backspace_active = is_active(layout_->backspace_key);

    next.fn = alt_active;
    next.ctrl = false;
    next.enter = enter_active;

    if(enter_active) {
      append_char('\n');
      if(debug_config_.log_decoded_keys) {
        Serial.println("[T-Deck][Keyboard] key: Enter (0x0A)");
      }
    }
    if(backspace_active) {
      append_char('\b');
      if(debug_config_.log_decoded_keys) {
        // Serial.println("[T-Deck][Keyboard] key: Backspace (0x08)");
      }
    }

    const auto is_modifier_position = [&](size_t col, size_t row) {
      return (layout_->symbol_toggle.column == col && layout_->symbol_toggle.row == row) ||
             (layout_->alt_modifier.column == col && layout_->alt_modifier.row == row) ||
             (layout_->shift_left.column == col && layout_->shift_left.row == row) ||
             (layout_->shift_right.column == col && layout_->shift_right.row == row) ||
             (layout_->enter_key.column == col && layout_->enter_key.row == row) ||
             (layout_->backspace_key.column == col && layout_->backspace_key.row == row);
    };

    for(size_t col = 0; col < kMatrixCols; ++col) {
      for(size_t row = 0; row < kMatrixRows; ++row) {
        if(!pressed_matrix[col][row]) {
          continue;
        }

        if(is_modifier_position(col, row)) {
          continue;
        }

        char value = symbol_layer ? layout_->symbolAt(col, row) : layout_->baseAt(col, row);
        if(value == 0) {
          continue;
        }

        if(shift_active && value >= 'a' && value <= 'z') {
          value = static_cast<char>(std::toupper(static_cast<unsigned char>(value)));
        }

        if(debug_config_.log_decoded_keys) {
          const bool printable = (value >= 32 && value <= 126);
          // Serial.printf("[T-Deck][Keyboard] key: col=%u row=%u -> 0x%02X",
          //               static_cast<unsigned>(col),
          //               static_cast<unsigned>(row),
          //               static_cast<uint8_t>(value));
          if(printable) {
            // Serial.printf(" ('%c')", value);
          }
          // Serial.printf(" [sym=%d shift=%d alt=%d]\n", symbol_layer, shift_active, alt_active);
        }

        append_char(value);
      }
    }

    pressed_ = any_pressed;
  } else {
    have_last_matrix_ = false;
    size_t index = 0;
    while(Wire.available() && index < KeysState::kMaxKeys) {
      int value = Wire.read();
      if(value <= 0) {
        continue;
      }
      char ch = static_cast<char>(value);
      next.word[index] = ch;
      next.hid_keys[index] = static_cast<uint8_t>(value);
      if(ch == '\n' || ch == '\r') {
        next.enter = true;
      }
      if(debug_config_.log_decoded_keys) {
        const bool printable = (ch >= 32 && ch <= 126);
        Serial.printf("[T-Deck][Keyboard] key: passthrough 0x%02X", static_cast<uint8_t>(ch));
        if(printable) {
          Serial.printf(" ('%c')", ch);
        }
        Serial.println();
      }
      ++index;
    }
    pressed_ = index > 0;
    if(pressed_) {
      requestRawMode(true);
      raw_mode_confirmed_ = false;
    }
  }

  state_ = next;
}

Keyboard_Class::KeysState Keyboard_Class::keysState() const {
  return state_;
}

void Keyboard_Class::requestRawMode(bool force) {
  const unsigned long now = millis();
  constexpr unsigned long kRetryIntervalMs = 200;
  if(force || (now - last_raw_mode_request_ms_ >= kRetryIntervalMs)) {
    writeKeyboardCommand(LILYGO_KB_MODE_RAW_CMD);
    last_raw_mode_request_ms_ = now;
  }
}

bool Keyboard_Class::isPressed() const {
  return pressed_;
}

bool Keyboard_Class::isKeyPressed(char key) const {
  return std::find(state_.word.begin(), state_.word.end(), key) != state_.word.end();
}

void Keyboard_Class::setLayout(const tdeck::keyboard::KeyboardLayout &layout) {
  if(layout.columns != kMatrixCols || layout.rows != kMatrixRows) {
    Serial.println("[T-Deck][Keyboard] Ignoring layout change (dimension mismatch)");
    return;
  }
  layout_ = &layout;
  have_last_matrix_ = false;
  last_matrix_.fill(0);
}

void Keyboard_Class::setDebugConfig(const tdeck::keyboard::KeyboardDebugConfig &debug) {
  debug_config_ = debug;
}

TDeckCardputer_Class M5Cardputer;

void TDeckCardputer_Class::begin() {
  begin(true);
}

void TDeckCardputer_Class::begin(bool enable_display) {
  Serial.println("[T-Deck] begin start");

  g_display_ready = false;

  pinMode(BOARD_POWERON, OUTPUT);
  digitalWrite(BOARD_POWERON, HIGH);

  pinMode(BOARD_SDCARD_CS, OUTPUT);
  digitalWrite(BOARD_SDCARD_CS, HIGH);

  pinMode(RADIO_CS_PIN, OUTPUT);
  digitalWrite(RADIO_CS_PIN, HIGH);

  pinMode(BOARD_TFT_CS, OUTPUT);
  pinMode(BOARD_TFT_DC, OUTPUT);
  digitalWrite(BOARD_TFT_CS, HIGH);

  pinMode(BOARD_SPI_MISO, INPUT_PULLUP);
  pinMode(BOARD_BOOT_PIN, INPUT_PULLUP);
  pinMode(BOARD_TBOX_G02, INPUT_PULLUP);
  pinMode(BOARD_TBOX_G01, INPUT_PULLUP);
  pinMode(BOARD_TBOX_G04, INPUT_PULLUP);
  pinMode(BOARD_TBOX_G03, INPUT_PULLUP);
  pinMode(BOARD_TOUCH_INT, INPUT);

  pinMode(BOARD_TFT_BACKLIGHT, OUTPUT);
  digitalWrite(BOARD_TFT_BACKLIGHT, LOW);

  SPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI);
  Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
  Wire.setClock(400000);

  Keyboard.begin();

  if(enable_display) {
    Display.begin();
    Display.setRotation(1);
    tdeck_set_backlight(12);
    g_display_ready = true;
  }

  Serial.println("[T-Deck] begin complete");
}

void TDeckCardputer_Class::update() {
  Keyboard.update();
}
