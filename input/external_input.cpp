#include "external_input.h"

#if ENABLE_BLUETOOTH_CONTROLLERS

ExternalInput &ExternalInput::instance() {
  static ExternalInput inst;
  return inst;
}

void ExternalInput::setKeyState(uint8_t keycode, bool pressed) {
  key_states_[keycode] = pressed;
}

bool ExternalInput::isKeyPressed(uint8_t keycode) const {
  return key_states_[keycode];
}

void ExternalInput::clear() {
  key_states_.fill(false);
}

#endif  // ENABLE_BLUETOOTH_CONTROLLERS
