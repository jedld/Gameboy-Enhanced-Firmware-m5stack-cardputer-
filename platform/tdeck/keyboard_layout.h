#pragma once

#include <cstddef>
#include <cstdint>

namespace tdeck::keyboard {

struct KeyPosition {
  uint8_t column;
  uint8_t row;
};

// Update this file to adapt the firmware to alternative keyboard matrices.
// - Adjust kDefaultColumns / kDefaultRows for the column/row count.
// - Edit kDefaultBaseMap / kDefaultSymbolMap (column-major order) to remap keys.
// - Update the KeyPosition entries below to match modifier locations.
// - Flip log_raw_matrix/log_decoded_keys in kDefaultDebugConfig to print diagnostics.

struct KeyboardLayout {
  std::size_t columns;
  std::size_t rows;
  const char *base;
  const char *symbol;
  KeyPosition symbol_toggle;
  KeyPosition alt_modifier;
  KeyPosition shift_left;
  KeyPosition shift_right;
  KeyPosition enter_key;
  KeyPosition backspace_key;

  constexpr char baseAt(std::size_t column, std::size_t row) const {
    return (column < columns && row < rows) ? base[column * rows + row] : 0;
  }

  constexpr char symbolAt(std::size_t column, std::size_t row) const {
    return (column < columns && row < rows) ? symbol[column * rows + row] : 0;
  }
};

struct KeyboardDebugConfig {
  bool log_raw_matrix = false;
  bool log_decoded_keys = false;
};

struct KeyboardConfig {
  KeyboardLayout layout;
  KeyboardDebugConfig debug;
};

constexpr std::size_t kDefaultColumns = 5;
constexpr std::size_t kDefaultRows = 7;

constexpr char kDefaultBaseMap[kDefaultColumns * kDefaultRows] = {
    // Column 0
    'q', 'w', 0, 'a', 0, ' ', 0,
    // Column 1
    'e', 's', 'd', 'p', 'x', 'z', 0,
    // Column 2
    'r', 'g', 't', 0, 'v', 'c', 'f',
    // Column 3
    'u', 'h', 'y', 0, 'b', 'n', 'j',
    // Column 4
    'o', 'l', 'i', 0, '$', 'm', 'k',
};

constexpr char kDefaultSymbolMap[kDefaultColumns * kDefaultRows] = {
    // Column 0
    '#', '1', 0, '*', 0, 0, '0',
    // Column 1
    '2', '4', '5', '@', '8', '7', 0,
    // Column 2
    '3', '/', '(', 0, '?', '9', '6',
    // Column 3
    '_', ':', ')', 0, '!', ',', ';',
    // Column 4
    '+', '"', '-', 0, 0, '.', '\'',
};

constexpr KeyboardLayout kDefaultLayout{
    kDefaultColumns,
    kDefaultRows,
    kDefaultBaseMap,
    kDefaultSymbolMap,
    KeyPosition{0, 2},
    KeyPosition{0, 4},
    KeyPosition{1, 6},
    KeyPosition{2, 3},
    KeyPosition{3, 3},
    KeyPosition{4, 3},
};

constexpr KeyboardDebugConfig kDefaultDebugConfig{};

constexpr KeyboardConfig kDefaultConfig{kDefaultLayout, kDefaultDebugConfig};

}  // namespace tdeck::keyboard
