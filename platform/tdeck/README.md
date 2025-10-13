# T-Deck Keyboard Developer Guide

This document explains how the T-Deck keyboard driver is structured, how to adapt the firmware to a different key matrix, and how to debug input issues when bringing a new keyboard online.

---

## Quick reference

| Purpose | Location / API |
| --- | --- |
| Default layout data & modifier positions | `platform/tdeck/keyboard_layout.h` |
| Runtime keyboard driver | `platform/tdeck/tdeck_cardputer.cpp` (`Keyboard_Class`) |
| Layout/debug setters exposed to sketches | `Keyboard.setLayout(...)`, `Keyboard.setDebugConfig(...)` |
| Flash helper | `./scripts/flash_tdeck.sh` |

The keyboard runs in **raw matrix mode** over I²C. Each update grabs one byte per column, where individual bits map to rows. Layout data translates those bitmasks into printable characters and handles modifier keys.

---

## Updating the default layout

1. **Describe the matrix geometry**  
   Update `kDefaultColumns` and `kDefaultRows` in `keyboard_layout.h` to match the new hardware.

2. **Remap characters**  
   Edit `kDefaultBaseMap` (normal layer) and `kDefaultSymbolMap` (function layer). The arrays are column-major: every 7 entries (default) correspond to a single column from top row to bottom row.

3. **Relocate modifiers**  
   Adjust the `KeyPosition` values inside `kDefaultLayout` for:
   - `symbol_toggle` (Fn layer)
   - `alt_modifier`
   - `shift_left`, `shift_right`
   - `enter_key`
   - `backspace_key`

4. **Rebuild**  
   ```bash
   pio run -e lilygo_tdeck
   ```
   The build will fail fast if the layout dimensions do not match what the driver receives at runtime.

5. **Flash & smoke test**  
   ```bash
   ./scripts/flash_tdeck.sh -p /dev/ttyACM0
   ```
   Replace the serial port with the device path on your machine.

---

## Providing a custom layout at runtime

You can override the default in your sketch before calling `Keyboard.update()`:

```cpp
#include "platform/tdeck/keyboard_layout.h"

using tdeck::keyboard::KeyPosition;
using tdeck::keyboard::KeyboardLayout;
using tdeck::keyboard::KeyboardConfig;

static constexpr char kMyBase[] = {
    // ... column-major matrix ...
};

static constexpr char kMySymbols[] = {
    // ... column-major matrix ...
};

static constexpr KeyboardLayout kMyLayout{
    /* columns = */ 5,
    /* rows    = */ 7,
    /* base    = */ kMyBase,
    /* symbol  = */ kMySymbols,
    /* symbol_toggle = */ KeyPosition{0, 2},
    /* alt_modifier  = */ KeyPosition{0, 4},
    /* shift_left    = */ KeyPosition{1, 6},
    /* shift_right   = */ KeyPosition{2, 3},
    /* enter_key     = */ KeyPosition{3, 3},
    /* backspace_key = */ KeyPosition{4, 3},
};

void setup() {
  Serial.begin(115200);
  M5Cardputer.begin();
  M5Cardputer.Keyboard.setLayout(kMyLayout);
}
```

> **Tip:** Store large maps in flash (`static constexpr char[]`) to keep RAM usage low. Multiple layouts can coexist; swap them at runtime based on detected hardware.

---

## Debugging input

### Enable Serial logging

```cpp
#include "platform/tdeck/keyboard_layout.h"

void setup() {
  Serial.begin(115200);
  M5Cardputer.begin();

  tdeck::keyboard::KeyboardDebugConfig debug{};
  debug.log_raw_matrix = true;      // dumps I²C column bytes
  debug.log_decoded_keys = true;    // logs translated characters
  M5Cardputer.Keyboard.setDebugConfig(debug);
}
```

- **Raw matrix output** looks like:  
  ` [T-Deck][Keyboard] matrix: c0=0x10 c1=0x00 c2=0x04 ...`
- **Decoded key output** shows coordinates, ASCII code, printable character (when available), and modifier state:  
  ` [T-Deck][Keyboard] key: col=2 row=3 -> 0x41 ('A') [sym=0 shift=1 alt=0]`

Remember to leave logging disabled for production builds—continuous Serial prints can starve the frame loop.

### Common checks

- **Layout mismatch:** No characters appear and `log_raw_matrix` prints data. Ensure `columns`/`rows` in your layout match `Wire.requestFrom(...)` bytes (currently 5×7).
- **Stuck modifier:** If every key reports symbol layer, confirm the `symbol_toggle` coordinates and verify the physical key is not pressed.
- **Ghost presses:** Check for wiring differences; the driver assumes diodes prevent ghosting. If your hardware lacks them, debounce or masking logic may be required.

---

## Regression checklist

- [ ] Update the layout arrays and modifier positions.
- [ ] Compile `lilygo_tdeck` in PlatformIO.
- [ ] Flash the firmware to hardware.
- [ ] Enable logging and verify every key output (base & symbol layers).
- [ ] Disable logging and re-test gameplay / emulator input.

Keep this guide close whenever you swap in a new keyboard matrix—most ports only require editing `keyboard_layout.h` and running through the debug steps above.
