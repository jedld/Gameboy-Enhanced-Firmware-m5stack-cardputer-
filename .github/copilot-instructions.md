## Firmware Layout
- `gb_cardputer.ino` holds setup/game loop, FreeRTOS tasks (`render_task`, `audio_task`), and the Peanut-GB integration; start there when wiring new features.
- The emulator state sits in `priv_t` (framebuffers, PSRAM caches, ROM source metadata) and is stored in `gb.direct.priv`; reuse the helpers in that file instead of allocating new globals.
- ROM streaming goes through `RomCache` (SD, embedded, or flashed sources) and honours PSRAM limits; prefer `rom_cache_*` APIs to touch cartridge data.
- Audio, Bluetooth, and accelerometer support are split out: `minigb_apu_cardputer/` for DSP/EQ, `bluetooth/` for NimBLE keyboard HID, `mbc7_cardputer.*` for BMI270 tilt hooks.

## Build & Flash
- Default toolchain is PlatformIO (`pio run -e m5stack_cardputer`); the `scripts/flash_cardputer.sh` wrapper handles build+upload and optional port selection.
- `platformio.ini` pins ESP-IDF toolchain and disables Bluetooth/MBC7/profiling by default; flip feature flags there or via `-D` overrides before building.
- The sketch assumes 16kB loop stack and the custom partition table in `partitions_cardputer_fullapp.csv`; keep both when adding new PlatformIO environments.
- Serial logging is at 115200 baud; most diagnostics (ROM cache, SD init, save IO) are surfaced through `Serial.printf` in `setup()`.

## Runtime Data Flow
- `setup()` initialises M5Cardputer, PSRAM, settings, then enters the home/file picker UI (`show_home_menu()` and `file_picker()`); the Arduino `loop()` stays empty.
- ROMs can come from SD (`RomSource::SdCard`), embedded firmware payloads (`scripts/embed_rom.py` maintains `embedded_rom.cpp`), or the flash cache partition (`RomSource::Flashed`).
- Large ROMs are copied into the `romstorage` partition (defined in `partitions_cardputer_fullapp.csv`); metadata lives in `RomStorageHeader` and is validated with `ROM_STORAGE_MAGIC`.
- Render frames move through PSRAM framebuffers and a FreeRTOS queue; always check `M5Cardputer.Display.dmaBusy()` / `waitDMA()` before drawing overlays or HUD elements.

## Storage Conventions
- SD mount point is `/sd`; configuration lives in `/sd/config/cardputer_settings.ini` (`audio`, `bootstrap`, `stretch`, `cache`, `volume`, `buttons`).
- Battery-backed saves sit under `/sd/saves/<rom>.sav`; MBC7 EEPROM mirrors use `.mbc7` files beside them and auto-flush every 4s.
- Quick save-states write `.ss` files per slot via `save_state_store_slot`; filenames are derived from the ROM slug or path.
- Screenshots drop into `/sd/screenshots/<rom>_<timestamp>.bmp`, and paths/macros are centralised near the top of `gb_cardputer.ino`.

## Feature Flags & Settings
- Respect `g_settings` (defined in `gb_cardputer.ino`) for user preferences like frame skip, stretch mode, audio, ROM cache size, and button mappings; call `apply_settings_constraints()` after edits.
- Palette behaviour is centralised in `palette_configure_for_dmg()`/`palette_disable_overrides()`; CGB bootstrap palettes live in `cgb_bootstrap_palettes.h`.
- Bluetooth controller input passes through `BluetoothManager::setKeyboardCallback` into `ExternalInput`; reuse that bridge instead of polling NimBLE directly.
- MBC7 tilt relies on `mbc7_cardputer_update()` sampling the BMI270; keep calibration hotkeys (`space`/`0`) and the filtered state when extending motion features.

## Tooling Tips
- Run `python3 scripts/embed_rom.py --help` for ROM embed workflows (`add`, `remove`, `autoboot`, `generate`); the script regenerates `embedded_rom.cpp` and `embedded_rom_config.h`.
- Asset tables such as `dmg.h`, `sgb.h`, and `sgb_borders.h` are generated data blobs; avoid hand-editing and regenerate with the upstream tooling if needed.
- Peanut-GB core lives in `peanutgb/peanut_gb.h`; any patches should remain local to this repo and mirror upstream diffs for easier rebases.
- When adding new automation, mirror the repository’s convention of storing helper scripts under `scripts/` and documenting them in `README.md`.
