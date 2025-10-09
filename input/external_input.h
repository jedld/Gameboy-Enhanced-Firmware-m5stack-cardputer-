#pragma once

#include <array>
#include <cstdint>

class ExternalInput {
public:
  static ExternalInput &instance();

  void setKeyState(uint8_t keycode, bool pressed);
  bool isKeyPressed(uint8_t keycode) const;

  void clear();

  template <typename Callback>
  void apply(Callback &&cb) const {
    for(uint16_t key = 0; key < key_states_.size(); ++key) {
      if(key_states_[key]) {
        cb(static_cast<uint8_t>(key));
      }
    }
  }

private:
  ExternalInput() = default;

  std::array<bool, 256> key_states_{};
};
