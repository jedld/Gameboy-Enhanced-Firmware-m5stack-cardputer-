# 1 "/tmp/tmp8govw193"
#include <Arduino.h>
# 1 "/home/jedld/workspace/Gameboy-Enhanced-Firmware-m5stack-cardputer-/gb_cardputer.ino"


#define ENABLE_SOUND 1
#define ENABLE_LCD 1
#define ENABLE_PROFILING 1

#ifndef ENABLE_MBC7
#define ENABLE_MBC7 1
#endif

#define MAX_FILES 256
#define MAX_PATH_LEN 256

#include <vector>
#include <cstring>
#include <cstdio>
#include <cstddef>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <limits.h>
#include <esp_heap_caps.h>
#include <esp_attr.h>
#include <esp32-hal-psram.h>
#include "esp32-hal-cpu.h"
#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#include "esp_err.h"
#endif
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include "M5Cardputer.h"
#if ENABLE_SOUND
#include "minigb_apu_cardputer/minigb_apu.h"
#endif
#include "peanutgb/peanut_gb.h"
#include <SD.h>
#include <pgmspace.h>
#include "gbc.h"
#include "cgb_bootstrap_palettes.h"
#include "mbc7_cardputer.h"
#include "embedded_rom.h"

struct PaletteState;

static inline uint64_t micros64() {
  return static_cast<uint64_t>(esp_timer_get_time());
}

static bool g_file_picker_cancelled = false;

#if ENABLE_PROFILING
static constexpr uint64_t PROFILER_LOG_INTERVAL_US = 5ULL * 1000 * 1000;

struct MainLoopProfiler {
  uint64_t last_log_us;
  uint32_t frames;
  uint32_t over_budget_frames;
  uint64_t accum_frame_us;
  uint64_t accum_poll_us;
  uint64_t accum_emu_us;
  uint64_t accum_dispatch_us;
  uint64_t accum_idle_us;
  uint64_t accum_requested_idle_us;
  uint64_t max_frame_us;
};

struct RenderProfiler {
  uint64_t total_us;
  uint64_t max_us;
  uint32_t frames;
  uint32_t rows_written;
  uint32_t segments_flushed;
};

struct RomCacheProfiler {
  size_t last_hits;
  size_t last_misses;
  size_t last_swaps;
};

static MainLoopProfiler g_main_profiler = {};
static RenderProfiler g_render_profiler = {};
static RomCacheProfiler g_rom_profiler = {};
static portMUX_TYPE profiler_spinlock = portMUX_INITIALIZER_UNLOCKED;

static void profiler_add_render_sample(uint64_t duration_us,
                                       uint32_t rows_written,
                                       uint32_t segments_flushed);
static RenderProfiler profiler_consume_render_stats();
static void profiler_record_frame(uint64_t frame_us,
                                  uint64_t poll_us,
                                  uint64_t emu_us,
                                  uint64_t dispatch_us,
                                  uint64_t idle_us,
                                  uint64_t requested_idle_us,
                                  bool over_budget,
                                  uint64_t now);
#endif

#define DEST_W 240
#define DEST_H 135

static constexpr uint32_t RENDER_TASK_STACK_SIZE = 2048;
static constexpr uint32_t AUDIO_TASK_STACK_SIZE = 2048;

#define DEBUG_DELAY 0

#define DISPLAY_CENTER(x) x + (DEST_W/2 - LCD_WIDTH/2)


static constexpr size_t ROM_STREAM_BLOCK_SIZE = 0x1000;
static constexpr size_t ROM_CACHE_BANK_MAX = 24;
static constexpr size_t ROM_CACHE_BANK_LIMIT_NO_PSRAM = 9;

static const uint16_t DMG_DEFAULT_PALETTE_RGB565[4] = { 0xFFFF, 0xAD55, 0x528A, 0x0000 };
static constexpr uint16_t FALLBACK_COLOUR_RGB565 = 0x0000;
static const char * const GBC_PALETTE_NAMES[GBC_PALETTE_COUNT] = {
  "Default (No buttons)",
  "Up",
  "Up + A",
  "Up + B",
  "Left",
  "Left + A",
  "Left + B",
  "Down",
  "Down + A",
  "Down + B",
  "Right",
  "Right + A",
  "Right + B"
};
static const size_t DEFAULT_GBC_PALETTE_INDEX = 0;

static constexpr size_t JOYPAD_BUTTON_COUNT = 8;




static uint16_t *swap_fb = nullptr;
static bool swap_fb_enabled = false;
static bool swap_fb_dma_capable = false;
static bool swap_fb_psram_backed = false;
static bool display_cache_valid = false;
static bool frame_row_map_initialised = false;
static uint8_t frame_row_map[DEST_H];
static uint16_t frame_row_weight[DEST_H];
static uint32_t swap_row_hash[DEST_H];
static constexpr unsigned int FALLBACK_SEGMENT_ROWS = 4;
static uint16_t fallback_segment_buffer[FALLBACK_SEGMENT_ROWS * LCD_WIDTH];

enum class JoypadButton : uint8_t {
  Up = 0,
  Down,
  Left,
  Right,
  A,
  B,
  Start,
  Select
};

static const char * const JOYPAD_BUTTON_LABELS[JOYPAD_BUTTON_COUNT] = {
  "Up",
  "Down",
  "Left",
  "Right",
  "A",
  "B",
  "Start",
  "Select"
};

static constexpr uint8_t DEFAULT_JOYPAD_KEYMAP[JOYPAD_BUTTON_COUNT] = {
  static_cast<uint8_t>('e'),
  static_cast<uint8_t>('s'),
  static_cast<uint8_t>('a'),
  static_cast<uint8_t>('d'),
  static_cast<uint8_t>('l'),
  static_cast<uint8_t>('k'),
  static_cast<uint8_t>('1'),
  static_cast<uint8_t>('2')
};

struct RomCacheBank {
  int32_t bank_number;
  bool valid;
  uint8_t *data;
  int16_t lru_prev;
  int16_t lru_next;
};

struct RomCache {
  File file;
  size_t size;
  const uint8_t *memory_rom;
  size_t memory_size;
  bool use_memory;
  uint8_t *bank0;
  RomCacheBank banks[ROM_CACHE_BANK_MAX];
  size_t bank_count;
  size_t cache_hits;
  size_t cache_misses;
  size_t cache_swaps;
  size_t bank_size;
  uint32_t bank_mask;
  uint32_t hot_bank_base;
  uint8_t bank_shift;
  uint8_t bank_shift_valid;
  int16_t lru_head;
  int16_t lru_tail;
  int32_t hot_bank;
  uint8_t *hot_bank_ptr;
};

enum class RomSource : uint8_t {
  None = 0,
  SdCard,
  Embedded
};

enum FrameSkipMode : uint8_t {
  FRAME_SKIP_MODE_AUTO = 0,
  FRAME_SKIP_MODE_FORCED = 1,
  FRAME_SKIP_MODE_DISABLED = 2,
  FRAME_SKIP_MODE_COUNT
};

struct FirmwareSettings {
  bool audio_enabled;
  bool cgb_bootstrap_palettes;
  uint8_t rom_cache_banks;
  uint8_t master_volume;
  uint8_t frame_skip_mode;
  uint8_t button_mapping[JOYPAD_BUTTON_COUNT];
};

static constexpr uint8_t DEFAULT_MASTER_VOLUME = 255;
static constexpr uint8_t SETTINGS_VERSION = 3;
static constexpr uint8_t VOLUME_STEP = 16;
static constexpr const char *SETTINGS_DIR = "/config";
static constexpr const char *SETTINGS_FILE_PATH = "/config/cardputer_settings.ini";
static constexpr const char *SAVES_DIR = "/saves";
static constexpr const char *SAVE_FILE_EXTENSION = ".sav";
static constexpr const char *MBC7_FILE_EXTENSION = ".mbc7";
static constexpr uint32_t SAVE_AUTO_FLUSH_INTERVAL_MS = 4000;
static constexpr uint32_t SAVE_FLUSH_RETRY_DELAY_MS = 1000;
static constexpr size_t MBC7_EEPROM_WORD_COUNT = 128;
static constexpr size_t MBC7_EEPROM_RAW_SIZE = MBC7_EEPROM_WORD_COUNT * sizeof(uint16_t);

static FirmwareSettings g_settings = {
  true,
  true,
  ROM_CACHE_BANK_MAX,
  DEFAULT_MASTER_VOLUME,
  static_cast<uint8_t>(FRAME_SKIP_MODE_AUTO),
  {
    static_cast<uint8_t>('e'),
    static_cast<uint8_t>('s'),
    static_cast<uint8_t>('a'),
    static_cast<uint8_t>('d'),
    static_cast<uint8_t>('l'),
    static_cast<uint8_t>('k'),
    static_cast<uint8_t>('1'),
    static_cast<uint8_t>('2')
  }
};
static bool g_psram_available = false;
static bool g_sd_mounted = false;
static bool g_spi2_initialised = false;
static bool g_settings_loaded = false;
static bool g_settings_dirty = false;

static bool ensure_sd_card(bool blocking);
static bool ensure_settings_dir();
static bool load_settings_from_sd();
static bool save_settings_to_sd();
static void apply_settings_constraints();
static void reset_save_state(struct priv_t *priv);
static void set_sd_rom_path(struct priv_t *priv, const char *path);
static void clear_sd_rom_path(struct priv_t *priv);
static bool ensure_saves_dir();
static void sanitise_identifier(const char *input, char *output, size_t output_len);
static bool build_save_path_for_rom(const char *rom_path, const char *extension, char *out, size_t out_len);
static bool derive_save_paths(struct priv_t *priv, bool uses_mbc7);
static bool load_cart_ram_from_sd(struct priv_t *priv);
static bool save_cart_ram_to_sd(const struct priv_t *priv);
static bool load_mbc7_eeprom_from_sd(struct priv_t *priv, struct gb_s *gb);
static bool save_mbc7_eeprom_to_sd(const struct priv_t *priv, const struct gb_s *gb);
static void handle_volume_keys(const Keyboard_Class::KeysState &status);
static void adjust_master_volume(int delta, bool persist, bool announce);
static void configure_performance_profile();

#if ENABLE_SOUND
static void apply_speaker_volume();
#else
static void apply_speaker_volume() {}
#endif

static constexpr uint8_t FRAME_SKIP_ENABLE_STREAK_DEFAULT = 6;
static constexpr uint8_t FRAME_SKIP_DISABLE_STREAK_DEFAULT = 30;
static constexpr uint8_t FRAME_SKIP_MIN_ACTIVE_FRAMES_DEFAULT = 24;
static constexpr uint8_t FRAME_SKIP_HOLD_FRAMES_DEFAULT = 6;

static constexpr uint8_t FRAME_SKIP_ENABLE_STREAK_FALLBACK = 4;
static constexpr uint8_t FRAME_SKIP_DISABLE_STREAK_FALLBACK = 18;
static constexpr uint8_t FRAME_SKIP_MIN_ACTIVE_FRAMES_FALLBACK = 16;
static constexpr uint8_t FRAME_SKIP_HOLD_FRAMES_FALLBACK = 4;

static constexpr uint8_t INTERLACE_ENABLE_STREAK = 4;
static constexpr uint8_t INTERLACE_DISABLE_STREAK = 24;
static constexpr uint8_t INTERLACE_HOLD_FRAMES = 16;

struct AdaptiveInterlaceState {
  uint8_t over_budget_streak;
  uint8_t under_budget_streak;
  uint8_t hold_frames;
  bool active;
};

static AdaptiveInterlaceState g_interlace_state = {};

static inline void updateAdaptiveFrameSkip(struct gb_s *gb, bool over_budget) {
  auto &state = gb->display.frame_skip_state;
  struct priv_t *priv = (struct priv_t *)gb->direct.priv;

  const bool full_cache_mode = swap_fb_enabled && swap_fb_psram_backed;
  const bool fallback_mode = !full_cache_mode;

  const uint8_t enable_streak = fallback_mode ? FRAME_SKIP_ENABLE_STREAK_FALLBACK
                                              : FRAME_SKIP_ENABLE_STREAK_DEFAULT;
  const uint8_t disable_streak = fallback_mode ? FRAME_SKIP_DISABLE_STREAK_FALLBACK
                                               : FRAME_SKIP_DISABLE_STREAK_DEFAULT;
  const uint8_t hold_frames_target = fallback_mode ? FRAME_SKIP_HOLD_FRAMES_FALLBACK
                                                   : FRAME_SKIP_HOLD_FRAMES_DEFAULT;
  const uint8_t min_active_frames_target = fallback_mode ? FRAME_SKIP_MIN_ACTIVE_FRAMES_FALLBACK
                                                         : FRAME_SKIP_MIN_ACTIVE_FRAMES_DEFAULT;


  if(state.hold_frames == 0) {
    state.hold_frames = hold_frames_target;
  }
  if(state.minimum_active_frames == 0) {
    state.minimum_active_frames = min_active_frames_target;
  }

  if(state.hold_frames != hold_frames_target) {
    state.hold_frames = hold_frames_target;
  }
  if(state.minimum_active_frames != min_active_frames_target) {
    state.minimum_active_frames = min_active_frames_target;
  }

  const bool frame_skip_supported = (priv != nullptr);

  if(state.debounce_frames_remaining > 0) {
    state.debounce_frames_remaining--;
  }

  if(state.frames_since_toggle < 0xFF) {
    state.frames_since_toggle++;
  }

  if(over_budget) {
    if(state.over_budget_streak < 0xFF) {
      state.over_budget_streak++;
    }
    state.under_budget_streak = 0;
  } else {
    if(state.under_budget_streak < 0xFF) {
      state.under_budget_streak++;
    }
    if(state.over_budget_streak > 0) {
      state.over_budget_streak--;
    }
  }

  const bool can_toggle = (state.debounce_frames_remaining == 0) &&
                          (state.frames_since_toggle >= state.minimum_active_frames);

  AdaptiveInterlaceState &interlace = g_interlace_state;
  const bool allow_interlace = (gb->cgb.enabled != 0) && swap_fb_enabled && swap_fb_psram_backed;

  if(allow_interlace) {
    if(over_budget) {
      if(interlace.over_budget_streak < 0xFF) {
        interlace.over_budget_streak++;
      }
      interlace.under_budget_streak = 0;
      interlace.hold_frames = INTERLACE_HOLD_FRAMES;
    } else {
      if(interlace.under_budget_streak < 0xFF) {
        interlace.under_budget_streak++;
      }
      if(interlace.over_budget_streak > 0) {
        interlace.over_budget_streak--;
      }
      if(interlace.hold_frames > 0) {
        interlace.hold_frames--;
      }
    }

    if(!interlace.active) {
      if(interlace.over_budget_streak >= INTERLACE_ENABLE_STREAK) {
        interlace.active = true;
        interlace.hold_frames = INTERLACE_HOLD_FRAMES;
        interlace.under_budget_streak = 0;
        gb->display.interlace_count = 0;
      }
    } else {
      const bool ready_to_disable =
          (interlace.hold_frames == 0) && (interlace.under_budget_streak >= INTERLACE_DISABLE_STREAK);
      if(ready_to_disable) {
        interlace.active = false;
        interlace.over_budget_streak = 0;
        interlace.under_budget_streak = 0;
        interlace.hold_frames = 0;
      }
    }

    gb->direct.interlace = interlace.active ? 1 : 0;

    if(interlace.active) {

      if(state.over_budget_streak > enable_streak * 2) {
        interlace.over_budget_streak = INTERLACE_ENABLE_STREAK;
      } else {
        over_budget = false;
      }
    }
  } else {
    if(interlace.active) {
      interlace = {};
    }
    gb->direct.interlace = 0;
    gb->display.interlace_count = 0;
  }

  if(!frame_skip_supported && state.current_frame_skip) {
    state.current_frame_skip = 0;
    gb->direct.frame_skip = 0;
    gb->display.frame_skip_count = 0;
    state.over_budget_streak = 0;
  }

  if(!state.current_frame_skip) {
   if(frame_skip_supported && can_toggle &&
     state.over_budget_streak >= enable_streak) {
      state.current_frame_skip = 1;
      gb->direct.frame_skip = 1;
      state.debounce_frames_remaining = state.hold_frames;
      state.frames_since_toggle = 0;
      state.over_budget_streak = 0;
      state.under_budget_streak = 0;
    }
  } else {
    if(can_toggle && state.under_budget_streak >= disable_streak) {
      state.current_frame_skip = 0;
      gb->direct.frame_skip = 0;
      gb->display.frame_skip_count = 0;
      state.debounce_frames_remaining = state.hold_frames;
      state.frames_since_toggle = 0;
      state.over_budget_streak = 0;
      state.under_budget_streak = 0;
    }
  }
}

static inline void apply_frame_skip_policy(struct gb_s *gb, bool over_budget) {
  FrameSkipMode mode = static_cast<FrameSkipMode>(g_settings.frame_skip_mode);


  updateAdaptiveFrameSkip(gb, over_budget);

  auto &state = gb->display.frame_skip_state;

  switch(mode) {
    case FRAME_SKIP_MODE_AUTO:
      return;
    case FRAME_SKIP_MODE_FORCED:
      gb->direct.frame_skip = 1;
      state.current_frame_skip = 1;
      state.over_budget_streak = 0;
      state.under_budget_streak = 0;
      state.debounce_frames_remaining = 0;
      state.frames_since_toggle = 0;
      return;
    case FRAME_SKIP_MODE_DISABLED:
      gb->direct.frame_skip = 0;
      gb->display.frame_skip_count = 0;
      state.current_frame_skip = 0;
      state.over_budget_streak = 0;
      state.under_budget_streak = 0;
      state.debounce_frames_remaining = 0;
      if(state.frames_since_toggle < state.minimum_active_frames) {
        state.frames_since_toggle = state.minimum_active_frames;
      }
      return;
    default:
      g_settings.frame_skip_mode = static_cast<uint8_t>(FRAME_SKIP_MODE_AUTO);
      return;
  }
}
static size_t rom_cache_preferred_bank_limit();
static size_t rom_cache_preferred_block_size();
void debugPrint(const char* str);
static bool build_save_path_for_rom(const char *rom_path,
                                    const char *extension,
                                    char *out,
                                    size_t out_len);
static bool load_mbc7_eeprom_from_sd(struct priv_t *, struct gb_s *);
static bool save_mbc7_eeprom_to_sd(const struct priv_t *, const struct gb_s *);
uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr);
uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr);
void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr,
         const uint8_t val);
static bool rom_cache_prepare_buffers(RomCache *cache);
static bool rom_cache_trim_banks(RomCache *cache, size_t new_count);
static bool rom_cache_fill_bank(RomCache *cache, RomCacheBank *slot, uint32_t bank);
static bool load_embedded_rom(struct priv_t *priv, const EmbeddedRomEntry *entry);
static void palette_apply_gbc_index(PaletteState *palette,
                                    size_t index,
                                    bool auto_selected,
                                    bool combo_selected);
static void palette_disable_overrides(PaletteState *palette);
static bool palette_map_combo(uint8_t direction,
                              bool button_a,
                              bool button_b,
                              size_t *index_out);
void gb_error(struct gb_s *gb, const enum gb_error_e gb_err, const uint16_t val);
void lcd_draw_line(struct gb_s *gb, const uint8_t pixels[160],
     const uint_fast8_t line);
void fit_frame(const uint16_t *fb, const uint32_t *row_hash, uint8_t *row_dirty);
void draw_frame(const uint16_t *fb);
static void renderTask(void *param);
static void audioTask(void *param);
static void profiler_reset_main(uint64_t now);
static void profiler_log(uint64_t now);
void set_font_size(int size);
static bool is_escape_key_char(char key);
static String format_binding_label(uint8_t key);
static void adjust_frame_skip_mode(int delta);
static bool has_rom_extension(const String &file_name);
static bool try_display_box_art(const String &rom_path, int x, int y, int max_width, int max_height);
char* file_picker();
static void audio_reset_buffers();
static uint32_t audio_select_sample_rate();
static void audioCoreInit(uint32_t sample_rate);
static void audioTeardown();
static void audio_release_finished();
static int audio_acquire_buffer();
static void audio_queue_push(size_t idx);
static bool gb_run_frame_watchdog(struct gb_s *gb,
                                  uint32_t max_steps,
                                  uint32_t *steps_executed);
void setup();
void loop();
#line 509 "/home/jedld/workspace/Gameboy-Enhanced-Firmware-m5stack-cardputer-/gb_cardputer.ino"
static size_t rom_cache_preferred_bank_limit() {
  return g_psram_available ? ROM_CACHE_BANK_MAX : ROM_CACHE_BANK_LIMIT_NO_PSRAM;
}

static size_t rom_cache_preferred_block_size() {
  return g_psram_available ? ROM_STREAM_BLOCK_SIZE : ROM_BANK_SIZE;
}

static inline void rom_cache_update_geometry(RomCache *cache) {
  if(cache == nullptr) {
    return;
  }

  cache->bank_mask = 0;
  cache->bank_shift = 0;
  cache->bank_shift_valid = 0;

  const size_t block_size = cache->bank_size;
  if(block_size == 0) {
    return;
  }

  if((block_size & (block_size - 1)) == 0) {
    cache->bank_mask = static_cast<uint32_t>(block_size - 1);
    cache->bank_shift = static_cast<uint8_t>(__builtin_ctzll(static_cast<unsigned long long>(block_size)));
    cache->bank_shift_valid = 1;
  }
}

static inline uint32_t rom_cache_bank_base(const RomCache *cache, uint32_t bank) {
  const size_t block_size = cache->bank_size ? cache->bank_size : ROM_STREAM_BLOCK_SIZE;
  if(cache->bank_shift_valid) {
    return bank << cache->bank_shift;
  }
  return bank * static_cast<uint32_t>(block_size);
}

struct PaletteState {
  bool gbc_enabled;
  uint8_t active_index;
  bool auto_assigned;
  bool combo_override;
  char label[32];
  uint16_t bg_rgb565[4];
  uint16_t obj0_rgb565[4];
  uint16_t obj1_rgb565[4];
};

struct priv_t;
static void rom_cache_reset(RomCache *cache);
static bool rom_cache_open(RomCache *cache, const char *path);
static bool rom_cache_open_memory(RomCache *cache, const uint8_t *data, size_t size);
static inline uint8_t IRAM_ATTR rom_cache_read(RomCache *cache, uint32_t addr);
static void rom_cache_close(RomCache *cache);
static uint8_t rom_cache_cgb_flag(const RomCache *cache);
static uint8_t* rom_cache_alloc_block(size_t bytes, bool prefer_internal_first);
static inline int16_t IRAM_ATTR rom_cache_bank_index(const RomCache *cache, const RomCacheBank *bank);
static inline void IRAM_ATTR rom_cache_lru_detach(RomCache *cache, int16_t index);
static inline void IRAM_ATTR rom_cache_lru_touch(RomCache *cache, int16_t index);
static inline void IRAM_ATTR rom_cache_lru_insert_after(RomCache *cache, int16_t index, int16_t after_index);
static const char* gbc_palette_name(size_t index);
static void palette_apply_dmg(PaletteState *palette);
static void palette_set_label(PaletteState *palette, const char *label);
static void palette_apply_gbc_index(PaletteState *palette, size_t index,
                                    bool auto_selected,
                                    bool combo_selected);
static void palette_apply_bootstrap(PaletteState *palette,
                                    size_t index,
                                    bool auto_selected,
                                    bool combo_selected);
static bool palette_capture_boot_combo(size_t *out_index);
static void palette_wait_for_boot_combo_release();
static size_t palette_lookup_auto_index(const struct priv_t *priv,
                                        bool *out_bootstrap,
                                        char *label_buffer,
                                        size_t label_buffer_len);
static void palette_configure_for_dmg(struct priv_t *priv);
static void apply_default_button_mapping();
static void wait_for_keyboard_release();
static void show_home_menu();
static void show_options_menu();
static void show_keymap_menu();
#if ENABLE_SOUND
static void audioSetup();
static void audioPump();
static size_t audio_queue_count = 0;
#endif


SPIClass SPI2;

static bool ensure_sd_card(bool blocking) {
  if(g_sd_mounted) {
    return true;
  }

  if(!g_spi2_initialised) {
    SPI2.begin(
        M5.getPin(m5::pin_name_t::sd_spi_sclk),
        M5.getPin(m5::pin_name_t::sd_spi_miso),
        M5.getPin(m5::pin_name_t::sd_spi_mosi),
        M5.getPin(m5::pin_name_t::sd_spi_ss));
    g_spi2_initialised = true;
  }

  const int cs_pin = M5.getPin(m5::pin_name_t::sd_spi_ss);
  uint32_t attempt_counter = 0;

  while(true) {
    if(SD.begin(cs_pin, SPI2)) {
      g_sd_mounted = true;
      Serial.println("SD card mounted");
      return true;
    }

    if(!blocking) {
      return false;
    }

    attempt_counter++;
    if((attempt_counter % 1000) == 0) {
      Serial.println("Waiting for SD card...");
    }
    delay(1);
  }
}


void debugPrint(const char* str) {
  M5Cardputer.Display.clearDisplay();
  M5Cardputer.Display.drawString(str, 0, 0);
  Serial.println(str);
#if DEBUG_DELAY
  delay(500);
#endif
}

static void configure_performance_profile() {
#if defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
  esp_pm_config_t pm_config = {
      .max_freq_mhz = 240,
      .min_freq_mhz = 240,
      .light_sleep_enable = false
  };
  esp_err_t err = esp_pm_configure(&pm_config);
  if(err != ESP_OK) {
    Serial.printf("esp_pm_configure failed (%d)\n", static_cast<int>(err));
  }
#endif
  setCpuFrequencyMhz(240);
}

static uint8_t* alloc_gb_buffer(size_t bytes, const char *label, bool prefer_internal_first = false) {
  uint8_t *ptr = nullptr;

  if(prefer_internal_first) {
    ptr = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(ptr == nullptr) {
      ptr = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
  } else {
    ptr = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(ptr == nullptr) {
      ptr = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
  }

  if(ptr == nullptr) {
    ptr = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
  }

  if(ptr == nullptr) {
    Serial.printf("Failed to allocate %s (%u bytes)\n", label, (unsigned)bytes);
    debugPrint("Alloc fail");
    delay(2000);
    return nullptr;
  }

  memset(ptr, 0x00, bytes);

  const char *region = esp_ptr_external_ram(ptr) ? "PSRAM" : "internal";
  Serial.printf("Allocated %s (%u bytes) in %s memory\n", label, (unsigned)bytes, region);

  return ptr;
}

static inline uint16_t rgb888_to_rgb565(uint32_t colour) {
  uint16_t r = (colour >> 16) & 0xFF;
  uint16_t g = (colour >> 8) & 0xFF;
  uint16_t b = colour & 0xFF;
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline uint16_t rgb888_to_gb555(uint32_t colour) {
  const uint16_t r = (colour >> 16) & 0xFF;
  const uint16_t g = (colour >> 8) & 0xFF;
  const uint16_t b = colour & 0xFF;
  return (uint16_t)(((r >> 3) & 0x1F) |
                    (((g >> 3) & 0x1F) << 5) |
                    (((b >> 3) & 0x1F) << 10));
}

static inline uint32_t framebuffer_hash_step(uint32_t hash, uint16_t value) {
  hash ^= value;
  hash *= 16777619u;
  return hash;
}


struct priv_t
{
  RomCache rom_cache;

  uint8_t *cart_ram;
  PaletteState palette;
  RomSource rom_source;
  const EmbeddedRomEntry *embedded_rom_entry;
  const uint8_t *embedded_rom;
  size_t embedded_rom_size;

  bool rom_is_cgb;
  bool rom_is_cgb_only;
  uint8_t rom_cgb_flag;


  uint16_t *framebuffers[2];
  uint32_t framebuffer_row_hash[2][LCD_HEIGHT];
  uint8_t framebuffer_row_dirty[2][LCD_HEIGHT];
  uint8_t write_fb_index;
  bool single_buffer_mode;
  QueueHandle_t frame_queue;
  SemaphoreHandle_t frame_buffer_free[2];
  size_t cart_ram_size;
  bool cart_ram_dirty;
  bool cart_ram_loaded;
  uint32_t cart_ram_last_flush_ms;
  char cart_save_path[MAX_PATH_LEN];
  bool cart_save_path_valid;
  bool cart_save_write_failed;
  bool mbc7_eeprom_dirty;
  bool mbc7_eeprom_loaded;
  uint32_t mbc7_last_flush_ms;
  char mbc7_save_path[MAX_PATH_LEN];
  bool mbc7_save_path_valid;
  bool mbc7_save_write_failed;
  char sd_rom_path[MAX_PATH_LEN];
  bool sd_rom_path_valid;
};

static struct gb_s gb;
static struct priv_t priv;
static TaskHandle_t render_task_handle = nullptr;
#if ENABLE_SOUND
static TaskHandle_t audio_task_handle = nullptr;
#endif

static constexpr const char *PRINTER_OUTPUT_DIR = "/printer";
static constexpr size_t PRINTER_TILE_BYTES = 16;
static constexpr size_t PRINTER_TILES_PER_ROW = 20;
static constexpr size_t PRINTER_PIXEL_ROWS_PER_TILE = 8;
static constexpr size_t PRINTER_IMAGE_WIDTH = 160;
static constexpr size_t PRINTER_RX_FIFO_SIZE = 16;
static constexpr uint8_t PRINTER_RX_FIFO_MASK = static_cast<uint8_t>(PRINTER_RX_FIFO_SIZE - 1);
static constexpr size_t PRINTER_MAX_IMAGE_BYTES = 32768;

struct GameBoyPrinterState {
  enum class Stage : uint8_t {
    WaitSync1 = 0,
    WaitSync2,
    Header,
    Data,
    ChecksumLow,
    ChecksumHigh
  };

  Stage stage;
  uint8_t header_index;
  uint16_t expected_length;
  uint16_t data_received;
  uint32_t checksum_accum;
  uint16_t checksum_expected;
  uint8_t command;
  uint8_t compression_flag;
  std::vector<uint8_t> packet_data;
  std::vector<uint8_t> image_data;
  std::vector<uint8_t> decompress_buffer;
  uint8_t rx_fifo[PRINTER_RX_FIFO_SIZE];
  uint8_t rx_head;
  uint8_t rx_tail;
  uint8_t status_lo;
  uint8_t status_hi;
  bool busy;
  uint8_t last_response;
  uint32_t job_counter;

  GameBoyPrinterState()
      : stage(Stage::WaitSync1),
        header_index(0),
        expected_length(0),
        data_received(0),
        checksum_accum(0),
        checksum_expected(0),
        command(0),
        compression_flag(0),
        packet_data(),
        image_data(),
        decompress_buffer(),
        rx_fifo{0},
        rx_head(0),
        rx_tail(0),
        status_lo(0),
        status_hi(0),
        busy(false),
        last_response(0),
        job_counter(0) {
    image_data.reserve(PRINTER_MAX_IMAGE_BYTES);
  }

  void reset(bool reset_jobs = false) {
    stage = Stage::WaitSync1;
    header_index = 0;
    expected_length = 0;
    data_received = 0;
    checksum_accum = 0;
    checksum_expected = 0;
    command = 0;
    compression_flag = 0;
    packet_data.clear();
    busy = false;
    status_lo = 0;
    status_hi = 0;
    rx_head = 0;
    rx_tail = 0;
    last_response = 0;
    if(reset_jobs) {
      job_counter = 0;
    }
    image_data.clear();
    decompress_buffer.clear();
  }

  uint8_t dequeueRx() {
    if(rx_head == rx_tail) {
      return 0x00;
    }
    uint8_t value = rx_fifo[rx_head];
    rx_head = static_cast<uint8_t>((rx_head + 1) & PRINTER_RX_FIFO_MASK);
    return value;
  }

  void enqueueRx(uint8_t value) {
    uint8_t next = static_cast<uint8_t>((rx_tail + 1) & PRINTER_RX_FIFO_MASK);
    if(next == rx_head) {
      return;
    }
    rx_fifo[rx_tail] = value;
    rx_tail = next;
  }

  uint8_t onTransmit(uint8_t tx_byte) {
    uint8_t response = dequeueRx();
    last_response = response;
    parseByte(tx_byte);
    return response;
  }

  uint8_t lastResponse() const {
    return last_response;
  }

  void parseByte(uint8_t byte) {
    switch(stage) {
      case Stage::WaitSync1:
        if(byte == 0x88) {
          stage = Stage::WaitSync2;
        }
        break;
      case Stage::WaitSync2:
        if(byte == 0x33) {
          stage = Stage::Header;
          header_index = 0;
          checksum_accum = 0;
          expected_length = 0;
          data_received = 0;
          packet_data.clear();
        } else if(byte != 0x88) {
          stage = Stage::WaitSync1;
        }
        break;
      case Stage::Header:
        switch(header_index) {
          case 0:
            command = byte;
            checksum_accum = byte;
            header_index++;
            break;
          case 1:
            compression_flag = byte;
            checksum_accum += byte;
            header_index++;
            break;
          case 2:
            expected_length = byte;
            checksum_accum += byte;
            header_index++;
            break;
          case 3:
            expected_length |= static_cast<uint16_t>(byte) << 8;
            checksum_accum += byte;
            header_index = 0;
            data_received = 0;
            if(expected_length > 0) {
              const uint16_t reserve_len = command == 0x02 && expected_length > 0x280 ? 0x280 : expected_length;
              packet_data.reserve(reserve_len);
              stage = Stage::Data;
            } else {
              stage = Stage::ChecksumLow;
            }
            break;
          default:
            stage = Stage::WaitSync1;
            break;
        }
        break;
      case Stage::Data:
        if(packet_data.size() < PRINTER_MAX_IMAGE_BYTES) {
          packet_data.push_back(byte);
        }
        checksum_accum += byte;
        data_received++;
        if(data_received >= expected_length) {
          stage = Stage::ChecksumLow;
        }
        break;
      case Stage::ChecksumLow:
        checksum_expected = byte;
        stage = Stage::ChecksumHigh;
        break;
      case Stage::ChecksumHigh: {
        checksum_expected |= static_cast<uint16_t>(byte) << 8;
        const uint16_t computed = static_cast<uint16_t>(checksum_accum & 0xFFFFu);
        if(computed != checksum_expected) {
          Serial.printf("Game Boy Printer: checksum mismatch cmd=0x%02X len=%u expected=0x%04X got=0x%04X\n",
                        command,
                        static_cast<unsigned>(expected_length),
                        static_cast<unsigned>(checksum_expected),
                        static_cast<unsigned>(computed));
        } else {
          processPacket();
        }
        stage = Stage::WaitSync1;
        queueStatusAck();
        break;
      }
    }
  }

  void queueStatusAck() {
    const uint8_t busy_flag = busy ? 0x04 : 0x00;
    enqueueRx(0x00);
    enqueueRx(static_cast<uint8_t>(status_lo | busy_flag));
    enqueueRx(status_hi);
    enqueueRx(0x00);
  }

  void processPacket() {
    switch(command) {
      case 0x00:
        image_data.clear();
        status_lo = 0x00;
        status_hi = 0x00;
        busy = false;
        break;
      case 0x01:

        break;
      case 0x02:
        handleDataPacket();
        break;
      case 0x04:
        handlePrintPacket();
        break;
      default:

        break;
    }
  }

  void handleDataPacket() {
    decompressPayload();
    if(decompress_buffer.empty()) {
      return;
    }
    if(image_data.size() + decompress_buffer.size() > PRINTER_MAX_IMAGE_BYTES) {
      Serial.println("Game Boy Printer: image buffer overflow, discarding data");
      image_data.clear();
      return;
    }
    image_data.insert(image_data.end(), decompress_buffer.begin(), decompress_buffer.end());
  }

  void handlePrintPacket() {
    busy = true;
    if(saveBufferedImage()) {
      image_data.clear();
      status_lo = 0x00;
      status_hi = 0x00;
    } else {
      status_hi |= 0x01;
    }
    busy = false;
  }

  void decompressPayload() {
    decompress_buffer.clear();
    if(packet_data.empty()) {
      return;
    }
    const bool compressed = (compression_flag & 0x01u) != 0;
    if(!compressed) {
      decompress_buffer.insert(decompress_buffer.end(), packet_data.begin(), packet_data.end());
      return;
    }

    size_t index = 0;
    while(index < packet_data.size()) {
      uint8_t control = packet_data[index++];
      if(control & 0x80) {
        const size_t count = static_cast<size_t>((control & 0x7Fu) + 3u);
        if(index >= packet_data.size()) {
          break;
        }
        const uint8_t value = packet_data[index++];
        if(decompress_buffer.size() + count > PRINTER_MAX_IMAGE_BYTES) {
          decompress_buffer.clear();
          Serial.println("Game Boy Printer: decompressed data exceeds limits");
          return;
        }
        decompress_buffer.insert(decompress_buffer.end(), count, value);
      } else {
        size_t count = static_cast<size_t>(control) + 1u;
        if(index + count > packet_data.size()) {
          count = packet_data.size() - index;
        }
        if(decompress_buffer.size() + count > PRINTER_MAX_IMAGE_BYTES) {
          decompress_buffer.clear();
          Serial.println("Game Boy Printer: decompressed data exceeds limits");
          return;
        }
        decompress_buffer.insert(decompress_buffer.end(), packet_data.begin() + index, packet_data.begin() + index + count);
        index += count;
      }
    }
  }

  bool ensureOutputPath(char *path_buffer, size_t buffer_len) {
    if(!ensure_sd_card(true)) {
      Serial.println("Game Boy Printer: SD card unavailable");
      return false;
    }
    if(!SD.exists(PRINTER_OUTPUT_DIR)) {
      if(!SD.mkdir(PRINTER_OUTPUT_DIR)) {
        Serial.println("Game Boy Printer: failed to create output directory");
        return false;
      }
    }
    for(uint32_t attempt = job_counter; attempt < job_counter + 10000; ++attempt) {
      char candidate[64];
      snprintf(candidate, sizeof(candidate), "%s/print_%04u.bmp", PRINTER_OUTPUT_DIR, static_cast<unsigned>(attempt & 0xFFFFu));
      if(!SD.exists(candidate)) {
        if(strlen(candidate) + 1 > buffer_len) {
          return false;
        }
        strncpy(path_buffer, candidate, buffer_len);
        path_buffer[buffer_len - 1] = '\0';
        job_counter = attempt + 1;
        return true;
      }
    }
    Serial.println("Game Boy Printer: exhausted filename attempts");
    return false;
  }

  bool saveBufferedImage() {
    if(image_data.empty()) {
      Serial.println("Game Boy Printer: no image data to save");
      return false;
    }

    const size_t total_bytes = image_data.size();
    if(total_bytes < PRINTER_TILES_PER_ROW * PRINTER_TILE_BYTES) {
      Serial.println("Game Boy Printer: insufficient image data");
      return false;
    }

    const size_t tiles_total = total_bytes / PRINTER_TILE_BYTES;
    if(tiles_total < PRINTER_TILES_PER_ROW) {
      Serial.println("Game Boy Printer: tile data incomplete");
      return false;
    }

    const size_t tile_rows = tiles_total / PRINTER_TILES_PER_ROW;
    if(tile_rows == 0) {
      Serial.println("Game Boy Printer: zero tile rows");
      return false;
    }

    const size_t height = tile_rows * PRINTER_PIXEL_ROWS_PER_TILE;
    const size_t row_stride = ((PRINTER_IMAGE_WIDTH * 3u) + 3u) & ~3u;
    std::vector<uint8_t> bmp_buffer(row_stride * height, 0xFF);
    static const uint8_t GREY_LEVELS[4] = { 0xFF, 0xAA, 0x55, 0x00 };

    for(size_t tile_row = 0; tile_row < tile_rows; ++tile_row) {
      for(size_t row = 0; row < PRINTER_PIXEL_ROWS_PER_TILE; ++row) {
        const size_t y = tile_row * PRINTER_PIXEL_ROWS_PER_TILE + row;
        if(y >= height) {
          break;
        }
        const size_t dst_row = height - 1 - y;
        uint8_t *dst = bmp_buffer.data() + dst_row * row_stride;
        for(size_t tile_col = 0; tile_col < PRINTER_TILES_PER_ROW; ++tile_col) {
          const size_t tile_index = tile_row * PRINTER_TILES_PER_ROW + tile_col;
          if(tile_index * PRINTER_TILE_BYTES + (row * 2 + 1) >= image_data.size()) {
            break;
          }
          const uint8_t *tile_base = &image_data[tile_index * PRINTER_TILE_BYTES];
          const uint8_t lo = tile_base[row * 2];
          const uint8_t hi = tile_base[row * 2 + 1];
          for(uint8_t bit = 0; bit < 8; ++bit) {
            const uint8_t colour_index = static_cast<uint8_t>(((hi >> (7 - bit)) & 0x01u) << 1) |
                                         static_cast<uint8_t>((lo >> (7 - bit)) & 0x01u);
            const uint8_t grey = GREY_LEVELS[colour_index & 0x03u];
            const size_t x = tile_col * 8u + bit;
            const size_t dst_index = x * 3u;
            if(dst_index + 2 < row_stride) {
              dst[dst_index + 0] = grey;
              dst[dst_index + 1] = grey;
              dst[dst_index + 2] = grey;
            }
          }
        }
      }
    }

    char path[64];
    if(!ensureOutputPath(path, sizeof(path))) {
      return false;
    }

    struct __attribute__((packed)) BmpFileHeader {
      uint16_t type;
      uint32_t size;
      uint16_t reserved1;
      uint16_t reserved2;
      uint32_t offset;
    } file_header;

    struct __attribute__((packed)) BmpInfoHeader {
      uint32_t size;
      int32_t width;
      int32_t height;
      uint16_t planes;
      uint16_t bit_count;
      uint32_t compression;
      uint32_t image_size;
      int32_t x_ppm;
      int32_t y_ppm;
      uint32_t colours_used;
      uint32_t colours_important;
    } info_header;

    const uint32_t headers_size = sizeof(file_header) + sizeof(info_header);
    file_header.type = 0x4D42;
    file_header.size = headers_size + static_cast<uint32_t>(bmp_buffer.size());
    file_header.reserved1 = 0;
    file_header.reserved2 = 0;
    file_header.offset = headers_size;

    info_header.size = sizeof(info_header);
    info_header.width = static_cast<int32_t>(PRINTER_IMAGE_WIDTH);
    info_header.height = static_cast<int32_t>(height);
    info_header.planes = 1;
    info_header.bit_count = 24;
    info_header.compression = 0;
    info_header.image_size = static_cast<uint32_t>(bmp_buffer.size());
    info_header.x_ppm = 2835;
    info_header.y_ppm = 2835;
    info_header.colours_used = 0;
    info_header.colours_important = 0;

    File file = SD.open(path, FILE_WRITE);
    if(!file) {
      Serial.printf("Game Boy Printer: failed to open %s for write\n", path);
      return false;
    }

    bool ok = true;
    if(file.write(reinterpret_cast<uint8_t *>(&file_header), sizeof(file_header)) != sizeof(file_header)) {
      ok = false;
    }
    if(ok && file.write(reinterpret_cast<uint8_t *>(&info_header), sizeof(info_header)) != sizeof(info_header)) {
      ok = false;
    }
    if(ok && file.write(bmp_buffer.data(), bmp_buffer.size()) != static_cast<int>(bmp_buffer.size())) {
      ok = false;
    }
    file.close();

    if(!ok) {
      Serial.printf("Game Boy Printer: failed to write image %s\n", path);
      SD.remove(path);
      return false;
    }

    Serial.printf("Game Boy Printer: saved image %s (%ux%u)\n",
                  path,
                  static_cast<unsigned>(PRINTER_IMAGE_WIDTH),
                  static_cast<unsigned>(height));
    return true;
  }
};

static GameBoyPrinterState g_printer;

static void gb_printer_serial_tx(struct gb_s *gb, const uint8_t tx);
static enum gb_serial_rx_ret_e gb_printer_serial_rx(struct gb_s *gb, uint8_t *rx);

static void apply_default_button_mapping() {
  memcpy(g_settings.button_mapping,
         DEFAULT_JOYPAD_KEYMAP,
         sizeof(DEFAULT_JOYPAD_KEYMAP));
}

static void apply_settings_constraints() {
  const uint8_t min_banks = 1;
  if(g_settings.rom_cache_banks < min_banks) {
    g_settings.rom_cache_banks = min_banks;
  }
  size_t preferred = rom_cache_preferred_bank_limit();
  if(preferred < min_banks) {
    preferred = min_banks;
  }
  if(preferred > ROM_CACHE_BANK_MAX) {
    preferred = ROM_CACHE_BANK_MAX;
  }
  if(g_settings.rom_cache_banks > preferred) {
    g_settings.rom_cache_banks = static_cast<uint8_t>(preferred);
  }
  if(g_settings.master_volume > 255) {
    g_settings.master_volume = 255;
  }
  if(g_settings.frame_skip_mode >= FRAME_SKIP_MODE_COUNT) {
    g_settings.frame_skip_mode = static_cast<uint8_t>(FRAME_SKIP_MODE_AUTO);
  }
}

static bool ensure_settings_dir() {
  if(!g_sd_mounted) {
    return false;
  }
  if(SD.exists(SETTINGS_DIR)) {
    return true;
  }
  if(SD.mkdir(SETTINGS_DIR)) {
    return true;
  }
  Serial.println("Failed to create settings directory on SD");
  return false;
}

static bool save_settings_to_sd() {
  if(!g_sd_mounted) {
    Serial.println("Settings save skipped: SD not mounted");
    return false;
  }
  if(!ensure_settings_dir()) {
    return false;
  }

  SD.remove(SETTINGS_FILE_PATH);
  File file = SD.open(SETTINGS_FILE_PATH, FILE_WRITE);
  if(!file) {
    Serial.println("Failed to open settings file for write");
    return false;
  }

  file.printf("# Cardputer firmware settings\n");
  file.printf("version=%u\n", static_cast<unsigned>(SETTINGS_VERSION));
  file.printf("audio=%u\n", g_settings.audio_enabled ? 1u : 0u);
  file.printf("bootstrap=%u\n", g_settings.cgb_bootstrap_palettes ? 1u : 0u);
  file.printf("cache=%u\n", static_cast<unsigned>(g_settings.rom_cache_banks));
  file.printf("volume=%u\n", static_cast<unsigned>(g_settings.master_volume));
  file.printf("frame_skip=%u\n", static_cast<unsigned>(g_settings.frame_skip_mode));
  file.print("keys=");
  for(size_t i = 0; i < JOYPAD_BUTTON_COUNT; ++i) {
    file.printf("0x%02X", static_cast<unsigned>(g_settings.button_mapping[i]));
    if(i + 1 < JOYPAD_BUTTON_COUNT) {
      file.print(",");
    }
  }
  file.print("\n");
  file.close();

  g_settings_dirty = false;
  g_settings_loaded = true;
  Serial.println("Settings saved to SD");
  return true;
}

static bool load_settings_from_sd() {
  if(!g_sd_mounted) {
    return false;
  }

  File file = SD.open(SETTINGS_FILE_PATH, FILE_READ);
  if(!file) {
    Serial.println("Settings file not found; using defaults");
    return false;
  }

  bool version_seen = false;
  bool volume_seen = false;
  bool upgrade_needed = false;
  uint8_t loaded_keys[JOYPAD_BUTTON_COUNT] = {0};

  while(file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
  if(line.length() == 0 || line.startsWith("#")) {
      continue;
    }

    int separator = line.indexOf('=');
    if(separator <= 0) {
      continue;
    }

    String key = line.substring(0, separator);
    String value = line.substring(separator + 1);
    key.trim();
    value.trim();
    key.toLowerCase();

    if(key == "version") {
      version_seen = true;
      long parsed_version = value.toInt();
      if(parsed_version < SETTINGS_VERSION) {
        upgrade_needed = true;
      }
    } else if(key == "audio") {
      g_settings.audio_enabled = (value.toInt() != 0);
    } else if(key == "bootstrap") {
      g_settings.cgb_bootstrap_palettes = (value.toInt() != 0);
    } else if(key == "cache") {
      long banks = value.toInt();
      if(banks >= 1 && banks <= ROM_CACHE_BANK_MAX) {
        g_settings.rom_cache_banks = static_cast<uint8_t>(banks);
      }
    } else if(key == "volume") {
      long vol = value.toInt();
      if(vol < 0) {
        vol = 0;
      } else if(vol > 255) {
        vol = 255;
      }
      g_settings.master_volume = static_cast<uint8_t>(vol);
      volume_seen = true;
    } else if(key == "frame_skip") {
      long parsed = value.toInt();
      if(parsed < 0) {
        parsed = FRAME_SKIP_MODE_AUTO;
      }
      if(parsed >= FRAME_SKIP_MODE_COUNT) {
        parsed = FRAME_SKIP_MODE_AUTO;
      }
      g_settings.frame_skip_mode = static_cast<uint8_t>(parsed);
    } else if(key == "keys") {
      size_t index = 0;
      int start = 0;
      while(index < JOYPAD_BUTTON_COUNT && start < value.length()) {
        int comma = value.indexOf(',', start);
        String token = (comma >= 0) ? value.substring(start, comma) : value.substring(start);
        token.trim();
        if(token.length() == 0) {
          break;
        }

        char *endptr = nullptr;
        long parsed = strtol(token.c_str(), &endptr, 0);
        if(endptr == token.c_str()) {
          break;
        }

        loaded_keys[index++] = static_cast<uint8_t>(parsed & 0xFF);
        if(comma < 0) {
          break;
        }
        start = comma + 1;
      }
      if(index == JOYPAD_BUTTON_COUNT) {
        memcpy(g_settings.button_mapping, loaded_keys, sizeof(loaded_keys));
      }
    }
  }

  file.close();

  if(!version_seen || !volume_seen) {
    upgrade_needed = true;
    if(!volume_seen) {
      g_settings.master_volume = DEFAULT_MASTER_VOLUME;
    }
  }

  apply_settings_constraints();
  g_settings_loaded = true;
  g_settings_dirty = upgrade_needed;

  if(upgrade_needed) {
    save_settings_to_sd();
  }

  Serial.println("Settings loaded from SD");
  return true;
}

static void reset_save_state(struct priv_t *priv) {
  if(priv == nullptr) {
    return;
  }

  priv->cart_ram_size = 0;
  priv->cart_ram_dirty = false;
  priv->cart_ram_loaded = false;
  priv->cart_ram_last_flush_ms = 0;
  priv->cart_save_path[0] = '\0';
  priv->cart_save_path_valid = false;
  priv->cart_save_write_failed = false;
  priv->mbc7_eeprom_dirty = false;
  priv->mbc7_eeprom_loaded = false;
  priv->mbc7_last_flush_ms = 0;
  priv->mbc7_save_path[0] = '\0';
  priv->mbc7_save_path_valid = false;
  priv->mbc7_save_write_failed = false;
  priv->sd_rom_path[0] = '\0';
  priv->sd_rom_path_valid = false;
}

static void set_sd_rom_path(struct priv_t *priv, const char *path) {
  if(priv == nullptr) {
    return;
  }
  if(path == nullptr) {
    priv->sd_rom_path[0] = '\0';
    priv->sd_rom_path_valid = false;
    return;
  }
  strncpy(priv->sd_rom_path, path, sizeof(priv->sd_rom_path) - 1);
  priv->sd_rom_path[sizeof(priv->sd_rom_path) - 1] = '\0';
  priv->sd_rom_path_valid = priv->sd_rom_path[0] != '\0';
}

static void clear_sd_rom_path(struct priv_t *priv) {
  if(priv == nullptr) {
    return;
  }
  priv->sd_rom_path[0] = '\0';
  priv->sd_rom_path_valid = false;
}

static bool ensure_saves_dir() {
  if(!g_sd_mounted) {
    return false;
  }
  if(SD.exists(SAVES_DIR)) {
    return true;
  }
  if(SD.mkdir(SAVES_DIR)) {
    return true;
  }
  Serial.println("Failed to create saves directory on SD");
  return false;
}

static void sanitise_identifier(const char *input, char *output, size_t output_len) {
  if(output == nullptr || output_len == 0) {
    return;
  }
  output[0] = '\0';
  if(input == nullptr) {
    return;
  }

  size_t out_index = 0;
  for(const char *ptr = input; *ptr != '\0' && (out_index + 1) < output_len; ++ptr) {
    unsigned char ch = static_cast<unsigned char>(*ptr);
    if(std::isalnum(ch)) {
      output[out_index++] = static_cast<char>(std::tolower(ch));
    } else if(ch == ' ' || ch == '-' || ch == '_' || ch == '.') {
      if(out_index > 0 && output[out_index - 1] == '_') {
        continue;
      }
      output[out_index++] = '_';
    }
  }

  while(out_index > 0 && output[out_index - 1] == '_') {
    --out_index;
  }
  output[out_index] = '\0';
}

static bool build_save_path_for_rom(const char *rom_path,
                                    const char *extension,
                                    char *out,
                                    size_t out_len) {
  if(rom_path == nullptr || extension == nullptr || out == nullptr || out_len == 0) {
    return false;
  }

  char buffer[MAX_PATH_LEN];
  strncpy(buffer, rom_path, sizeof(buffer) - 1);
  buffer[sizeof(buffer) - 1] = '\0';

  char *last_slash = strrchr(buffer, '/');
  char *last_dot = strrchr(buffer, '.');
  if(last_dot != nullptr && (last_slash == nullptr || last_dot > last_slash)) {
    *last_dot = '\0';
  }

  const size_t suffix_len = strlen(extension);
  const size_t base_len = strlen(buffer);
  if(base_len + suffix_len + 1 > out_len) {
    return false;
  }

  snprintf(out, out_len, "%s%s", buffer, extension);
  return true;
}

static bool derive_save_paths(struct priv_t *priv, bool uses_mbc7) {
  if(priv == nullptr) {
    return false;
  }

  priv->cart_save_path_valid = false;
  priv->cart_save_write_failed = false;
  priv->mbc7_save_path_valid = false;
  priv->mbc7_save_write_failed = false;

  bool any_valid = false;

  if(priv->rom_source == RomSource::SdCard && priv->sd_rom_path_valid) {
    if(build_save_path_for_rom(priv->sd_rom_path,
                               SAVE_FILE_EXTENSION,
                               priv->cart_save_path,
                               sizeof(priv->cart_save_path))) {
      priv->cart_save_path_valid = true;
      any_valid = true;
    }
    if(uses_mbc7 && build_save_path_for_rom(priv->sd_rom_path,
                                            MBC7_FILE_EXTENSION,
                                            priv->mbc7_save_path,
                                            sizeof(priv->mbc7_save_path))) {
      priv->mbc7_save_path_valid = true;
      any_valid = true;
    }
    return any_valid;
  }

  if(priv->rom_source == RomSource::Embedded && priv->embedded_rom_entry != nullptr) {
    const EmbeddedRomEntry *entry = priv->embedded_rom_entry;
    const char *identifier_source = nullptr;
    if(entry->id != nullptr && entry->id[0] != '\0') {
      identifier_source = entry->id;
    } else if(entry->name != nullptr && entry->name[0] != '\0') {
      identifier_source = entry->name;
    }

    char identifier[64];
    sanitise_identifier(identifier_source, identifier, sizeof(identifier));
    if(identifier[0] == '\0') {
      strncpy(identifier, "embedded", sizeof(identifier) - 1);
      identifier[sizeof(identifier) - 1] = '\0';
    }

    int written = snprintf(priv->cart_save_path,
                           sizeof(priv->cart_save_path),
                           "%s/%s%s",
                           SAVES_DIR,
                           identifier,
                           SAVE_FILE_EXTENSION);
    if(written > 0 && static_cast<size_t>(written) < sizeof(priv->cart_save_path)) {
      priv->cart_save_path_valid = true;
      any_valid = true;
    }

    if(uses_mbc7) {
      written = snprintf(priv->mbc7_save_path,
                         sizeof(priv->mbc7_save_path),
                         "%s/%s%s",
                         SAVES_DIR,
                         identifier,
                         MBC7_FILE_EXTENSION);
      if(written > 0 && static_cast<size_t>(written) < sizeof(priv->mbc7_save_path)) {
        priv->mbc7_save_path_valid = true;
        any_valid = true;
      }
    }

    return any_valid;
  }

  return any_valid;
}

static bool load_cart_ram_from_sd(struct priv_t *priv) {
  if(priv == nullptr || priv->cart_ram == nullptr || priv->cart_ram_size == 0) {
    return false;
  }
  if(!priv->cart_save_path_valid || !g_sd_mounted) {
    return false;
  }

  File file = SD.open(priv->cart_save_path, FILE_READ);
  if(!file) {
    Serial.printf("Save file not found: %s\n", priv->cart_save_path);
    return false;
  }

  size_t total_read = 0;
  while(total_read < priv->cart_ram_size && file.available()) {
    const size_t remaining = priv->cart_ram_size - total_read;
    const size_t chunk = remaining > 512 ? 512 : remaining;
    int read_bytes = file.read(priv->cart_ram + total_read, chunk);
    if(read_bytes <= 0) {
      break;
    }
    total_read += static_cast<size_t>(read_bytes);
  }
  file.close();

  if(total_read > 0 && total_read < priv->cart_ram_size) {
    memset(priv->cart_ram + total_read, 0xFF, priv->cart_ram_size - total_read);
  }

  if(total_read > 0) {
    Serial.printf("Loaded %u bytes of save RAM from %s\n",
                  static_cast<unsigned>(total_read),
                  priv->cart_save_path);
    priv->cart_ram_loaded = true;
    priv->cart_ram_dirty = false;
    return true;
  }

  Serial.printf("Save file read returned 0 bytes: %s\n", priv->cart_save_path);
  return false;
}

static bool save_cart_ram_to_sd(const struct priv_t *priv) {
  if(priv == nullptr || priv->cart_ram == nullptr || priv->cart_ram_size == 0) {
    return false;
  }
  if(!priv->cart_save_path_valid || !g_sd_mounted) {
    return false;
  }

  if(priv->rom_source == RomSource::Embedded && !ensure_saves_dir()) {
    return false;
  }

  SD.remove(priv->cart_save_path);
  File file = SD.open(priv->cart_save_path, FILE_WRITE);
  if(!file) {
    Serial.printf("Failed to open save file for write: %s\n", priv->cart_save_path);
    return false;
  }

  size_t total_written = 0;
  while(total_written < priv->cart_ram_size) {
    const size_t remaining = priv->cart_ram_size - total_written;
    const size_t chunk = remaining > 512 ? 512 : remaining;
    int written = file.write(priv->cart_ram + total_written, chunk);
    if(written != static_cast<int>(chunk)) {
      Serial.printf("Save write truncated (%d/%u) for %s\n",
                    written,
                    static_cast<unsigned>(chunk),
                    priv->cart_save_path);
      file.close();
      return false;
    }
    total_written += chunk;
  }
  file.flush();
  file.close();

  Serial.printf("Saved %u bytes of cart RAM to %s\n",
                static_cast<unsigned>(total_written),
                priv->cart_save_path);
  return true;
}

#if ENABLE_MBC7
static bool load_mbc7_eeprom_from_sd(struct priv_t *priv, struct gb_s *gb) {
  if(priv == nullptr || gb == nullptr) {
    return false;
  }
  if(!priv->mbc7_save_path_valid || !g_sd_mounted) {
    return false;
  }

  File file = SD.open(priv->mbc7_save_path, FILE_READ);
  if(!file) {
    Serial.printf("MBC7 EEPROM file not found: %s\n", priv->mbc7_save_path);
    return false;
  }

  uint8_t buffer[MBC7_EEPROM_RAW_SIZE];
  size_t total_read = file.read(buffer, sizeof(buffer));
  file.close();

  if(total_read == 0) {
    Serial.printf("MBC7 EEPROM read returned 0 bytes: %s\n", priv->mbc7_save_path);
    return false;
  }

  const size_t word_count = std::min(static_cast<size_t>(total_read / 2), MBC7_EEPROM_WORD_COUNT);
  for(size_t i = 0; i < word_count; ++i) {
    const size_t idx = i * 2;
    uint16_t value = static_cast<uint16_t>(buffer[idx]);
    if(idx + 1 < total_read) {
      value |= static_cast<uint16_t>(buffer[idx + 1]) << 8;
    }
    gb->mbc7.eeprom.data[i] = value;
  }
  for(size_t i = word_count; i < MBC7_EEPROM_WORD_COUNT; ++i) {
    gb->mbc7.eeprom.data[i] = 0xFFFF;
  }

  Serial.printf("Loaded MBC7 EEPROM (%u bytes) from %s\n",
                static_cast<unsigned>(total_read),
                priv->mbc7_save_path);
  priv->mbc7_eeprom_loaded = true;
  priv->mbc7_eeprom_dirty = false;
  return true;
}

static bool save_mbc7_eeprom_to_sd(const struct priv_t *priv, const struct gb_s *gb) {
  if(priv == nullptr || gb == nullptr) {
    return false;
  }
  if(!priv->mbc7_save_path_valid || !g_sd_mounted) {
    return false;
  }

  if(priv->rom_source == RomSource::Embedded && !ensure_saves_dir()) {
    return false;
  }

  SD.remove(priv->mbc7_save_path);
  File file = SD.open(priv->mbc7_save_path, FILE_WRITE);
  if(!file) {
    Serial.printf("Failed to open MBC7 EEPROM file for write: %s\n",
                  priv->mbc7_save_path);
    return false;
  }

  uint8_t buffer[MBC7_EEPROM_RAW_SIZE];
  for(size_t i = 0; i < MBC7_EEPROM_WORD_COUNT; ++i) {
    const uint16_t value = gb->mbc7.eeprom.data[i];
    buffer[i * 2] = static_cast<uint8_t>(value & 0xFF);
    buffer[i * 2 + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  }

  size_t written = file.write(buffer, sizeof(buffer));
  file.flush();
  file.close();

  if(written != sizeof(buffer)) {
    Serial.printf("MBC7 EEPROM write truncated (%u/%u) for %s\n",
                  static_cast<unsigned>(written),
                  static_cast<unsigned>(sizeof(buffer)),
                  priv->mbc7_save_path);
    return false;
  }

  Serial.printf("Saved MBC7 EEPROM (%u bytes) to %s\n",
                static_cast<unsigned>(written),
                priv->mbc7_save_path);
  return true;
}
#else
static bool load_mbc7_eeprom_from_sd(struct priv_t *, struct gb_s *) {
  return false;
}

static bool save_mbc7_eeprom_to_sd(const struct priv_t *, const struct gb_s *) {
  return false;
}
#endif

static void adjust_master_volume(int delta, bool persist, bool announce) {
  int new_volume = static_cast<int>(g_settings.master_volume) + delta;
  if(new_volume < 0) {
    new_volume = 0;
  } else if(new_volume > 255) {
    new_volume = 255;
  }

  if(new_volume == g_settings.master_volume) {
    return;
  }

  g_settings.master_volume = static_cast<uint8_t>(new_volume);
  if(announce) {
    Serial.printf("Volume: %u\n", static_cast<unsigned>(g_settings.master_volume));
  }

  apply_speaker_volume();

  if(persist) {
    save_settings_to_sd();
  } else {
    g_settings_dirty = true;
  }
}

static void handle_volume_keys(const Keyboard_Class::KeysState &status) {
  bool up_pressed = false;
  bool down_pressed = false;

  for(auto key : status.word) {
    switch(key) {
      case '=':
      case '+':
        up_pressed = true;
        break;
      case '-':
      case '_':
        down_pressed = true;
        break;
      default:
        break;
    }
  }

  static bool previous_up = false;
  static bool previous_down = false;

  if(up_pressed && !previous_up) {
    adjust_master_volume(static_cast<int>(VOLUME_STEP), true, true);
  }
  if(down_pressed && !previous_down) {
    adjust_master_volume(-static_cast<int>(VOLUME_STEP), true, true);
  }

  previous_up = up_pressed;
  previous_down = down_pressed;
}

static void poll_keyboard() {
  gb.direct.joypad = 0xff;
  M5Cardputer.update();
  Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
  handle_volume_keys(status);
  if(!M5Cardputer.Keyboard.isPressed()) {
    return;
  }

  auto press_button = [&](JoypadButton button) {
    switch(button) {
      case JoypadButton::Up:
        gb.direct.joypad_bits.up = 0;
        break;
      case JoypadButton::Down:
        gb.direct.joypad_bits.down = 0;
        break;
      case JoypadButton::Left:
        gb.direct.joypad_bits.left = 0;
        break;
      case JoypadButton::Right:
        gb.direct.joypad_bits.right = 0;
        break;
      case JoypadButton::A:
        gb.direct.joypad_bits.a = 0;
        break;
      case JoypadButton::B:
        gb.direct.joypad_bits.b = 0;
        break;
      case JoypadButton::Start:
        gb.direct.joypad_bits.start = 0;
        break;
      case JoypadButton::Select:
        gb.direct.joypad_bits.select = 0;
        break;
    }
  };

  auto handle_key = [&](char key) {
    if(key == 0) {
      return;
    }
    const char normalized = static_cast<char>(std::tolower(static_cast<unsigned char>(key)));
    for(size_t i = 0; i < JOYPAD_BUTTON_COUNT; ++i) {
      const uint8_t binding = g_settings.button_mapping[i];
      if(binding == 0) {
        continue;
      }
      if(normalized == static_cast<char>(binding)) {
        press_button(static_cast<JoypadButton>(i));
      }
    }
  };

  for(auto key : status.word) {
    handle_key(key);
  }
}

static void poll_keyboard();

static void wait_for_keyboard_release() {
  while(M5Cardputer.Keyboard.isPressed()) {
    M5Cardputer.update();
    delay(30);
  }
}

static inline size_t rom_source_size(const struct priv_t *priv) {
  if(priv == nullptr) {
    return 0;
  }
  if(priv->rom_source == RomSource::Embedded) {
    return priv->embedded_rom_size;
  }
  if(priv->rom_source == RomSource::SdCard) {
    return priv->rom_cache.size;
  }
  return 0;
}

static inline uint8_t rom_source_read_byte(const struct priv_t *priv, uint32_t addr) {
  if(priv == nullptr) {
    return 0xFF;
  }

  if(priv->rom_source == RomSource::Embedded) {
    RomCache *cache = const_cast<RomCache *>(&priv->rom_cache);
    if(cache->use_memory) {
      return rom_cache_read(cache, addr);
    }
    if(priv->embedded_rom == nullptr || addr >= priv->embedded_rom_size) {
      return 0xFF;
    }
    cache->cache_hits++;
    return pgm_read_byte(priv->embedded_rom + addr);
  }

  if(priv->rom_source == RomSource::SdCard) {
    RomCache *cache = const_cast<RomCache *>(&priv->rom_cache);
    return rom_cache_read(cache, addr);
  }

  return 0xFF;
}




uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
  struct priv_t * const p = (struct priv_t *)gb->direct.priv;
  return rom_source_read_byte(p, addr);
}




uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
  const struct priv_t * const p = (const struct priv_t *)gb->direct.priv;

#if ENABLE_MBC7

  if(gb->mbc == 7) {
    if(addr < 256) {

      uint16_t word_index = addr / 2;
      if(word_index < 128) {
        if(addr & 1) {
          return (gb->mbc7.eeprom.data[word_index] >> 8) & 0xFF;
        } else {
          return gb->mbc7.eeprom.data[word_index] & 0xFF;
        }
      }
    }
    return 0xFF;
  }
#endif

  if(p->cart_ram == nullptr || addr >= p->cart_ram_size) {
    return 0xFF;
  }
  return p->cart_ram[addr];
}




void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr,
         const uint8_t val)
{
  struct priv_t * const p = (struct priv_t *)gb->direct.priv;

#if ENABLE_MBC7

  if(gb->mbc == 7) {
    if(addr < 256) {

      uint16_t word_index = addr / 2;
      if(word_index < 128) {
        if(addr & 1) {
          gb->mbc7.eeprom.data[word_index] = (gb->mbc7.eeprom.data[word_index] & 0x00FF) | ((uint16_t)val << 8);
        } else {
          gb->mbc7.eeprom.data[word_index] = (gb->mbc7.eeprom.data[word_index] & 0xFF00) | val;
        }
        p->mbc7_eeprom_dirty = true;
        p->mbc7_save_write_failed = false;
      }
    }
    return;
  }
#endif

  if(p->cart_ram == nullptr || addr >= p->cart_ram_size) {
    return;
  }

  if(p->cart_ram[addr] == val) {
    return;
  }

  p->cart_ram[addr] = val;
  p->cart_ram_dirty = true;
  p->cart_save_write_failed = false;
}

static void gb_printer_serial_tx(struct gb_s *gb, const uint8_t tx) {
  (void)gb;
  g_printer.onTransmit(tx);
}

static enum gb_serial_rx_ret_e gb_printer_serial_rx(struct gb_s *gb, uint8_t *rx) {
  (void)gb;
  if(rx == nullptr) {
    return GB_SERIAL_RX_NO_CONNECTION;
  }
  *rx = g_printer.lastResponse();
  return GB_SERIAL_RX_SUCCESS;
}




static uint8_t* rom_cache_alloc_block(size_t bytes, bool prefer_internal_first) {
  uint8_t *ptr = nullptr;
  if(prefer_internal_first) {
    ptr = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(ptr != nullptr) {
      return ptr;
    }
  }

  ptr = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if(ptr != nullptr) {
    return ptr;
  }

  if(!prefer_internal_first) {
    ptr = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(ptr != nullptr) {
      return ptr;
    }
  }

  return (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
}

static inline int16_t IRAM_ATTR rom_cache_bank_index(const RomCache *cache, const RomCacheBank *bank) {
  return static_cast<int16_t>(bank - cache->banks);
}

static inline void IRAM_ATTR rom_cache_lru_detach(RomCache *cache, int16_t index) {
  if(index < 0 || index >= ROM_CACHE_BANK_MAX) {
    return;
  }

  RomCacheBank *entry = &cache->banks[index];
  int16_t prev = entry->lru_prev;
  int16_t next = entry->lru_next;

  if(prev >= 0) {
    cache->banks[prev].lru_next = next;
  }
  if(next >= 0) {
    cache->banks[next].lru_prev = prev;
  }
  if(cache->lru_head == index) {
    cache->lru_head = next;
  }
  if(cache->lru_tail == index) {
    cache->lru_tail = prev;
  }

  entry->lru_prev = -1;
  entry->lru_next = -1;
}

static inline void IRAM_ATTR rom_cache_lru_touch(RomCache *cache, int16_t index) {
  if(index < 0 || index >= ROM_CACHE_BANK_MAX) {
    return;
  }

  rom_cache_lru_detach(cache, index);

  RomCacheBank *entry = &cache->banks[index];
  entry->lru_prev = -1;
  entry->lru_next = cache->lru_head;

  if(cache->lru_head >= 0) {
    cache->banks[cache->lru_head].lru_prev = index;
  }

  cache->lru_head = index;
  if(cache->lru_tail < 0) {
    cache->lru_tail = index;
  }
}

static inline void IRAM_ATTR rom_cache_lru_insert_after(RomCache *cache, int16_t index, int16_t after_index) {
  if(index < 0 || index >= ROM_CACHE_BANK_MAX) {
    return;
  }

  if(after_index < 0 || after_index >= ROM_CACHE_BANK_MAX) {
    rom_cache_lru_touch(cache, index);
    return;
  }

  rom_cache_lru_detach(cache, index);

  RomCacheBank *entry = &cache->banks[index];
  RomCacheBank *after_entry = &cache->banks[after_index];

  entry->lru_prev = after_index;
  entry->lru_next = after_entry->lru_next;

  if(after_entry->lru_next >= 0) {
    cache->banks[after_entry->lru_next].lru_prev = index;
  } else {
    cache->lru_tail = index;
  }

  after_entry->lru_next = index;
}

static bool rom_cache_prepare_buffers(RomCache *cache) {
  if(cache == nullptr) {
    return false;
  }

  size_t bank_limit = rom_cache_preferred_bank_limit();
  if(cache->bank_count == 0) {
    cache->bank_count = bank_limit;
  }
  if(cache->bank_count > bank_limit) {
    cache->bank_count = bank_limit;
  }
  if(cache->bank_count > ROM_CACHE_BANK_MAX) {
    cache->bank_count = ROM_CACHE_BANK_MAX;
  }

  cache->bank_size = rom_cache_preferred_block_size();
  rom_cache_update_geometry(cache);

  if(cache->bank0 == nullptr) {
    cache->bank0 = rom_cache_alloc_block(cache->bank_size, true);
    if(cache->bank0 == nullptr) {
      Serial.println("ROM cache: failed to allocate bank0 buffer");
      return false;
    }
  }
  memset(cache->bank0, 0xFF, cache->bank_size);

  size_t allocated_banks = 0;

  for(size_t i = 0; i < cache->bank_count; ++i) {
    RomCacheBank &entry = cache->banks[i];
    if(entry.data == nullptr) {
      entry.data = rom_cache_alloc_block(cache->bank_size, false);
    }
    if(entry.data == nullptr) {
      Serial.printf("ROM cache: failed to allocate bank buffer #%u\n", (unsigned)i);
      break;
    }
    memset(entry.data, 0xFF, cache->bank_size);
    entry.bank_number = -1;
    entry.valid = false;
    entry.lru_prev = -1;
    entry.lru_next = -1;
    allocated_banks++;
  }

  if(allocated_banks == 0) {
    Serial.println("ROM cache: unable to allocate any cache banks");
    return false;
  }

  if(allocated_banks < cache->bank_count) {
    Serial.printf("ROM cache: reducing active banks to %u due to memory limits\n",
                  (unsigned)allocated_banks);
    for(size_t i = allocated_banks; i < cache->bank_count; ++i) {
      if(cache->banks[i].data != nullptr) {
        heap_caps_free(cache->banks[i].data);
        cache->banks[i].data = nullptr;
      }
      cache->banks[i].bank_number = -1;
      cache->banks[i].valid = false;
      cache->banks[i].lru_prev = -1;
      cache->banks[i].lru_next = -1;
    }
    cache->bank_count = allocated_banks;
  }

  for(size_t i = cache->bank_count; i < ROM_CACHE_BANK_MAX; ++i) {
    if(cache->banks[i].data != nullptr) {
      heap_caps_free(cache->banks[i].data);
      cache->banks[i].data = nullptr;
    }
    cache->banks[i].bank_number = -1;
    cache->banks[i].valid = false;
    cache->banks[i].lru_prev = -1;
    cache->banks[i].lru_next = -1;
  }

  cache->cache_hits = 0;
  cache->cache_misses = 0;
  cache->cache_swaps = 0;
  cache->lru_head = -1;
  cache->lru_tail = -1;
  cache->size = 0;
  cache->use_memory = false;
  cache->memory_rom = nullptr;
  cache->memory_size = 0;
  cache->hot_bank = -1;
  cache->hot_bank_ptr = nullptr;
  cache->hot_bank_base = 0;
#if ENABLE_PROFILING
  g_rom_profiler.last_hits = 0;
  g_rom_profiler.last_misses = 0;
  g_rom_profiler.last_swaps = 0;
#endif

  return true;
}

static void rom_cache_reset(RomCache *cache) {
  size_t bank_count = cache->bank_count;
  if(bank_count == 0 || bank_count > ROM_CACHE_BANK_MAX) {
    bank_count = ROM_CACHE_BANK_MAX;
  }

  size_t bank_limit = rom_cache_preferred_bank_limit();
  if(bank_count > bank_limit) {
    bank_count = bank_limit;
  }

  if(cache->bank0 != nullptr) {
    heap_caps_free(cache->bank0);
    cache->bank0 = nullptr;
  }

  for(size_t i = 0; i < ROM_CACHE_BANK_MAX; ++i) {
    if(cache->banks[i].data != nullptr) {
      heap_caps_free(cache->banks[i].data);
      cache->banks[i].data = nullptr;
    }
    cache->banks[i].bank_number = -1;
    cache->banks[i].valid = false;
    cache->banks[i].lru_prev = -1;
    cache->banks[i].lru_next = -1;
  }

  cache->file = File();
  cache->size = 0;
  cache->memory_rom = nullptr;
  cache->memory_size = 0;
  cache->use_memory = false;
  cache->bank_count = bank_count;
  cache->bank_size = rom_cache_preferred_block_size();
  rom_cache_update_geometry(cache);
  cache->cache_hits = 0;
  cache->cache_misses = 0;
  cache->cache_swaps = 0;
  cache->lru_head = -1;
  cache->lru_tail = -1;
  cache->hot_bank = -1;
  cache->hot_bank_ptr = nullptr;
  cache->hot_bank_base = 0;
#if ENABLE_PROFILING
  g_rom_profiler.last_hits = 0;
  g_rom_profiler.last_misses = 0;
  g_rom_profiler.last_swaps = 0;
#endif
}

static bool rom_cache_trim_banks(RomCache *cache, size_t new_count) {
  if(cache == nullptr) {
    return false;
  }

  if(new_count < 1) {
    new_count = 1;
  }

  if(new_count >= cache->bank_count) {
    return true;
  }

  for(size_t i = new_count; i < cache->bank_count; ++i) {
    RomCacheBank &entry = cache->banks[i];
    if(entry.data != nullptr) {
      heap_caps_free(entry.data);
      entry.data = nullptr;
    }
    entry.bank_number = -1;
    entry.valid = false;
    entry.lru_prev = -1;
    entry.lru_next = -1;
  }

  cache->bank_count = new_count;
  cache->lru_head = -1;
  cache->lru_tail = -1;
  cache->cache_hits = 0;
  cache->cache_misses = 0;
  cache->cache_swaps = 0;
  cache->hot_bank = -1;
  cache->hot_bank_ptr = nullptr;
  cache->hot_bank_base = 0;

  for(size_t i = 0; i < cache->bank_count; ++i) {
    RomCacheBank &entry = cache->banks[i];
    entry.bank_number = -1;
    entry.valid = false;
    entry.lru_prev = -1;
    entry.lru_next = -1;
  }

  return true;
}

static bool rom_cache_fill_bank(RomCache *cache, RomCacheBank *slot, uint32_t bank) {
  if(cache == nullptr || slot == nullptr) {
    return false;
  }

  const size_t block_size = cache->bank_size ? cache->bank_size : ROM_STREAM_BLOCK_SIZE;
  if(slot->data == nullptr) {
    slot->data = rom_cache_alloc_block(block_size, false);
    if(slot->data == nullptr) {
      Serial.println("ROM cache: failed to allocate bank buffer");
      return false;
    }
    memset(slot->data, 0xFF, block_size);
  }

  uint32_t base = rom_cache_bank_base(cache, bank);

  if(cache->use_memory) {
    if(cache->memory_rom == nullptr || base >= cache->memory_size) {
      return false;
    }

    size_t remaining = cache->memory_size - base;
    size_t to_copy = remaining > block_size ? block_size : remaining;
    if(to_copy > 0) {
      memcpy_P(slot->data, cache->memory_rom + base, to_copy);
    }
    if(to_copy < block_size) {
      memset(slot->data + to_copy, 0xFF, block_size - to_copy);
    }

    slot->bank_number = bank;
    slot->valid = true;
    return true;
  }

  if(!cache->file || base >= cache->size) {
    return false;
  }

  if(!cache->file.seek(base)) {
    return false;
  }

  size_t remaining = cache->size - base;
  size_t to_read = remaining > block_size ? block_size : remaining;
  size_t read_total = 0;

  while(read_total < to_read) {
    int chunk = cache->file.read(slot->data + read_total, to_read - read_total);
    if(chunk <= 0) {
      break;
    }
    read_total += chunk;
  }

  if(read_total != to_read) {
    Serial.printf("ROM cache read short (bank %u, expected %u, got %u)\n",
                  (unsigned)bank, (unsigned)to_read, (unsigned)read_total);
    if(read_total < block_size) {
      memset(slot->data + read_total, 0xFF, block_size - read_total);
    }
  }

  if(to_read < block_size) {
    memset(slot->data + to_read, 0xFF, block_size - to_read);
  }

  slot->bank_number = bank;
  slot->valid = true;
  return true;
}

static bool rom_cache_open(RomCache *cache, const char *path) {
  rom_cache_close(cache);

  if(!rom_cache_prepare_buffers(cache)) {
    rom_cache_reset(cache);
    return false;
  }

  cache->file = SD.open(path, FILE_READ);
  if(!cache->file) {
    Serial.println("Failed to open ROM file");
    return false;
  }

  size_t rom_size = cache->file.size();
  if(rom_size == 0) {
    Serial.println("ROM size is zero");
    cache->file.close();
    rom_cache_reset(cache);
    return false;
  }

  cache->size = rom_size;

  Serial.printf("Streaming ROM '%s' (%u bytes)\n", path, (unsigned)rom_size);
  Serial.printf("ROM cache banks in use: %u\n", (unsigned)cache->bank_count);
  Serial.printf("Free PSRAM: %u bytes, Free internal heap: %u bytes\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  cache->file.seek(0);
  size_t to_read = rom_size > cache->bank_size ? cache->bank_size : rom_size;
  size_t read_total = 0;
  while(read_total < to_read) {
    int chunk = cache->file.read(cache->bank0 + read_total, to_read - read_total);
    if(chunk <= 0) {
      break;
    }
    read_total += chunk;
  }

  if(read_total != to_read) {
    Serial.printf("Failed to read first ROM bank (expected %u, got %u)\n",
                  (unsigned)to_read, (unsigned)read_total);
    rom_cache_close(cache);
    return false;
  }

  if(to_read < cache->bank_size) {
    memset(cache->bank0 + to_read, 0xFF, cache->bank_size - to_read);
  }

  cache->hot_bank = 0;
  cache->hot_bank_ptr = cache->bank0;
  cache->hot_bank_base = rom_cache_bank_base(cache, 0);

  return true;
}

static bool rom_cache_open_memory(RomCache *cache, const uint8_t *data, size_t size) {
  if(cache == nullptr || data == nullptr || size == 0) {
    return false;
  }

  rom_cache_close(cache);
  cache->bank_count = 0;
  cache->bank_size = 0;
  rom_cache_update_geometry(cache);
  cache->use_memory = false;
  cache->memory_rom = data;
  cache->memory_size = size;
  cache->size = size;
  cache->cache_hits = 0;
  cache->cache_misses = 0;
  cache->cache_swaps = 0;
  cache->hot_bank = -1;
  cache->hot_bank_ptr = nullptr;
  cache->hot_bank_base = 0;

  Serial.printf("Embedded ROM mapped directly (%u bytes)\n", (unsigned)size);
  Serial.println("ROM cache disabled for embedded source");
  Serial.printf("Free PSRAM: %u bytes, Free internal heap: %u bytes\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  return true;
}

static inline uint8_t IRAM_ATTR rom_cache_read(RomCache *cache, uint32_t addr) {
  if(cache->size == 0 || addr >= cache->size) {
    return 0xFF;
  }

  if(cache->use_memory) {
    cache->cache_hits++;
    return pgm_read_byte(cache->memory_rom + addr);
  }

  const size_t block_size = cache->bank_size ? cache->bank_size : ROM_STREAM_BLOCK_SIZE;

  if(cache->hot_bank_ptr != nullptr) {
    const uint32_t base = cache->hot_bank_base;
    if(addr >= base) {
      const uint32_t rel = addr - base;
      if(rel < block_size) {
        cache->cache_hits++;
        if(rel + 64 < block_size) {
          __builtin_prefetch(cache->hot_bank_ptr + rel + 64, 0, 1);
        }
        return cache->hot_bank_ptr[rel];
      }
    }
  }

  if(addr < block_size) {
    if(cache->bank0 != nullptr) {
      cache->cache_hits++;
      cache->hot_bank = 0;
      cache->hot_bank_ptr = cache->bank0;
      cache->hot_bank_base = 0;
      return cache->bank0[addr];
    }
    cache->hot_bank = -1;
    cache->hot_bank_ptr = nullptr;
    cache->hot_bank_base = 0;
    return 0xFF;
  }

  uint32_t bank;
  uint32_t offset;
  if(cache->bank_shift_valid) {
    bank = addr >> cache->bank_shift;
    offset = addr & cache->bank_mask;
  } else {
    bank = addr / block_size;
    offset = addr % block_size;
  }

  size_t bank_count = cache->bank_count;
  if(bank_count == 0) {
    bank_count = 1;
  }

  RomCacheBank *empty_slot = nullptr;

  for(size_t i = 0; i < bank_count; ++i) {
    RomCacheBank *candidate = &cache->banks[i];
    if(candidate->valid && candidate->data != nullptr) {
      if(candidate->bank_number == (int32_t)bank) {
        cache->cache_hits++;
        int16_t candidate_index = rom_cache_bank_index(cache, candidate);
        rom_cache_lru_touch(cache, candidate_index);
        cache->hot_bank = candidate->bank_number;
        cache->hot_bank_ptr = candidate->data;
        cache->hot_bank_base = rom_cache_bank_base(cache, bank);
        if(offset + 64 < block_size) {
          __builtin_prefetch(candidate->data + offset + 64, 0, 1);
        }
        return candidate->data[offset];
      }
    } else if(empty_slot == nullptr) {
      empty_slot = candidate;
    }
  }

  cache->cache_misses++;

  RomCacheBank *slot = empty_slot;
  if(slot == nullptr) {
    if(cache->lru_tail >= 0) {
      slot = &cache->banks[cache->lru_tail];
    } else {
      slot = &cache->banks[0];
    }
  }

  const bool slot_prev_valid = slot->valid;
  const int32_t slot_prev_bank = slot->bank_number;

  int16_t slot_index = rom_cache_bank_index(cache, slot);
  rom_cache_lru_detach(cache, slot_index);
  slot->valid = false;

  if(!rom_cache_fill_bank(cache, slot, bank)) {
    Serial.printf("Failed to fill ROM cache bank %u\n", (unsigned)bank);
    if(slot->data != nullptr) {
      memset(slot->data, 0xFF, block_size);
    }
    slot->bank_number = bank;
    slot->valid = true;
  }

  rom_cache_lru_touch(cache, slot_index);

  if(slot_prev_valid && slot_prev_bank != (int32_t)bank) {
    cache->cache_swaps++;
  }

  if(cache->bank_count > 1) {
    uint32_t next_bank = bank + 1;
    if((uint64_t)next_bank * block_size < cache->size) {
      bool have_next = false;
      for(size_t i = 0; i < bank_count; ++i) {
        if(cache->banks[i].valid && cache->banks[i].bank_number == (int32_t)next_bank) {
          have_next = true;
          break;
        }
      }
      if(!have_next) {
        RomCacheBank *prefetch_slot = nullptr;
        for(size_t i = 0; i < bank_count; ++i) {
          RomCacheBank *candidate = &cache->banks[i];
          if(candidate == slot) {
            continue;
          }
          if(!candidate->valid) {
            prefetch_slot = candidate;
            break;
          }
        }

        if(prefetch_slot == nullptr && cache->lru_tail >= 0) {
          RomCacheBank *tail = &cache->banks[cache->lru_tail];
          if(tail == slot && tail->lru_prev >= 0) {
            prefetch_slot = &cache->banks[tail->lru_prev];
          } else if(tail != slot) {
            prefetch_slot = tail;
          }
        }

        if(prefetch_slot != nullptr) {
          const bool prefetch_prev_valid = prefetch_slot->valid;
          const int32_t prefetch_prev_bank = prefetch_slot->bank_number;
          int16_t prefetch_index = rom_cache_bank_index(cache, prefetch_slot);
          rom_cache_lru_detach(cache, prefetch_index);
          prefetch_slot->valid = false;
          if(!rom_cache_fill_bank(cache, prefetch_slot, next_bank)) {
            if(prefetch_slot->data != nullptr) {
              memset(prefetch_slot->data, 0xFF, block_size);
            }
            prefetch_slot->bank_number = next_bank;
            prefetch_slot->valid = true;
          }
          rom_cache_lru_insert_after(cache, prefetch_index, slot_index);
          if(prefetch_prev_valid && prefetch_prev_bank != (int32_t)next_bank) {
            cache->cache_swaps++;
          }
        }
      }
    }
  }

  if(slot->data != nullptr && offset + 64 < block_size) {
    __builtin_prefetch(slot->data + offset + 64, 0, 1);
  }

  if(slot->data != nullptr) {
    cache->hot_bank = slot->bank_number;
    cache->hot_bank_ptr = slot->data;
    cache->hot_bank_base = rom_cache_bank_base(cache, slot->bank_number);
    return slot->data[offset];
  }

  cache->hot_bank = -1;
  cache->hot_bank_ptr = nullptr;
  cache->hot_bank_base = 0;
  return 0xFF;
}

static void rom_cache_close(RomCache *cache) {
  if(cache->file) {
    cache->file.close();
  }
  rom_cache_reset(cache);
}

static uint8_t rom_cache_cgb_flag(const RomCache *cache) {
  if(cache == nullptr || cache->size <= 0x0143) {
    return 0;
  }
  if(cache->memory_rom != nullptr && cache->memory_size > 0x0143) {
    return pgm_read_byte(cache->memory_rom + 0x0143);
  }
  if(cache->bank0 != nullptr && cache->bank_size > 0x0143) {
    return cache->bank0[0x0143];
  }
  return 0;
}

static bool load_embedded_rom(struct priv_t *priv, const EmbeddedRomEntry *entry) {
  if(priv == nullptr || entry == nullptr) {
    return false;
  }

  rom_cache_close(&priv->rom_cache);

  priv->embedded_rom_entry = entry;
  priv->embedded_rom = entry->data;
  priv->embedded_rom_size = entry->size;
  priv->rom_source = RomSource::None;
  clear_sd_rom_path(priv);

  if(priv->embedded_rom == nullptr || priv->embedded_rom_size == 0) {
    Serial.println("Embedded ROM data is not available");
    priv->embedded_rom_entry = nullptr;
    return false;
  }

  if(!rom_cache_open_memory(&priv->rom_cache, priv->embedded_rom, priv->embedded_rom_size)) {
    Serial.println("Failed to initialise embedded ROM cache");
    priv->embedded_rom = nullptr;
    priv->embedded_rom_size = 0;
    priv->embedded_rom_entry = nullptr;
    return false;
  }

  priv->rom_source = RomSource::Embedded;

  if(priv->embedded_rom_size > 0x0143) {
    priv->rom_cgb_flag = rom_source_read_byte(priv, 0x0143);
  } else {
    priv->rom_cgb_flag = 0;
  }

  priv->rom_is_cgb = (priv->rom_cgb_flag & 0x80) != 0;
  priv->rom_is_cgb_only = (priv->rom_cgb_flag == 0xC0);

  const char *name = (entry->name != nullptr && entry->name[0] != '\0')
                         ? entry->name
                         : (entry->id != nullptr ? entry->id : "Embedded ROM");

  Serial.printf("Using embedded ROM '%s' (%u bytes)\n",
                name,
                (unsigned)priv->embedded_rom_size);

  if(priv->rom_is_cgb_only) {
    Serial.println("Warning: Embedded ROM is marked as CGB-only; compatibility is limited.");
  }

  return true;
}

static const char* gbc_palette_name(size_t index) {
  if(index < GBC_PALETTE_COUNT) {
    return GBC_PALETTE_NAMES[index];
  }
  return "Unknown";
}

static void palette_set_label(PaletteState *palette, const char *label) {
  if(palette == nullptr) {
    return;
  }

  if(label == nullptr) {
    palette->label[0] = '\0';
    return;
  }

  strncpy(palette->label, label, sizeof(palette->label) - 1);
  palette->label[sizeof(palette->label) - 1] = '\0';
}

static void palette_apply_dmg(PaletteState *palette) {
  if(palette == nullptr) {
    return;
  }

  palette->gbc_enabled = false;
  palette->active_index = 0xFF;
  palette->auto_assigned = false;
  palette->combo_override = false;
  palette_set_label(palette, "DMG Default");

  for(size_t i = 0; i < 4; ++i) {
    palette->bg_rgb565[i] = DMG_DEFAULT_PALETTE_RGB565[i];
    palette->obj0_rgb565[i] = DMG_DEFAULT_PALETTE_RGB565[i];
    palette->obj1_rgb565[i] = DMG_DEFAULT_PALETTE_RGB565[i];
  }
}

static void palette_apply_gbc_index(PaletteState *palette,
                                    size_t index,
                                    bool auto_selected,
                                    bool combo_selected) {
  if(palette == nullptr || GBC_PALETTE_COUNT == 0) {
    return;
  }

  if(index >= GBC_PALETTE_COUNT) {
    index = DEFAULT_GBC_PALETTE_INDEX;
  }

  const size_t base = index * 12;

  palette->gbc_enabled = true;
  palette->active_index = static_cast<uint8_t>(index);
  palette->auto_assigned = auto_selected;
  palette->combo_override = combo_selected;
  palette_set_label(palette, gbc_palette_name(index));

  for(size_t i = 0; i < 4; ++i) {
    const uint16_t bg = rgb888_to_rgb565(GBC_PALETTES[base + i]);
    const uint16_t obj0 = rgb888_to_rgb565(GBC_PALETTES[base + 4 + i]);
    const uint16_t obj1 = rgb888_to_rgb565(GBC_PALETTES[base + 8 + i]);
    palette->bg_rgb565[i] = bg;
    palette->obj0_rgb565[i] = obj0;
    palette->obj1_rgb565[i] = obj1;
  }
}

static void palette_apply_bootstrap(PaletteState *palette,
                                    size_t index,
                                    bool auto_selected,
                                    bool combo_selected) {
  if(palette == nullptr || index >= kCgbBootstrapPaletteCount) {
    return;
  }

  const uint32_t *colours = kCgbBootstrapPaletteData[index];

  palette->gbc_enabled = true;
  palette->active_index = static_cast<uint8_t>(index);
  palette->auto_assigned = auto_selected;
  palette->combo_override = combo_selected;

  for(size_t i = 0; i < 4; ++i) {
    const uint16_t bg = rgb888_to_rgb565(colours[i]);
    const uint16_t obj0 = rgb888_to_rgb565(colours[4 + i]);
    const uint16_t obj1 = rgb888_to_rgb565(colours[8 + i]);
    palette->bg_rgb565[i] = bg;
    palette->obj0_rgb565[i] = obj0;
    palette->obj1_rgb565[i] = obj1;
  }
}

static void palette_disable_overrides(PaletteState *palette) {
  if(palette == nullptr) {
    return;
  }

  palette->gbc_enabled = false;
  palette->active_index = 0xFF;
  palette->auto_assigned = false;
  palette->combo_override = false;
  palette_set_label(palette, "CGB Native");
}

enum BootComboDirection : uint8_t {
  BOOT_DIR_NONE = 0,
  BOOT_DIR_UP,
  BOOT_DIR_RIGHT,
  BOOT_DIR_DOWN,
  BOOT_DIR_LEFT
};

static bool palette_map_combo(uint8_t direction,
                              bool button_a,
                              bool button_b,
                              size_t *index_out) {
  if(index_out == nullptr) {
    return false;
  }

  if(direction == BOOT_DIR_NONE) {
    return false;
  }


  if(button_a && button_b) {
    return false;
  }

  size_t index = SIZE_MAX;
  switch(direction) {
    case BOOT_DIR_UP:
      index = button_a ? 2 : (button_b ? 3 : 1);
      break;
    case BOOT_DIR_RIGHT:
      index = button_a ? 11 : (button_b ? 12 : 10);
      break;
    case BOOT_DIR_DOWN:
      index = button_a ? 8 : (button_b ? 9 : 7);
      break;
    case BOOT_DIR_LEFT:
      index = button_a ? 5 : (button_b ? 6 : 4);
      break;
    default:
      return false;
  }

  if(index >= GBC_PALETTE_COUNT) {
    return false;
  }

  *index_out = index;
  return true;
}

static void palette_extract_keys(const Keyboard_Class::KeysState &status,
                                 bool &up,
                                 bool &right,
                                 bool &down,
                                 bool &left,
                                 bool &button_a,
                                 bool &button_b) {
  up = right = down = left = false;
  button_a = button_b = false;

  for(auto key : status.word) {
    switch(key) {
      case 'e':
      case 'E':
        up = true;
        break;
      case 'd':
      case 'D':
        right = true;
        break;
      case 's':
      case 'S':
        down = true;
        break;
      case 'a':
      case 'A':
        left = true;
        break;
      case 'l':
      case 'L':
        button_a = true;
        break;
      case 'k':
      case 'K':
        button_b = true;
        break;
      default:
        break;
    }
  }
}

static bool palette_capture_boot_combo(size_t *out_index) {
  if(out_index == nullptr) {
    return false;
  }

  const uint64_t start = micros64();
  const uint64_t timeout = start + 700000;

  while(micros64() < timeout) {
    M5Cardputer.update();
    const Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

    bool up, right, down, left, button_a, button_b;
    palette_extract_keys(status, up, right, down, left, button_a, button_b);

    uint8_t direction = BOOT_DIR_NONE;
    const uint8_t dir_count = static_cast<uint8_t>(up) + static_cast<uint8_t>(right) +
                              static_cast<uint8_t>(down) + static_cast<uint8_t>(left);
    if(dir_count == 1) {
      if(up) {
        direction = BOOT_DIR_UP;
      } else if(right) {
        direction = BOOT_DIR_RIGHT;
      } else if(down) {
        direction = BOOT_DIR_DOWN;
      } else if(left) {
        direction = BOOT_DIR_LEFT;
      }
    }

    size_t detected_index = SIZE_MAX;
    if(palette_map_combo(direction, button_a, button_b, &detected_index)) {
      *out_index = detected_index;
      return true;
    }

    delay(10);
  }

  return false;
}

static void palette_wait_for_boot_combo_release() {
  const uint64_t start = micros64();
  const uint64_t timeout = start + 400000;
  while(micros64() < timeout) {
    M5Cardputer.update();
    if(!M5Cardputer.Keyboard.isPressed()) {
      break;
    }
    delay(10);
  }
}

static inline int8_t hex_char_to_nibble(uint8_t value) {
  if(value >= '0' && value <= '9') {
    return static_cast<int8_t>(value - '0');
  }
  if(value >= 'A' && value <= 'F') {
    return static_cast<int8_t>(10 + (value - 'A'));
  }
  if(value >= 'a' && value <= 'f') {
    return static_cast<int8_t>(10 + (value - 'a'));
  }
  return -1;
}

static size_t palette_lookup_auto_index(const struct priv_t *priv,
                                        bool *out_bootstrap,
                                        char *label_buffer,
                                        size_t label_buffer_len) {
  if(out_bootstrap != nullptr) {
    *out_bootstrap = false;
  }

  if(label_buffer != nullptr && label_buffer_len > 0) {
    label_buffer[0] = '\0';
  }

  if(priv == nullptr || !g_settings.cgb_bootstrap_palettes) {
    return SIZE_MAX;
  }

  const size_t rom_size = rom_source_size(priv);

  if(rom_size < 0x150) {
    return SIZE_MAX;
  }

  auto read_rom = [&](uint32_t address) {
    return rom_source_read_byte(priv, address);
  };


  const uint8_t old_license = read_rom(0x014B);
  bool nintendo_license = false;
  if(old_license == 0x33) {
    const uint8_t hi = read_rom(0x0144);
    const uint8_t lo = read_rom(0x0145);
    const int8_t hi_nibble = hex_char_to_nibble(hi);
    const int8_t lo_nibble = hex_char_to_nibble(lo);
    if(hi_nibble >= 0 && lo_nibble >= 0) {
      const uint8_t new_license = static_cast<uint8_t>((hi_nibble << 4) | lo_nibble);
      nintendo_license = (new_license == 0x01);
    }
  } else {
    nintendo_license = (old_license == 0x01);
  }

  if(!nintendo_license) {
    return SIZE_MAX;
  }


  uint8_t title_bytes[16];
  bool has_meaningful_chars = false;
  for(size_t i = 0; i < 16; ++i) {
    const uint8_t value = read_rom(0x0134 + i);
    title_bytes[i] = value;
    if(value != 0x00 && value != 0x80 && value != 0xFF && value != ' ') {
      has_meaningful_chars = true;
    }
  }

  if(!has_meaningful_chars) {
    return SIZE_MAX;
  }

  uint8_t name_hash = 0;
  for(size_t i = 0; i < 16; ++i) {
    name_hash = static_cast<uint8_t>(name_hash + title_bytes[i]);
  }

  const uint8_t disambiguation = title_bytes[3];

  size_t exact_match = SIZE_MAX;
  size_t fallback_match = SIZE_MAX;
  for(size_t i = 0; i < kCgbBootstrapNameCount; ++i) {
    const CgbBootstrapPaletteEntry &entry = kCgbBootstrapNameMap[i];
    if(entry.name_hash != name_hash) {
      continue;
    }

    if(entry.disambiguation == disambiguation) {
      exact_match = entry.palette_index;
      break;
    }

    if(entry.disambiguation == 0 && fallback_match == SIZE_MAX) {
      fallback_match = entry.palette_index;
    }
  }

  size_t palette_index = exact_match != SIZE_MAX ? exact_match : fallback_match;
  if(palette_index == SIZE_MAX || palette_index >= kCgbBootstrapPaletteCount) {
    return SIZE_MAX;
  }

  if(out_bootstrap != nullptr) {
    *out_bootstrap = true;
  }

  if(label_buffer != nullptr && label_buffer_len > 0) {
    char trimmed[17] = {0};
    size_t length = 16;
    while(length > 0) {
      uint8_t c = title_bytes[length - 1];
      if(c == 0x00 || c == 0x80 || c == 0xFF || c == ' ') {
        --length;
      } else {
        break;
      }
    }

    const size_t copy_len = (length < sizeof(trimmed) - 1) ? length : (sizeof(trimmed) - 1);
    for(size_t i = 0; i < copy_len; ++i) {
      uint8_t c = title_bytes[i];
      if(c == 0x00 || c == 0x80 || c == 0xFF) {
        break;
      }
      if(c >= 0x20 && c <= 0x7E) {
        trimmed[i] = static_cast<char>(c);
      } else {
        trimmed[i] = '?';
      }
    }

    if(trimmed[0] != '\0') {
      snprintf(label_buffer, label_buffer_len, "Auto: %s", trimmed);
    } else {
      snprintf(label_buffer, label_buffer_len, "Auto: Bootstrap");
    }
  }

  return palette_index;
}

static void palette_configure_for_dmg(struct priv_t *priv) {
  if(priv == nullptr) {
    return;
  }

  palette_apply_dmg(&priv->palette);

  char auto_label[32] = {0};
  bool auto_selected = false;
  bool combo_selected = false;
  bool bootstrap_auto = false;
  size_t bootstrap_index = SIZE_MAX;
  size_t palette_index = DEFAULT_GBC_PALETTE_INDEX;

  const size_t auto_index = palette_lookup_auto_index(priv,
                                                      &bootstrap_auto,
                                                      auto_label,
                                                      sizeof(auto_label));
  if(auto_index != SIZE_MAX) {
    auto_selected = true;
    if(bootstrap_auto) {
      bootstrap_index = auto_index;
    } else {
      palette_index = auto_index;
    }
  }

  size_t combo_index = SIZE_MAX;
  if(palette_capture_boot_combo(&combo_index)) {
    combo_selected = true;
    auto_selected = false;
    bootstrap_auto = false;
    palette_index = combo_index;
    palette_wait_for_boot_combo_release();
  }

  if(combo_selected || !bootstrap_auto) {
    palette_apply_gbc_index(&priv->palette,
                            palette_index,
                            auto_selected,
                            combo_selected);
  } else if(bootstrap_auto) {
    palette_apply_bootstrap(&priv->palette,
                            bootstrap_index,
                            true,
                            false);
  }

  if(auto_selected && auto_label[0] != '\0') {
    palette_set_label(&priv->palette, auto_label);
  }

  const char *source = combo_selected ? "combo" : (auto_selected ? "auto" : "default");
  Serial.printf("Applied DMG GBC palette: %s (%s)\n", priv->palette.label, source);

  if(combo_selected) {
    char message[48];
    snprintf(message, sizeof(message), "Palette: %s", priv->palette.label);
    debugPrint(message);
    delay(500);
    M5Cardputer.Display.clearDisplay();
  }
}




void gb_error(struct gb_s *gb, const enum gb_error_e gb_err, const uint16_t val) {
  const char* gb_err_str[GB_INVALID_MAX] = {
    "UNKNOWN",
    "INVALID OPCODE",
    "INVALID READ",
    "INVALID WRITE",
    "HATL FOREVER"
  };

 struct priv_t * priv = (struct priv_t *)gb->direct.priv;

  if(priv->cart_ram != NULL) {
    free(priv->cart_ram);
    priv->cart_ram = NULL;
  }
  if(priv->rom_source == RomSource::SdCard) {
    rom_cache_close(&priv->rom_cache);
  }
  priv->rom_source = RomSource::None;
  priv->embedded_rom = nullptr;
  priv->embedded_rom_size = 0;
}

#if ENABLE_LCD



void lcd_draw_line(struct gb_s *gb, const uint8_t pixels[160],
     const uint_fast8_t line)
{
  struct priv_t *priv = (priv_t*)gb->direct.priv;

  uint16_t *active_fb = priv->framebuffers[priv->write_fb_index];
  if(active_fb == nullptr) {
    return;
  }

  if(gb->direct.frame_skip && gb->display.frame_skip_count == 0) {
    return;
  }

  uint32_t *row_hash = priv->framebuffer_row_hash[priv->write_fb_index];
  uint8_t *row_dirty = priv->framebuffer_row_dirty[priv->write_fb_index];
  uint32_t previous_hash = 0;
  if(row_hash != nullptr) {
    previous_hash = row_hash[line];
  }
  uint32_t hash = 2166136261u;

  if(gb->cgb.enabled) {

    uint16_t *dst = active_fb + (line * LCD_WIDTH);
    uint32_t *cgb_line = gb->display.cgb_line;

    for(unsigned int x = 0; x < LCD_WIDTH; x += 4) {
      uint16_t c0 = rgb888_to_rgb565(cgb_line[x]);
      uint16_t c1 = rgb888_to_rgb565(cgb_line[x+1]);
      uint16_t c2 = rgb888_to_rgb565(cgb_line[x+2]);
      uint16_t c3 = rgb888_to_rgb565(cgb_line[x+3]);
      dst[x] = c0;
      dst[x+1] = c1;
      dst[x+2] = c2;
      dst[x+3] = c3;
      hash = framebuffer_hash_step(hash, c0);
      hash = framebuffer_hash_step(hash, c1);
      hash = framebuffer_hash_step(hash, c2);
      hash = framebuffer_hash_step(hash, c3);
    }
    if(row_hash != nullptr) {
      row_hash[line] = hash;
    }
    if(row_dirty != nullptr) {
      row_dirty[line] = (hash != previous_hash) ? 1 : 0;
    }
    return;
  }


  uint16_t *dst = active_fb + (line * LCD_WIDTH);

#if PEANUT_GB_12_COLOUR
  if(priv->palette.gbc_enabled) {
    const uint16_t *bg_lut = priv->palette.bg_rgb565;
    const uint16_t *obj0_lut = priv->palette.obj0_rgb565;
    const uint16_t *obj1_lut = priv->palette.obj1_rgb565;

    for(unsigned int x = 0; x < LCD_WIDTH; x++) {
      const uint8_t p = pixels[x];
      const uint8_t pal_bits = (p & LCD_PALETTE_ALL) >> 4;
      const uint16_t *lut = (pal_bits == 0) ? obj0_lut :
                            (pal_bits == 1) ? obj1_lut : bg_lut;
      const uint16_t colour = lut[p & LCD_COLOUR];
      dst[x] = colour;
      hash = framebuffer_hash_step(hash, colour);
    }
    if(row_hash != nullptr) {
      row_hash[line] = hash;
    }
    if(row_dirty != nullptr) {
      row_dirty[line] = (hash != previous_hash) ? 1 : 0;
    }
    return;
  }
#endif


  const uint16_t *bg_lut = priv->palette.bg_rgb565;
  if(bg_lut == nullptr) {
    bg_lut = DMG_DEFAULT_PALETTE_RGB565;
  }
  for(unsigned int x = 0; x < LCD_WIDTH; x += 4) {
    uint16_t c0 = bg_lut[pixels[x] & LCD_COLOUR];
    uint16_t c1 = bg_lut[pixels[x+1] & LCD_COLOUR];
    uint16_t c2 = bg_lut[pixels[x+2] & LCD_COLOUR];
    uint16_t c3 = bg_lut[pixels[x+3] & LCD_COLOUR];
    dst[x] = c0;
    dst[x+1] = c1;
    dst[x+2] = c2;
    dst[x+3] = c3;
    hash = framebuffer_hash_step(hash, c0);
    hash = framebuffer_hash_step(hash, c1);
    hash = framebuffer_hash_step(hash, c2);
    hash = framebuffer_hash_step(hash, c3);
  }

  if(row_hash != nullptr) {
    row_hash[line] = hash;
  }
  if(row_dirty != nullptr) {
    row_dirty[line] = (hash != previous_hash) ? 1 : 0;
  }
}

#if ENABLE_PROFILING
static void profiler_add_render_sample(uint64_t duration_us,
                                       uint32_t rows_written,
                                       uint32_t segments_flushed) {
  portENTER_CRITICAL(&profiler_spinlock);
  g_render_profiler.total_us += duration_us;
  if(duration_us > g_render_profiler.max_us) {
    g_render_profiler.max_us = duration_us;
  }
  g_render_profiler.frames++;
  g_render_profiler.rows_written += rows_written;
  g_render_profiler.segments_flushed += segments_flushed;
  portEXIT_CRITICAL(&profiler_spinlock);
}

static RenderProfiler profiler_consume_render_stats() {
  RenderProfiler snapshot;
  portENTER_CRITICAL(&profiler_spinlock);
  snapshot = g_render_profiler;
  g_render_profiler = {};
  portEXIT_CRITICAL(&profiler_spinlock);
  return snapshot;
}
#endif




void fit_frame(const uint16_t *fb, const uint32_t *row_hash, uint8_t *row_dirty) {
  if(fb == nullptr) {
    return;
  }
#if ENABLE_PROFILING
  uint64_t render_start = micros64();
  uint32_t rows_written = 0;
  uint32_t segments_flushed = 0;
#endif
  if(!frame_row_map_initialised) {
    constexpr float scale = static_cast<float>(LCD_HEIGHT) / static_cast<float>(DEST_H);
    const float max_src = static_cast<float>(LCD_HEIGHT - 1);
    for(unsigned int j = 0; j < DEST_H; j++) {
      float src_y = (static_cast<float>(j) + 0.5f) * scale - 0.5f;
      if(src_y < 0.0f) {
        src_y = 0.0f;
      } else if(src_y > max_src) {
        src_y = max_src;
      }

      int y0 = static_cast<int>(src_y);
      float frac = src_y - static_cast<float>(y0);

      if(frac >= 0.5f && y0 < LCD_HEIGHT - 1) {
        y0 += 1;
      }

      if(y0 >= LCD_HEIGHT) {
        y0 = LCD_HEIGHT - 1;
      }

      frame_row_map[j] = static_cast<uint8_t>(y0);
      frame_row_weight[j] = 0;
    }
    frame_row_map_initialised = true;
  }

  const int32_t x_offset = DISPLAY_CENTER(0);
  const size_t row_bytes = LCD_WIDTH * sizeof(uint16_t);
  const bool cache_was_valid = display_cache_valid;

  auto needs_update = [&](unsigned int src_y0, uint16_t weight) -> bool {
    if(row_dirty == nullptr || !cache_was_valid) {
      return true;
    }
    if(src_y0 >= LCD_HEIGHT) {
      return true;
    }
    const bool dirty0 = row_dirty[src_y0] != 0;
    if(weight == 0) {
      return dirty0;
    }
    if(src_y0 + 1 >= LCD_HEIGHT) {
      return dirty0;
    }
    if(weight >= 256) {
      return row_dirty[src_y0 + 1] != 0;
    }
    return dirty0 || (row_dirty[src_y0 + 1] != 0);
  };

  auto blend_pixel = [](uint16_t c0, uint16_t c1, uint16_t w0, uint16_t w1) -> uint16_t {
    const uint32_t r0 = (c0 >> 11) & 0x1F;
    const uint32_t g0 = (c0 >> 5) & 0x3F;
    const uint32_t b0 = c0 & 0x1F;
    const uint32_t r1 = (c1 >> 11) & 0x1F;
    const uint32_t g1 = (c1 >> 5) & 0x3F;
    const uint32_t b1 = c1 & 0x1F;

    const uint32_t r = (r0 * w0 + r1 * w1 + 128) >> 8;
    const uint32_t g = (g0 * w0 + g1 * w1 + 128) >> 8;
    const uint32_t b = (b0 * w0 + b1 * w1 + 128) >> 8;

    return static_cast<uint16_t>(((r & 0x1F) << 11) |
                                 ((g & 0x3F) << 5) |
                                 (b & 0x1F));
  };

  auto compose_row = [&](uint16_t *dst,
                         unsigned int src_y0,
                         uint16_t weight,
                         bool compute_hash) -> uint32_t {
    const uint16_t *row0 = fb + (src_y0 * LCD_WIDTH);
    const bool can_blend = (weight != 0) && (src_y0 + 1 < LCD_HEIGHT);

    if(!can_blend) {
      memcpy(dst, row0, row_bytes);
      if(!compute_hash) {
        return 0;
      }
      if(row_hash != nullptr) {
        return row_hash[src_y0];
      }
      uint32_t hash = 2166136261u;
      for(unsigned int x = 0; x < LCD_WIDTH; ++x) {
        hash = framebuffer_hash_step(hash, dst[x]);
      }
      return hash;
    }

    const uint16_t *row1 = fb + ((src_y0 + 1) * LCD_WIDTH);
    const uint16_t w1 = weight;
    const uint16_t w0 = 256 - weight;

    if(!compute_hash) {
      for(unsigned int x = 0; x < LCD_WIDTH; ++x) {
        dst[x] = blend_pixel(row0[x], row1[x], w0, w1);
      }
      return 0;
    }

    uint32_t hash = 2166136261u;
    for(unsigned int x = 0; x < LCD_WIDTH; ++x) {
      const uint16_t colour = blend_pixel(row0[x], row1[x], w0, w1);
      dst[x] = colour;
      hash = framebuffer_hash_step(hash, colour);
    }
    return hash;
  };

  uint16_t line_buffer[LCD_WIDTH];

  const bool use_full_cache = swap_fb_enabled && swap_fb_psram_backed && swap_fb != nullptr && row_hash != nullptr;

  if(!use_full_cache) {
    unsigned int segment_rows = 0;
    unsigned int segment_start = 0;
    bool any_change = false;

    auto flush_segment = [&](unsigned int count) {
      if(count == 0) {
        return;
      }
      if(!any_change) {
        M5Cardputer.Display.startWrite();
        any_change = true;
      }
      if(M5Cardputer.Display.dmaBusy()) {
        M5Cardputer.Display.waitDMA();
      }
  M5Cardputer.Display.setAddrWindow(x_offset, segment_start, LCD_WIDTH, count);
  M5Cardputer.Display.writePixelsDMA(fallback_segment_buffer, LCD_WIDTH * count, true);
  M5Cardputer.Display.waitDMA();
#if ENABLE_PROFILING
      rows_written += count;
      segments_flushed++;
#endif
      segment_rows = 0;
    };

    for(unsigned int j = 0; j < DEST_H; j++) {
      const unsigned int src_y0 = frame_row_map[j];
      const uint16_t weight = frame_row_weight[j];
      const bool dirty_hint = needs_update(src_y0, weight);

      if(display_cache_valid && row_hash != nullptr) {
        uint32_t expected_hash = 0;
        bool can_skip = false;
        if(weight == 0) {
          expected_hash = row_hash[src_y0];
          can_skip = true;
        } else if(weight == 256 && src_y0 + 1 < LCD_HEIGHT) {
          expected_hash = row_hash[src_y0 + 1];
          can_skip = true;
        }

        if(can_skip && !dirty_hint && swap_row_hash[j] == expected_hash) {
          flush_segment(segment_rows);
          continue;
        }
      }

      const uint32_t dest_hash = compose_row(line_buffer, src_y0, weight, true);
      const bool row_changed = (!display_cache_valid) || (swap_row_hash[j] != dest_hash);

      if(row_changed) {
        swap_row_hash[j] = dest_hash;
        if(segment_rows == 0) {
          segment_start = j;
        }
        memcpy(fallback_segment_buffer + (segment_rows * LCD_WIDTH), line_buffer, row_bytes);
        segment_rows++;
        if(segment_rows == FALLBACK_SEGMENT_ROWS) {
          flush_segment(segment_rows);
        }
      } else {
        flush_segment(segment_rows);
      }
    }

    flush_segment(segment_rows);

    if(any_change) {
      M5Cardputer.Display.waitDMA();
      M5Cardputer.Display.endWrite();
      display_cache_valid = true;
    }

    if(row_dirty != nullptr) {
      memset(row_dirty, 0, LCD_HEIGHT * sizeof(uint8_t));
    }

#if ENABLE_PROFILING
    profiler_add_render_sample(micros64() - render_start, rows_written, segments_flushed);
#endif
    return;
  }

  unsigned int segment_start = 0;
  unsigned int segment_count = 0;
  bool any_change = false;

  auto flush_segment = [&](unsigned int start, unsigned int count) {
    if(count == 0) {
      return;
    }
    if(!any_change) {
      M5Cardputer.Display.startWrite();
      any_change = true;
    }
    if(M5Cardputer.Display.dmaBusy()) {
      M5Cardputer.Display.waitDMA();
    }
  M5Cardputer.Display.setAddrWindow(x_offset, start, LCD_WIDTH, count);
  M5Cardputer.Display.writePixelsDMA(swap_fb + (start * LCD_WIDTH),
                    LCD_WIDTH * count,
                    swap_fb_dma_capable);
#if ENABLE_PROFILING
    rows_written += count;
    segments_flushed++;
#endif
  };

  for(unsigned int j = 0; j < DEST_H; j++) {
    const unsigned int src_y0 = frame_row_map[j];
    const uint16_t weight = frame_row_weight[j];
    uint16_t *cached_row = swap_fb + (j * LCD_WIDTH);
    const bool dirty_hint = needs_update(src_y0, weight);

    if(display_cache_valid && row_hash != nullptr) {
      uint32_t expected_hash = 0;
      bool can_skip = false;
      if(weight == 0) {
        expected_hash = row_hash[src_y0];
        can_skip = true;
      } else if(weight == 256 && src_y0 + 1 < LCD_HEIGHT) {
        expected_hash = row_hash[src_y0 + 1];
        can_skip = true;
      }

      if(can_skip && !dirty_hint && swap_row_hash[j] == expected_hash) {
        if(segment_count != 0) {
          flush_segment(segment_start, segment_count);
          segment_count = 0;
        }
        continue;
      }
    }

    const uint32_t dest_hash = compose_row(cached_row, src_y0, weight, true);

    if(!display_cache_valid || swap_row_hash[j] != dest_hash) {
      swap_row_hash[j] = dest_hash;
      if(segment_count == 0) {
        segment_start = j;
      }
      segment_count++;
    } else if(segment_count != 0) {
      flush_segment(segment_start, segment_count);
      segment_count = 0;
    }
  }

  if(segment_count != 0) {
    flush_segment(segment_start, segment_count);
  }

  if(any_change) {
    M5Cardputer.Display.waitDMA();
    M5Cardputer.Display.endWrite();
    display_cache_valid = true;
  }

  if(row_dirty != nullptr) {
    memset(row_dirty, 0, LCD_HEIGHT * sizeof(uint8_t));
  }

#if ENABLE_PROFILING
  profiler_add_render_sample(micros64() - render_start, rows_written, segments_flushed);
#endif
}



void draw_frame(const uint16_t *fb) {
  if(fb == nullptr) {
    return;
  }
  const unsigned int max_y = DEST_H < LCD_HEIGHT ? DEST_H : LCD_HEIGHT;
  const size_t row_bytes = LCD_WIDTH * sizeof(uint16_t);

  M5Cardputer.Display.startWrite();
  for(unsigned int j = 0; j < max_y; j++) {
    const uint16_t *src_row = fb + (j * LCD_WIDTH);
    if(swap_fb_enabled && swap_fb != nullptr) {
      uint16_t *cached_row = swap_fb + (j * LCD_WIDTH);
      if(memcmp(cached_row, src_row, row_bytes) == 0) {
        continue;
      }
      memcpy(cached_row, src_row, row_bytes);
    }

    if(M5Cardputer.Display.dmaBusy()) {
      M5Cardputer.Display.waitDMA();
    }
    M5Cardputer.Display.setAddrWindow(0, j, LCD_WIDTH, 1);
    M5Cardputer.Display.writePixelsDMA(src_row, LCD_WIDTH, true);
  }
  M5Cardputer.Display.waitDMA();
  M5Cardputer.Display.endWrite();
}

static void renderTask(void *param) {
  (void)param;
  uint8_t index = 0;
  while(true) {
    if(priv.frame_queue != nullptr &&
       xQueueReceive(priv.frame_queue, &index, portMAX_DELAY) == pdTRUE) {
      if(priv.framebuffers[index] != nullptr) {
        fit_frame(priv.framebuffers[index],
                  priv.framebuffer_row_hash[index],
                  priv.framebuffer_row_dirty[index]);
      }
      if(priv.frame_buffer_free[index] != nullptr) {
        xSemaphoreGive(priv.frame_buffer_free[index]);
      }
    }
  }
}

#endif

#if ENABLE_SOUND
static void audioTask(void *param) {
  (void)param;
  const TickType_t period = pdMS_TO_TICKS(4);
  while(true) {
    audioPump();
    vTaskDelay(period);
  }
}
#endif

#if ENABLE_PROFILING
static void profiler_reset_main(uint64_t now) {
  g_main_profiler.frames = 0;
  g_main_profiler.over_budget_frames = 0;
  g_main_profiler.accum_frame_us = 0;
  g_main_profiler.accum_poll_us = 0;
  g_main_profiler.accum_emu_us = 0;
  g_main_profiler.accum_dispatch_us = 0;
  g_main_profiler.accum_idle_us = 0;
  g_main_profiler.accum_requested_idle_us = 0;
  g_main_profiler.max_frame_us = 0;
  g_main_profiler.last_log_us = now;
}

static void profiler_log(uint64_t now) {
  if(g_main_profiler.frames == 0) {
    g_main_profiler.last_log_us = now;
    return;
  }

  const uint64_t interval_us = now - g_main_profiler.last_log_us;
  if(interval_us == 0) {
    g_main_profiler.last_log_us = now;
    return;
  }

  const double frames = static_cast<double>(g_main_profiler.frames);
  const double fps = frames * 1e6 / static_cast<double>(interval_us);
  const double avg_frame = static_cast<double>(g_main_profiler.accum_frame_us) / frames;
  const double avg_poll = static_cast<double>(g_main_profiler.accum_poll_us) / frames;
  const double avg_emu = static_cast<double>(g_main_profiler.accum_emu_us) / frames;
  const double avg_dispatch = static_cast<double>(g_main_profiler.accum_dispatch_us) / frames;
  const double avg_idle = static_cast<double>(g_main_profiler.accum_idle_us) / frames;
  const double avg_idle_requested = static_cast<double>(g_main_profiler.accum_requested_idle_us) / frames;

  RenderProfiler render_stats = profiler_consume_render_stats();
  const double render_frames = render_stats.frames ? static_cast<double>(render_stats.frames) : 1.0;
  const double avg_render = render_stats.frames ? static_cast<double>(render_stats.total_us) / render_frames : 0.0;
  const double avg_render_rows = render_stats.frames ? static_cast<double>(render_stats.rows_written) / render_frames : 0.0;
  const double avg_render_segments = render_stats.frames ? static_cast<double>(render_stats.segments_flushed) / render_frames : 0.0;
  const double max_render = static_cast<double>(render_stats.max_us);

  const size_t rom_hits = priv.rom_cache.cache_hits;
  const size_t rom_misses = priv.rom_cache.cache_misses;
  const size_t rom_swaps = priv.rom_cache.cache_swaps;
  const size_t delta_hits = rom_hits - g_rom_profiler.last_hits;
  const size_t delta_misses = rom_misses - g_rom_profiler.last_misses;
  const size_t delta_swaps = rom_swaps - g_rom_profiler.last_swaps;
  g_rom_profiler.last_hits = rom_hits;
  g_rom_profiler.last_misses = rom_misses;
  g_rom_profiler.last_swaps = rom_swaps;
  const size_t rom_total = delta_hits + delta_misses;
  const double rom_hit_rate = rom_total ? (static_cast<double>(delta_hits) * 100.0 / static_cast<double>(rom_total)) : 0.0;

  uint32_t queue_depth = 0;
  if(priv.frame_queue != nullptr) {
    queue_depth = uxQueueMessagesWaiting(priv.frame_queue);
  }

#if ENABLE_SOUND
  const size_t audio_backlog = audio_queue_count;
#else
  const size_t audio_backlog = 0;
#endif

  const int cgb_double_speed = gb.cgb.speed_double ? 1 : 0;

  Serial.printf(
    "[PROF] fps=%.2f frame(avg=%.1f max=%llu) poll=%.1f emu=%.1f handoff=%.1f idle=%.1f/%.1f render=%.1f/%.1f (rows=%.1f seg=%.1f) over=%u/%u queue=%u rom=%.1f%% (H=%u M=%u S=%u) audioQ=%u swapFb=%d cgb2x=%d\n",
    fps,
    avg_frame,
    static_cast<unsigned long long>(g_main_profiler.max_frame_us),
    avg_poll,
    avg_emu,
    avg_dispatch,
    avg_idle,
    avg_idle_requested,
    avg_render,
    max_render,
    avg_render_rows,
    avg_render_segments,
    g_main_profiler.over_budget_frames,
    g_main_profiler.frames,
    static_cast<unsigned>(queue_depth),
    rom_hit_rate,
    static_cast<unsigned>(delta_hits),
    static_cast<unsigned>(delta_misses),
  static_cast<unsigned>(delta_swaps),
    static_cast<unsigned>(audio_backlog),
    (int)swap_fb_enabled,
    cgb_double_speed);

  profiler_reset_main(now);
}

static void profiler_record_frame(uint64_t frame_us,
                                  uint64_t poll_us,
                                  uint64_t emu_us,
                                  uint64_t dispatch_us,
                                  uint64_t idle_us,
                                  uint64_t requested_idle_us,
                                  bool over_budget,
                                  uint64_t now) {
  g_main_profiler.frames++;
  g_main_profiler.accum_frame_us += frame_us;
  g_main_profiler.accum_poll_us += poll_us;
  g_main_profiler.accum_emu_us += emu_us;
  g_main_profiler.accum_dispatch_us += dispatch_us;
  g_main_profiler.accum_idle_us += idle_us;
  g_main_profiler.accum_requested_idle_us += requested_idle_us;
  if(frame_us > g_main_profiler.max_frame_us) {
    g_main_profiler.max_frame_us = frame_us;
  }
  if(over_budget) {
    g_main_profiler.over_budget_frames++;
  }

  if(g_main_profiler.last_log_us == 0) {
    g_main_profiler.last_log_us = now;
    return;
  }

  if(now - g_main_profiler.last_log_us >= PROFILER_LOG_INTERVAL_US) {
    profiler_log(now);
  }
}
#endif
# 3906 "/home/jedld/workspace/Gameboy-Enhanced-Firmware-m5stack-cardputer-/gb_cardputer.ino"
void set_font_size(int size) {
  const int height = M5Cardputer.Display.height();
  int textsize = height / size;
  if(textsize <= 0) {
    textsize = 1;
  }
  M5Cardputer.Display.setTextSize(textsize);
}

static bool is_escape_key_char(char key) {
  switch(key) {
    case 0x1B:
    case '`':
    case '~':
      return true;
    default:
      return false;
  }
}

static bool keys_state_contains_escape(const Keyboard_Class::KeysState &status) {
  for(char key : status.word) {
    if(is_escape_key_char(key)) {
      return true;
    }
  }

  for(uint8_t hid : status.hid_keys) {
    if(hid == 0x29) {
      return true;
    }
  }
  return false;
}

static String format_binding_label(uint8_t key) {
  if(key == 0) {
    return String("Unbound");
  }
  if(key == ' ') {
    return String("Space");
  }
  if(key == '\t') {
    return String("Tab");
  }
  if(key == '\r' || key == '\n') {
    return String("Enter");
  }
  if(is_escape_key_char(static_cast<char>(key))) {
    return String("Esc");
  }
  if(key >= 32 && key <= 126) {
    char display = static_cast<char>(key);
    if(std::isalpha(static_cast<unsigned char>(display))) {
      display = static_cast<char>(std::toupper(static_cast<unsigned char>(display)));
    }
    char buffer[2] = { display, '\0' };
    return String(buffer);
  }
  char buffer[8];
  snprintf(buffer, sizeof(buffer), "0x%02X", static_cast<unsigned>(key));
  return String(buffer);
}

static const char* frame_skip_mode_label(uint8_t mode_value) {
  FrameSkipMode mode = static_cast<FrameSkipMode>(mode_value);
  switch(mode) {
    case FRAME_SKIP_MODE_AUTO:
      return "Auto";
    case FRAME_SKIP_MODE_FORCED:
      return "Forced";
    case FRAME_SKIP_MODE_DISABLED:
      return "Off";
    default:
      return "Auto";
  }
}

static void adjust_frame_skip_mode(int delta) {
  if(delta == 0) {
    return;
  }

  int mode = static_cast<int>(g_settings.frame_skip_mode);
  const int count = static_cast<int>(FRAME_SKIP_MODE_COUNT);
  mode = (mode + (delta % count) + count) % count;
  g_settings.frame_skip_mode = static_cast<uint8_t>(mode);
}

static void show_keymap_menu() {
  const uint8_t reset_index = JOYPAD_BUTTON_COUNT;
  const uint8_t back_index = JOYPAD_BUTTON_COUNT + 1;
  const uint8_t option_count = back_index + 1;

  uint8_t selection = 0;
  bool redraw = true;
  bool done = false;
  String status_message;
  bool keymap_changed = false;

  while(!done) {
    if(redraw) {
      redraw = false;
      M5Cardputer.Display.clearDisplay();
      set_font_size(84);
      M5Cardputer.Display.setCursor(0, 0);
      M5Cardputer.Display.println("Button Mapping");

      auto draw_option = [&](uint8_t index, const String &label) {
        String line = (selection == index) ? ("> " + label) : ("  " + label);
        M5Cardputer.Display.println(line);
      };

      for(size_t i = 0; i < JOYPAD_BUTTON_COUNT; ++i) {
        String label = String(JOYPAD_BUTTON_LABELS[i]) + ": " +
                       format_binding_label(g_settings.button_mapping[i]);
        draw_option(static_cast<uint8_t>(i), label);
      }

      draw_option(reset_index, "Reset to defaults");
      draw_option(back_index, "Back");

      if(status_message.length() > 0) {
        M5Cardputer.Display.println(status_message);
      }

      set_font_size(200);
      M5Cardputer.Display.println("J/S=Down  K/W=Up");
      M5Cardputer.Display.println("L/ENTER=Select  H=Prev");
    }

    M5Cardputer.update();
    if(!M5Cardputer.Keyboard.isPressed()) {
      delay(30);
      continue;
    }

    Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
    bool handled = false;

    auto bump_selection = [&](int delta) {
      selection = static_cast<uint8_t>((selection + option_count + delta) % option_count);
      redraw = true;
    };

    auto confirm_selection = [&]() {
      wait_for_keyboard_release();
      if(selection < JOYPAD_BUTTON_COUNT) {
        const size_t button_index = selection;
        M5Cardputer.Display.clearDisplay();
        set_font_size(84);
        M5Cardputer.Display.setCursor(0, 0);
        String prompt = "Press new key for ";
        prompt += JOYPAD_BUTTON_LABELS[button_index];
        M5Cardputer.Display.println(prompt);
        M5Cardputer.Display.println();
        set_font_size(180);
        M5Cardputer.Display.println("ENTER=Cancel");

        bool assigned = false;
        while(!assigned) {
          M5Cardputer.update();
          if(M5Cardputer.Keyboard.isPressed()) {
            Keyboard_Class::KeysState capture = M5Cardputer.Keyboard.keysState();
            if(capture.enter) {
              wait_for_keyboard_release();
              status_message = "Mapping cancelled";
              redraw = true;
              return;
            }
            if(!capture.word.empty()) {
              char new_key = capture.word.front();
              wait_for_keyboard_release();
              uint8_t normalized = static_cast<uint8_t>(new_key);
              if(std::isalpha(static_cast<unsigned char>(normalized))) {
                normalized = static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(normalized)));
              }
              g_settings.button_mapping[button_index] = normalized;
              keymap_changed = true;

              bool duplicate = false;
              for(size_t i = 0; i < JOYPAD_BUTTON_COUNT; ++i) {
                if(i == button_index) {
                  continue;
                }
                if(g_settings.button_mapping[i] == normalized) {
                  duplicate = true;
                  break;
                }
              }

              status_message = String(JOYPAD_BUTTON_LABELS[button_index]) +
                               " -> " + format_binding_label(normalized);
              if(duplicate) {
                status_message += " (shared)";
              }

              redraw = true;
              assigned = true;
            }
          }
          delay(20);
        }
      } else if(selection == reset_index) {
        apply_default_button_mapping();
        status_message = "Mapping reset to defaults";
        keymap_changed = true;
        redraw = true;
      } else {
        done = true;
      }
    };


    if(keys_state_contains_escape(status)) {
      wait_for_keyboard_release();
      done = true;
      handled = true;
    } else {
      for(auto key : status.word) {
        switch(key) {
          case ';':
          case ',':
          case 'k':
          case 'K':
          case 'w':
          case 'W':
            bump_selection(-1);
            handled = true;
            break;
          case '.':
          case 'j':
          case 'J':
          case 's':
          case 'S':
            bump_selection(1);
            handled = true;
            break;
          case 'h':
          case 'H':
          case 'a':
          case 'A':
            bump_selection(-1);
            handled = true;
            break;
          case 'l':
          case 'L':
            confirm_selection();
            handled = true;
            break;
          default:
            break;
        }
      }
    }

    if(status.enter) {
      confirm_selection();
      handled = true;
    }

    if(handled) {
      delay(160);
    } else {
      delay(30);
    }
  }

  M5Cardputer.Display.clearDisplay();

  if(keymap_changed) {
    apply_settings_constraints();
    save_settings_to_sd();
  }
}

static void show_options_menu() {
  if(g_settings.rom_cache_banks == 0 || g_settings.rom_cache_banks > ROM_CACHE_BANK_MAX) {
    g_settings.rom_cache_banks = 2;
  }

  enum Option : uint8_t {
    OPTION_AUDIO = 0,
    OPTION_BOOTSTRAP = 1,
    OPTION_CACHE = 2,
    OPTION_VOLUME = 3,
    OPTION_FRAME_SKIP = 4,
    OPTION_KEYMAP = 5,
    OPTION_DONE = 6,
    OPTION_COUNT
  };

  uint8_t selection = OPTION_AUDIO;
  bool redraw = true;
  bool done = false;
  bool settings_changed = false;

  while(!done) {
    if(redraw) {
      redraw = false;
      M5Cardputer.Display.clearDisplay();
      set_font_size(84);
      M5Cardputer.Display.setCursor(0, 0);
      M5Cardputer.Display.println("Options");

      auto draw_option = [&](Option opt, const char *label, const String &value) {
        String line = value.length() ? (String(label) + ": " + value) : String(label);
        if(selection == opt) {
          line = "> " + line;
        } else {
          line = "  " + line;
        }
        M5Cardputer.Display.println(line);
      };

      draw_option(OPTION_AUDIO, "Audio", g_settings.audio_enabled ? "On" : "Off");
      draw_option(OPTION_BOOTSTRAP,
                  "CGB auto palettes",
                  g_settings.cgb_bootstrap_palettes ? "On" : "Off");
      draw_option(OPTION_CACHE, "ROM cache banks", String(g_settings.rom_cache_banks));
    draw_option(OPTION_VOLUME, "Volume", String(g_settings.master_volume));
    draw_option(OPTION_FRAME_SKIP,
          "Frame skip",
          String(frame_skip_mode_label(g_settings.frame_skip_mode)));
      draw_option(OPTION_KEYMAP, "Configure buttons", "");
      draw_option(OPTION_DONE, "Back", "");

  set_font_size(200);
  M5Cardputer.Display.println("J/S=Down  K/W=Up  ESC=Back");
  M5Cardputer.Display.println("L/ENTER=Next  H=Prev  (cache/frame skip)");
    }

    M5Cardputer.update();
    if(!M5Cardputer.Keyboard.isPressed()) {
      delay(30);
      continue;
    }

    Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

    auto bump_selection = [&](int delta) {
      selection = static_cast<uint8_t>((selection + OPTION_COUNT + delta) % OPTION_COUNT);
      redraw = true;
    };

    auto confirm_selection = [&]() {
      switch(selection) {
        case OPTION_AUDIO:
          g_settings.audio_enabled = !g_settings.audio_enabled;
          settings_changed = true;
          redraw = true;
          break;
        case OPTION_BOOTSTRAP:
          g_settings.cgb_bootstrap_palettes = !g_settings.cgb_bootstrap_palettes;
          settings_changed = true;
          redraw = true;
          break;
        case OPTION_CACHE:
          g_settings.rom_cache_banks++;
          if(g_settings.rom_cache_banks == 0 || g_settings.rom_cache_banks > ROM_CACHE_BANK_MAX) {
            g_settings.rom_cache_banks = 1;
          }
          settings_changed = true;
          redraw = true;
          break;
        case OPTION_VOLUME:
          adjust_master_volume(static_cast<int>(VOLUME_STEP), false, true);
          settings_changed = true;
          redraw = true;
          break;
        case OPTION_FRAME_SKIP:
          adjust_frame_skip_mode(1);
          settings_changed = true;
          redraw = true;
          break;
        case OPTION_KEYMAP:
          wait_for_keyboard_release();
          show_keymap_menu();
          redraw = true;
          break;
        case OPTION_DONE:
          wait_for_keyboard_release();
          done = true;
          break;
      }
    };

    auto handle_left_input = [&]() {
      if(selection == OPTION_CACHE) {
        if(g_settings.rom_cache_banks <= 1) {
          g_settings.rom_cache_banks = ROM_CACHE_BANK_MAX;
        } else {
          g_settings.rom_cache_banks--;
        }
        settings_changed = true;
        redraw = true;
      } else if(selection == OPTION_VOLUME) {
        adjust_master_volume(-static_cast<int>(VOLUME_STEP), false, true);
        settings_changed = true;
        redraw = true;
      } else if(selection == OPTION_FRAME_SKIP) {
        adjust_frame_skip_mode(-1);
        settings_changed = true;
        redraw = true;
      }
    };

    bool handled = false;

    if(keys_state_contains_escape(status)) {
      wait_for_keyboard_release();
      done = true;
      handled = true;
    } else {
      for(auto key : status.word) {
        switch(key) {
          case ';':
          case ',':
          case 'k':
          case 'K':
          case 'w':
          case 'W':
            bump_selection(-1);
            handled = true;
            break;
          case '.':
          case 'j':
          case 'J':
          case 's':
          case 'S':
            bump_selection(1);
            handled = true;
            break;
          case 'h':
          case 'H':
          case 'a':
          case 'A':
            handle_left_input();
            handled = true;
            break;
          case 'l':
          case 'L':
            confirm_selection();
            handled = true;
            break;
          default:
            break;
        }
      }
    }

    if(status.enter) {
      confirm_selection();
      handled = true;
    }

    if(handled) {
      delay(150);
    } else {
      delay(30);
    }
  }

  if(settings_changed) {
    apply_settings_constraints();
    save_settings_to_sd();
  }

  if(g_settings.rom_cache_banks == 0) {
    g_settings.rom_cache_banks = 3;
  }

  M5Cardputer.Display.clearDisplay();
}

static void show_home_menu() {
  const uint8_t OPTION_LAUNCH = 0;
  const uint8_t OPTION_OPTIONS = 1;
  const uint8_t OPTION_COUNT = 2;

  uint8_t selection = OPTION_LAUNCH;
  bool redraw = true;
  bool launch_selected = false;

  while(!launch_selected) {
    if(redraw) {
      redraw = false;
      M5Cardputer.Display.clearDisplay();
      set_font_size(84);
      M5Cardputer.Display.setCursor(0, 0);
      M5Cardputer.Display.println("Main Menu");

      auto draw_option = [&](uint8_t index, const String &label) {
        String line = (selection == index) ? ("> " + label) : ("  " + label);
        M5Cardputer.Display.println(line);
      };

      draw_option(OPTION_LAUNCH, "Launch ROM browser");
      draw_option(OPTION_OPTIONS, "Options");

  set_font_size(200);
      M5Cardputer.Display.println();
      M5Cardputer.Display.println("Audio: " + String(g_settings.audio_enabled ? "On" : "Off"));
      M5Cardputer.Display.println("Cache banks: " + String(g_settings.rom_cache_banks));
  M5Cardputer.Display.println("Volume: " + String(g_settings.master_volume));
      M5Cardputer.Display.println();
  M5Cardputer.Display.println("J/S=Down  K/W=Up");
  M5Cardputer.Display.println("L/ENTER=Select  H=Prev");
    }

    M5Cardputer.update();
    if(!M5Cardputer.Keyboard.isPressed()) {
      delay(30);
      continue;
    }

    Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

    auto bump_selection = [&](int delta) {
      selection = static_cast<uint8_t>((selection + OPTION_COUNT + delta) % OPTION_COUNT);
      redraw = true;
    };

    auto confirm_selection = [&]() {
      wait_for_keyboard_release();
      if(selection == OPTION_LAUNCH) {
        launch_selected = true;
      } else {
        show_options_menu();
        redraw = true;
      }
    };

    bool handled = false;


    if(keys_state_contains_escape(status)) {
      handled = true;
    } else {
      for(auto key : status.word) {
        switch(key) {
          case ';':
          case ',':
          case 'k':
          case 'K':
          case 'w':
          case 'W':
            bump_selection(-1);
            handled = true;
            break;
          case '.':
          case 'j':
          case 'J':
          case 's':
          case 'S':
            bump_selection(1);
            handled = true;
            break;
          case 'h':
          case 'H':
          case 'a':
          case 'A':
            bump_selection(-1);
            handled = true;
            break;
          case 'l':
          case 'L':
            confirm_selection();
            handled = true;
            break;
          default:
            break;
        }
      }
    }

    if(status.enter) {
      confirm_selection();
      handled = true;
    }

    if(handled) {
      delay(150);
    } else {
      delay(30);
    }
  }

  M5Cardputer.Display.clearDisplay();
}

static bool has_rom_extension(const String &file_name) {
  int dot_index = file_name.lastIndexOf('.');
  if(dot_index < 0) {
    return false;
  }

  String extension = file_name.substring(dot_index + 1);
  extension.toLowerCase();
  return extension.equals("gb") || extension.equals("gbc");
}



static bool try_display_box_art(const String &rom_path, int x, int y, int max_width, int max_height) {
  int dot_index = rom_path.lastIndexOf('.');
  if(dot_index < 0) {
    return false;
  }

  String base_path = rom_path.substring(0, dot_index);
  const String art_extensions[] = {".png", ".jpg", ".jpeg"};

  for(const String &ext : art_extensions) {
    String art_path = base_path + ext;
    if(!SD.exists(art_path.c_str())) {
      continue;
    }

    bool drawn = false;
    if(ext.equals(".png")) {
      drawn = M5Cardputer.Display.drawPngFile(art_path.c_str(), x, y, max_width, max_height);
    } else {
      drawn = M5Cardputer.Display.drawJpgFile(art_path.c_str(), x, y, max_width, max_height);
    }

    if(drawn) {
      return true;
    }
  }

  return false;
}


char* file_picker() {
  struct Entry {
    String name;
    bool isDirectory;
    bool isEmbedded;
    size_t embeddedIndex;
    const EmbeddedRomEntry *embeddedEntry;
  };

  g_file_picker_cancelled = false;

  char current_path[MAX_PATH_LEN];
  strncpy(current_path, "/", MAX_PATH_LEN);
  current_path[MAX_PATH_LEN - 1] = '\0';

  while(true) {
    File dir = SD.open(current_path);
    const bool dir_opened = static_cast<bool>(dir);
    const bool at_root = strcmp(current_path, "/") == 0;

  const size_t embedded_count = kEmbeddedRomCount;

  if(!dir_opened && !(at_root && embedded_count > 0)) {
      debugPrint("Dir open fail");
      delay(500);
      strncpy(current_path, "/", MAX_PATH_LEN);
      current_path[MAX_PATH_LEN - 1] = '\0';
      continue;
    }

    std::vector<Entry> entries;
    entries.reserve(16);

    bool has_parent = dir_opened && strcmp(current_path, "/") != 0;
    if(has_parent) {
      entries.push_back({String(".."), true, false, SIZE_MAX, nullptr});
    }

    if(at_root && embedded_count > 0) {
      for(size_t i = 0; i < embedded_count; ++i) {
        const EmbeddedRomEntry *entry = embedded_rom_get(i);
        if(entry == nullptr) {
          continue;
        }

        const char *label = nullptr;
        if(entry->name != nullptr && entry->name[0] != '\0') {
          label = entry->name;
        } else if(entry->id != nullptr && entry->id[0] != '\0') {
          label = entry->id;
        } else {
          label = "Embedded ROM";
        }

        entries.push_back({String(label), false, true, i, entry});
      }
    }

    if(dir_opened) {
      while(true) {
        File entry = dir.openNextFile();
        if(!entry) {
          break;
        }

  Entry e{String(entry.name()), entry.isDirectory(), false, SIZE_MAX, nullptr};
        if(e.isDirectory || has_rom_extension(e.name)) {
          entries.push_back(e);
          if(entries.size() >= MAX_FILES) {
            entry.close();
            break;
          }
        }
        entry.close();
      }
      dir.close();
    }

    if(entries.empty()) {
      if(!has_parent) {
        M5Cardputer.Display.clearDisplay();
        M5Cardputer.Display.printf("No ROMs found!\n");
        M5Cardputer.Display.printf("Place ROMs on SD card\n");
        delay(1500);
        M5Cardputer.Display.printf("Press Home for ROMs ->");
        while(!M5Cardputer.Keyboard.isKeyPressed('.')) {
          if(M5Cardputer.Keyboard.isKeyPressed('`')) {
            M5Cardputer.Display.clearDisplay();
            M5Cardputer.Display.printf("Open the QR code using your camera!\n");
            M5Cardputer.Display.println("https://tinyurl.com/444jzbs2");
            delay(2000);
            delay(4000);
            M5Cardputer.Display.clearDisplay();
            M5Cardputer.Display.printf("Reloading file picker...\n");
            delay(250);
            ESP.restart();
          }
        }
        delay(1000);
        M5.Lcd.qrcode("https://tinyurl.com/444jzbs2", 30, 20, 180, 0);
      } else {
        char *slash = strrchr(current_path, '/');
        if(slash != NULL) {
          if(slash == current_path) {
            current_path[1] = '\0';
          } else {
            *slash = '\0';
          }
        }
      }
      delay(200);
      continue;
    }

    int select_index = 0;
    bool redraw = true;
    bool selection_made = false;
    bool esc_pressed = false;

    while(!selection_made && !esc_pressed) {
      M5Cardputer.update();
      if(M5Cardputer.Keyboard.isPressed()) {
        Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

        if(keys_state_contains_escape(status)) {
          esc_pressed = true;
          delay(160);
        } else {
          for(auto key : status.word) {
            switch(key) {
              case ';':
              case ',':
                select_index--;
                redraw = true;
                delay(180);
                break;
              case '.':
                select_index++;
                redraw = true;
                delay(180);
                break;
              case 'k':
              case 'K':
              case 'w':
              case 'W':
                select_index--;
                redraw = true;
                delay(160);
                break;
              case 'j':
              case 'J':
              case 's':
              case 'S':
                select_index++;
                redraw = true;
                delay(160);
                break;
              case 'l':
              case 'L':
                selection_made = true;
                delay(160);
                break;
              default:
                break;
            }
          }

          if(status.enter) {
            selection_made = true;
          }
        }
      }

      int entry_count = entries.size();
      if(entry_count == 0) {
        break;
      }

      if(select_index < 0) {
        select_index = entry_count - 1;
        redraw = true;
      } else if(select_index >= entry_count) {
        select_index = 0;
        redraw = true;
      }

      if(redraw) {
        redraw = false;

        const int dispW = M5Cardputer.Display.width();
        const int dispH = M5Cardputer.Display.height();
        set_font_size(128);
        const int fontH = M5Cardputer.Display.fontHeight();
        int lineHeight = fontH + 6;
        if(lineHeight <= 0) {
          lineHeight = fontH + 6;
        }

        int visible_rows = dispH / lineHeight;
        if(visible_rows < 3) {
          visible_rows = 3;
        }
        if(visible_rows > entry_count) {
          visible_rows = entry_count;
        }

        int start_index = select_index - visible_rows / 2;
        if(start_index < 0) {
          start_index = 0;
        }
        if(start_index + visible_rows > entry_count) {
          start_index = entry_count - visible_rows;
          if(start_index < 0) {
            start_index = 0;
          }
        }

        const int listHeight = lineHeight * visible_rows;
        const int top = (dispH - listHeight) / 2;
        const int textPadding = 10;

        const uint16_t bg_default = M5Cardputer.Display.color565(0, 0, 0);
        const uint16_t highlight_color = M5Cardputer.Display.color565(160, 190, 255);
        const uint16_t fg_default = M5Cardputer.Display.color565(210, 210, 210);
        const uint16_t fg_selected = M5Cardputer.Display.color565(255, 255, 255);

        bool background_drawn = false;
        if(select_index >= 0 && select_index < entry_count) {
          const Entry &selected_entry = entries[select_index];
          if(!selected_entry.isDirectory && !selected_entry.isEmbedded && has_rom_extension(selected_entry.name)) {
            String full_path;
            if(strcmp(current_path, "/") == 0) {
              full_path = "/" + selected_entry.name;
            } else {
              full_path = String(current_path) + "/" + selected_entry.name;
            }
            background_drawn = try_display_box_art(full_path, 0, 0, dispW, dispH);
          }
        }

        if(!background_drawn) {
          M5Cardputer.Display.fillScreen(bg_default);
        }

        for(int row = 0; row < visible_rows; ++row) {
          int entry_idx = start_index + row;
          if(entry_idx >= entry_count) {
            break;
          }

          bool is_selected = (entry_idx == select_index);
          int y = top + row * lineHeight;
          uint16_t row_fg = is_selected ? fg_selected : fg_default;

          if(is_selected) {
            M5Cardputer.Display.drawRoundRect(2, y, dispW - 4, lineHeight, 6, highlight_color);
          }

          M5Cardputer.Display.setTextColor(row_fg);

          String label = entries[entry_idx].name;
          if(entries[entry_idx].isDirectory && !entries[entry_idx].isEmbedded && label != "..") {
            label += "/";
          }

          String indicator;
          if(entries[entry_idx].isEmbedded) {
            const bool is_autoboot = entries[entry_idx].embeddedEntry != nullptr && entries[entry_idx].embeddedEntry->autoboot;
            indicator = is_autoboot ? "[FW★] " : "[FW] ";
          } else if(entries[entry_idx].isDirectory) {
            indicator = (label == "..") ? "[UP] " : "[DIR] ";
          }

          String render_text = indicator + label;
          bool shortened = false;
          const int max_text_width = dispW - (textPadding * 2) - (is_selected ? M5Cardputer.Display.textWidth("> ") : 0);
          while(render_text.length() > 1 && M5Cardputer.Display.textWidth(render_text.c_str()) > max_text_width) {
            render_text.remove(render_text.length() - 1);
            shortened = true;
          }
          if(shortened && render_text.length() > 3) {
            render_text.remove(render_text.length() - 3);
            render_text += "...";
          }

          int textY = y + (lineHeight - fontH) / 2;
          if(textY < y) {
            textY = y;
          }

          int cursorX = textPadding;
          if(is_selected) {
            M5Cardputer.Display.setCursor(cursorX, textY);
            M5Cardputer.Display.print("> ");
            cursorX += M5Cardputer.Display.textWidth("> ");
          }

          M5Cardputer.Display.setCursor(cursorX, textY);
          M5Cardputer.Display.print(render_text);
        }

        M5Cardputer.Display.setTextColor(M5Cardputer.Display.color565(255, 255, 255));
      }
    }

    if(entries.empty()) {
      continue;
    }


    if(esc_pressed) {
      if(strcmp(current_path, "/") == 0) {

        g_file_picker_cancelled = true;
        return NULL;
      } else {

        size_t len = strlen(current_path);
        if(len > 1) {
          char *slash = strrchr(current_path, '/');
          if(slash != NULL) {
            if(slash == current_path) {
              current_path[1] = '\0';
            } else {
              *slash = '\0';
            }
          }
        }
        delay(150);
        continue;
      }
    }

    Entry chosen = entries[select_index];

    if(chosen.isEmbedded) {
      if(chosen.embeddedEntry == nullptr) {
        debugPrint("FW entry missing");
        delay(500);
        continue;
      }

      constexpr const char *kEmbeddedPrefix = ":embedded/";
      char index_buf[32];
      int written = snprintf(index_buf, sizeof(index_buf), "%zu", chosen.embeddedIndex);
      if(written <= 0 || static_cast<size_t>(written) >= sizeof(index_buf)) {
        debugPrint("FW index err");
        delay(500);
        continue;
      }

      const size_t needed = strlen(kEmbeddedPrefix) + static_cast<size_t>(written) + 1;
      char *selected_path = (char*)malloc(needed);
      if(selected_path == NULL) {
        debugPrint("Path alloc fail");
        delay(500);
        continue;
      }

      snprintf(selected_path, needed, "%s%zu", kEmbeddedPrefix, chosen.embeddedIndex);
      g_file_picker_cancelled = false;
      return selected_path;
    }

    if(chosen.isDirectory) {
      if(chosen.name == "..") {
        size_t len = strlen(current_path);
        if(len > 1) {
          char *slash = strrchr(current_path, '/');
          if(slash != NULL) {
            if(slash == current_path) {
              current_path[1] = '\0';
            } else {
              *slash = '\0';
            }
          }
        }
      } else {
        char next_path[MAX_PATH_LEN];
        if(strcmp(current_path, "/") == 0) {
          snprintf(next_path, MAX_PATH_LEN, "/%s", chosen.name.c_str());
        } else {
          snprintf(next_path, MAX_PATH_LEN, "%s/%s", current_path, chosen.name.c_str());
        }
        strncpy(current_path, next_path, MAX_PATH_LEN - 1);
        current_path[MAX_PATH_LEN - 1] = '\0';
      }
      delay(150);
      continue;
    }

    size_t needed = strlen(current_path) + 1 + chosen.name.length() + 1;
    char *selected_path = (char*)malloc(needed);
    if(selected_path == NULL) {
      debugPrint("Path alloc fail");
      delay(500);
      continue;
    }

    if(strcmp(current_path, "/") == 0) {
      snprintf(selected_path, needed, "/%s", chosen.name.c_str());
    } else {
      snprintf(selected_path, needed, "%s/%s", current_path, chosen.name.c_str());
    }

    g_file_picker_cancelled = false;
    return selected_path;
  }
}

#if ENABLE_SOUND
static constexpr size_t AUDIO_BUFFER_COUNT = 4;
static constexpr size_t AUDIO_TARGET_QUEUE = 3;
static constexpr uint32_t AUDIO_REQUESTED_SAMPLE_RATE = AUDIO_DEFAULT_SAMPLE_RATE;

static int16_t *audio_buffers[AUDIO_BUFFER_COUNT];
static uint8_t audio_buffer_state[AUDIO_BUFFER_COUNT];
static size_t audio_queue[AUDIO_BUFFER_COUNT];
static size_t audio_queue_head = 0;
static size_t audio_queue_tail = 0;
static size_t audio_next_fill = 0;
static bool audio_initialised = false;
static bool audio_engine_initialised = false;

static int16_t *audio_alloc_dma_buffer(size_t bytes) {
  const uint32_t caps_order[] = {
    MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT,
    MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT,
    MALLOC_CAP_DMA | MALLOC_CAP_8BIT
  };

  const size_t first_index = g_psram_available ? 0 : 1;
  for(size_t i = first_index; i < sizeof(caps_order) / sizeof(caps_order[0]); ++i) {
    int16_t *ptr = reinterpret_cast<int16_t *>(heap_caps_malloc(bytes, caps_order[i]));
    if(ptr != nullptr) {
      return ptr;
    }
  }

  return nullptr;
}

static void audio_reset_buffers() {
  for(size_t i = 0; i < AUDIO_BUFFER_COUNT; ++i) {
    if(audio_buffers[i] != nullptr) {
      heap_caps_free(audio_buffers[i]);
      audio_buffers[i] = nullptr;
    }
    audio_buffer_state[i] = 0;
  }
  memset(audio_queue, 0, sizeof(audio_queue));
  audio_queue_head = audio_queue_tail = audio_queue_count = 0;
  audio_next_fill = 0;
}

static uint32_t audio_select_sample_rate() {
  if(g_psram_available) {
    return AUDIO_REQUESTED_SAMPLE_RATE;
  }
  return 16384u;
}

static void audioCoreInit(uint32_t sample_rate) {
  audio_set_sample_rate(sample_rate);
  if(!audio_engine_initialised) {
    audio_init();
    audio_engine_initialised = true;
  }
}

static void audioTeardown() {
  audio_reset_buffers();
  if(audio_initialised) {
    M5Cardputer.Speaker.end();
    audio_initialised = false;
  }
}

static void audio_release_finished() {
  const size_t playing = M5Cardputer.Speaker.isPlaying(0);
  while(audio_queue_count > playing) {
    const size_t idx = audio_queue[audio_queue_head];
    audio_queue_head = (audio_queue_head + 1) % AUDIO_BUFFER_COUNT;
    audio_queue_count--;
    audio_buffer_state[idx] = 0;
  }
}

static int audio_acquire_buffer() {
  for(size_t attempt = 0; attempt < AUDIO_BUFFER_COUNT; ++attempt) {
    const size_t idx = (audio_next_fill + attempt) % AUDIO_BUFFER_COUNT;
    if(audio_buffer_state[idx] == 0 && audio_buffers[idx] != nullptr) {
      audio_buffer_state[idx] = 1;
      audio_next_fill = (idx + 1) % AUDIO_BUFFER_COUNT;
      return (int)idx;
    }
  }
  return -1;
}

static void audio_queue_push(size_t idx) {
  audio_queue[audio_queue_tail] = idx;
  audio_queue_tail = (audio_queue_tail + 1) % AUDIO_BUFFER_COUNT;
  audio_queue_count++;
}

static void apply_speaker_volume() {
  if(!audio_initialised) {
    return;
  }
  const uint8_t volume = g_settings.master_volume;
  M5Cardputer.Speaker.setVolume(volume);
  M5Cardputer.Speaker.setAllChannelVolume(volume);
}

static void audioSetup() {
  audioTeardown();

  const uint32_t requested_rate = audio_select_sample_rate();
  audioCoreInit(requested_rate);

  auto base_cfg = M5Cardputer.Speaker.config();
  auto cfg = base_cfg;
  cfg.sample_rate = requested_rate;
  cfg.stereo = true;
  cfg.magnification = 8;
  audio_set_sample_rate(requested_rate);
  Serial.printf("audioSetup: requested sample rate %u Hz (psram=%s)\n",
                (unsigned)requested_rate,
                g_psram_available ? "yes" : "no");
  size_t dma_len = audio_samples_per_buffer();
  if(dma_len > 1024) {
    dma_len = 1024;
  }
  if(!g_psram_available) {
    if(dma_len > 512) {
      dma_len = 512;
    }
    cfg.dma_buf_count = 4;
  }
  dma_len &= ~1u;
  cfg.dma_buf_len = dma_len;
  if(g_psram_available) {
    cfg.dma_buf_count = 12;
  }
  cfg.task_priority = tskIDLE_PRIORITY + 4;
  cfg.task_pinned_core = 0;
  cfg.use_dac = false;

  cfg.pin_data_out = 42;
  cfg.pin_bck = 41;
  cfg.pin_ws = 43;
  cfg.pin_mck = -1;

  M5Cardputer.Speaker.end();
  M5Cardputer.Speaker.config(cfg);
  audio_initialised = M5Cardputer.Speaker.begin();
  Serial.printf("Speaker.begin (stereo) data_out=%d bck=%d ws=%d mck=%d -> %s\n",
                cfg.pin_data_out,
                cfg.pin_bck,
                cfg.pin_ws,
                cfg.pin_mck,
                audio_initialised ? "OK" : "FAIL");

  if(!audio_initialised) {
    cfg.pin_data_out = 42;
    cfg.stereo = false;
    M5Cardputer.Speaker.end();
    M5Cardputer.Speaker.config(cfg);
    audio_initialised = M5Cardputer.Speaker.begin();
    Serial.printf("Speaker.begin retry mono data_out=%d bck=%d ws=%d mck=%d -> %s\n",
                  cfg.pin_data_out,
                  cfg.pin_bck,
                  cfg.pin_ws,
                  cfg.pin_mck,
                  audio_initialised ? "OK" : "FAIL");
  }

  if(!audio_initialised) {
    Serial.println("Speaker.begin() failed; restoring default config and disabling audio");
    M5Cardputer.Speaker.config(base_cfg);
    audioTeardown();
    return;
  }

  const uint32_t actual_sample_rate = M5Cardputer.Speaker.config().sample_rate;
  if(actual_sample_rate != audio_get_sample_rate()) {
    Serial.printf("audioSetup: speaker adjusted sample rate %u -> %u\n",
                  (unsigned)audio_get_sample_rate(),
                  (unsigned)actual_sample_rate);
    audio_set_sample_rate(actual_sample_rate);
  }

  const size_t interleaved_samples = audio_samples_per_buffer();
  const size_t buffer_bytes = interleaved_samples * sizeof(int16_t);
  bool buffer_ok = true;
  Serial.printf("Audio memory (before alloc): internal DMA=%u bytes, psram DMA=%u bytes\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
  for(size_t i = 0; i < AUDIO_BUFFER_COUNT; ++i) {
    audio_buffers[i] = audio_alloc_dma_buffer(buffer_bytes);
    if(audio_buffers[i] == nullptr) {
      buffer_ok = false;
      break;
    }
    memset(audio_buffers[i], 0, buffer_bytes);
    audio_buffer_state[i] = 0;
  }

  if(!buffer_ok) {
    Serial.println("Audio buffer allocation failed; disabling audio");
    audioTeardown();
    return;
  }

  M5Cardputer.Speaker.setVolume(255);
  M5Cardputer.Speaker.setAllChannelVolume(255);

  Serial.printf("audioSetup: init=%d sample_rate=%u stereo=%d dma_len=%u dma_count=%u frames=%u\n",
                (int)audio_initialised,
                (unsigned)M5Cardputer.Speaker.config().sample_rate,
                (int)M5Cardputer.Speaker.config().stereo,
                (unsigned)M5Cardputer.Speaker.config().dma_buf_len,
                (unsigned)M5Cardputer.Speaker.config().dma_buf_count,
                (unsigned)audio_samples_per_frame());

  audio_release_finished();
  audioPump();
  apply_speaker_volume();
}

static void audioPump() {
  if(!audio_initialised) {
    static bool warned = false;
    if(!warned) {
      Serial.println("audioPump: audio not initialised");
      warned = true;
    }
    return;
  }

  audio_release_finished();

  while(audio_queue_count < AUDIO_TARGET_QUEUE) {
    const int buffer_index = audio_acquire_buffer();
    if(buffer_index < 0) {
      break;
    }

    int16_t *samples = audio_buffers[buffer_index];
    if(samples == nullptr) {
      audio_buffer_state[buffer_index] = 0;
      break;
    }
    const size_t interleaved_samples = audio_samples_per_buffer();
    if(interleaved_samples == 0) {
      audio_buffer_state[buffer_index] = 0;
      break;
    }

    const uint32_t sample_rate = audio_get_sample_rate();
    audio_callback(nullptr,
                   reinterpret_cast<uint8_t *>(samples),
                   interleaved_samples * sizeof(int16_t));

    bool queued = M5Cardputer.Speaker.playRaw(samples,
                                              interleaved_samples,
                                              sample_rate,
                                              true,
                                              1,
                                              0,
                                              false);
    if(!queued) {
      audio_release_finished();
      queued = M5Cardputer.Speaker.playRaw(samples,
                                           interleaved_samples,
                                           sample_rate,
                                           true,
                                           1,
                                           0,
                                           false);
    }

    if(!queued) {
      audio_buffer_state[buffer_index] = 0;
      break;
    }

    audio_queue_push((size_t)buffer_index);
  }
}
#endif

static bool gb_run_frame_watchdog(struct gb_s *gb,
                                  uint32_t max_steps,
                                  uint32_t *steps_executed) {
  if(steps_executed != nullptr) {
    *steps_executed = 0;
  }

  gb->gb_frame = 0;
  uint32_t steps = 0;
  while(!gb->gb_frame && steps < max_steps) {
    __gb_step_cpu(gb);
    steps++;
  }

  if(steps_executed != nullptr) {
    *steps_executed = steps;
  }
  return gb->gb_frame != 0;
}

static constexpr uint32_t GB_FRAME_STEP_BUDGET = 2000000;

void setup() {


  Serial.begin(115200);
  delay(100);
  Serial.println("Cardputer firmware booting...");
  configure_performance_profile();
  reset_save_state(&priv);
  g_printer.reset(true);

  bool psram_ok = psramInit();
  Serial.printf("PSRAM init: %s, size=%u bytes, free=%u bytes\n",
                psram_ok ? "OK" : "FAIL",
                (unsigned)ESP.getPsramSize(),
                (unsigned)ESP.getFreePsram());
  g_psram_available = psram_ok && ESP.getFreePsram() > 0;
  if(!psram_ok) {
    debugPrint("PSRAM init failed; using internal RAM only");
    delay(1500);
  }

  if(!g_psram_available && g_settings.rom_cache_banks > ROM_CACHE_BANK_LIMIT_NO_PSRAM) {
    Serial.printf("Limiting ROM cache banks to %u (no PSRAM)\n",
                  (unsigned)ROM_CACHE_BANK_LIMIT_NO_PSRAM);
    g_settings.rom_cache_banks = ROM_CACHE_BANK_LIMIT_NO_PSRAM;
  }
  apply_settings_constraints();


  auto cfg = M5.config();
  cfg.internal_spk = true;
  cfg.internal_mic = true;
  cfg.fallback_board = m5::board_t::board_M5Cardputer;

  M5Cardputer.begin(cfg, true);

#if ENABLE_SOUND

#endif


  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.initDMA();
  set_font_size(80);

  const size_t embedded_rom_count = kEmbeddedRomCount;
  const bool embedded_available = embedded_rom_count > 0;
  const EmbeddedRomEntry *autoboot_entry = embedded_rom_get_autoboot();

  debugPrint("Waiting for SD Card to Init...");
  const bool sd_required = !embedded_available;
  bool sd_available = ensure_sd_card(sd_required);
  if(!sd_available) {
    Serial.println("SD card not detected; continuing without SD support.");
  }

  if(sd_available) {
    if(!load_settings_from_sd()) {
      apply_settings_constraints();
      save_settings_to_sd();
    } else {
      apply_settings_constraints();
    }
  } else {
    apply_settings_constraints();
  }

  show_home_menu();

  gb.wram = alloc_gb_buffer(WRAM_TOTAL_SIZE, "WRAM", true);
  gb.vram = alloc_gb_buffer(VRAM_TOTAL_SIZE, "VRAM", true);
  gb.oam = alloc_gb_buffer(OAM_SIZE, "OAM", true);
  gb.hram_io = alloc_gb_buffer(HRAM_IO_SIZE, "HRAM/IO", true);
  if(!gb.wram || !gb.vram || !gb.oam || !gb.hram_io) {
    debugPrint("Out of memory");
    while(true) {
      delay(1000);
    }
  }

  if(!sd_available && !embedded_available) {
    debugPrint("SD card init failed");
    while(true) {
      delay(1000);
    }
  }


  enum gb_init_error_e ret = GB_INIT_NO_ERROR;
  while(true) {
    g_printer.reset(false);
    palette_apply_dmg(&priv.palette);
    priv.rom_is_cgb = false;
    priv.rom_is_cgb_only = false;
    priv.rom_cgb_flag = 0;
    priv.rom_cache.bank_count = g_settings.rom_cache_banks;
    rom_cache_reset(&priv.rom_cache);
    debugPrint("postInit");

    reset_save_state(&priv);

    debugPrint("Before filepick");

    bool rom_ready = false;
    rom_cache_close(&priv.rom_cache);
    priv.rom_source = RomSource::None;
    priv.embedded_rom_entry = nullptr;
    priv.embedded_rom = nullptr;
    priv.embedded_rom_size = 0;

    const EmbeddedRomEntry *single_embedded_entry = (embedded_rom_count == 1) ? embedded_rom_get(0) : nullptr;
    bool try_embedded_first = false;
    const EmbeddedRomEntry *boot_entry = nullptr;
    if(autoboot_entry != nullptr) {
      boot_entry = autoboot_entry;
      try_embedded_first = true;
    } else if(!sd_available && single_embedded_entry != nullptr) {
      boot_entry = single_embedded_entry;
      try_embedded_first = true;
    }

    bool rom_browser_cancelled = false;
    while(!rom_ready) {
      if(try_embedded_first) {
        try_embedded_first = false;
        if(boot_entry != nullptr && load_embedded_rom(&priv, boot_entry)) {
          if(priv.rom_is_cgb) {
            palette_disable_overrides(&priv.palette);
          } else {
            palette_configure_for_dmg(&priv);
          }

          const size_t rom_size = rom_source_size(&priv);
          Serial.printf("ROM size: %u bytes, CGB flag: 0x%02X\n",
                        (unsigned)rom_size,
                        (unsigned)priv.rom_cgb_flag);
          if(priv.rom_is_cgb_only) {
            Serial.println("Warning: ROM is marked as CGB-only; compatibility is limited.");
          }

          const char *mode_label = priv.rom_is_cgb_only ? "[CGB ONLY]" : (priv.rom_is_cgb ? "[CGB]" : "[DMG]");
          const EmbeddedRomEntry *loaded_entry = priv.embedded_rom_entry;
          const char *rom_name = "Embedded";
          if(loaded_entry != nullptr) {
            if(loaded_entry->name != nullptr && loaded_entry->name[0] != '\0') {
              rom_name = loaded_entry->name;
            } else if(loaded_entry->id != nullptr && loaded_entry->id[0] != '\0') {
              rom_name = loaded_entry->id;
            }
          }

          char info[96];
          snprintf(info,
                   sizeof(info),
                   "%s ready %s (%u KB)",
                   rom_name,
                   mode_label,
                   (unsigned)(rom_size / 1024));
          debugPrint(info);
          delay(250);
          if(priv.rom_is_cgb_only) {
            debugPrint("CGB-only ROM support is experimental");
            delay(500);
          }
          rom_ready = true;
          continue;
        } else {
          debugPrint("Embedded ROM unavailable");
          delay(1000);
          if(!sd_available && !embedded_available) {
            debugPrint("Insert SD card for ROMs");
            delay(1500);
          }
        }
      }

      if(!sd_available) {
        sd_available = ensure_sd_card(false);
        if(!sd_available && !embedded_available) {
          delay(200);
          continue;
        }
      }

      char* selected_file = file_picker();
      if(selected_file == NULL) {
        if(g_file_picker_cancelled) {
          g_file_picker_cancelled = false;
          rom_browser_cancelled = true;
          break;
        }
        debugPrint("No selection");
        delay(1000);
        continue;
      }

      constexpr const char *kEmbeddedPrefix = ":embedded/";
      const bool embedded_selected = (strncmp(selected_file, kEmbeddedPrefix, strlen(kEmbeddedPrefix)) == 0);
      if(embedded_selected) {
        const char *index_str = selected_file + strlen(kEmbeddedPrefix);
        char *endptr = nullptr;
        unsigned long selected_index = strtoul(index_str, &endptr, 10);
        const bool parse_ok = (endptr != index_str) && (*endptr == '\0');
        const EmbeddedRomEntry *selected_entry = nullptr;
        if(parse_ok) {
          selected_entry = embedded_rom_get(static_cast<size_t>(selected_index));
        }

        free(selected_file);

        if(selected_entry == nullptr) {
          debugPrint("FW entry missing");
          delay(1000);
          continue;
        }

        if(load_embedded_rom(&priv, selected_entry)) {
          if(priv.rom_is_cgb) {
            palette_disable_overrides(&priv.palette);
          } else {
            palette_configure_for_dmg(&priv);
          }

          const size_t rom_size = rom_source_size(&priv);
          const char *mode_label = priv.rom_is_cgb_only ? "[CGB ONLY]" : (priv.rom_is_cgb ? "[CGB]" : "[DMG]");
          const EmbeddedRomEntry *loaded_entry = priv.embedded_rom_entry;
          const char *rom_name = "Embedded";
          if(loaded_entry != nullptr) {
            if(loaded_entry->name != nullptr && loaded_entry->name[0] != '\0') {
              rom_name = loaded_entry->name;
            } else if(loaded_entry->id != nullptr && loaded_entry->id[0] != '\0') {
              rom_name = loaded_entry->id;
            }
          }

          char info[96];
          snprintf(info,
                   sizeof(info),
                   "%s ready %s (%u KB)",
                   rom_name,
                   mode_label,
                   (unsigned)(rom_size / 1024));
          debugPrint(info);
          delay(250);
          if(priv.rom_is_cgb_only) {
            debugPrint("CGB-only ROM support is experimental");
            delay(500);
          }
          rom_ready = true;
          continue;
        } else {
          debugPrint("Embedded ROM load failed");
          delay(1200);
          continue;
        }
      }

      debugPrint(selected_file);
      set_sd_rom_path(&priv, selected_file);
      bool opened = rom_cache_open(&priv.rom_cache, selected_file);
      free(selected_file);

      if(opened) {
        priv.rom_source = RomSource::SdCard;
        priv.embedded_rom_entry = nullptr;
        priv.embedded_rom = nullptr;
        priv.embedded_rom_size = 0;
        sd_available = true;

        priv.rom_cgb_flag = rom_cache_cgb_flag(&priv.rom_cache);
        priv.rom_is_cgb = (priv.rom_cgb_flag & 0x80) != 0;
        priv.rom_is_cgb_only = (priv.rom_cgb_flag == 0xC0);

        if(priv.rom_is_cgb) {
          palette_disable_overrides(&priv.palette);
        } else {
          palette_configure_for_dmg(&priv);
        }

        const size_t rom_size = rom_source_size(&priv);
        Serial.printf("ROM size: %u bytes, CGB flag: 0x%02X\n",
                      (unsigned)rom_size,
                      (unsigned)priv.rom_cgb_flag);
        if(priv.rom_is_cgb_only) {
          Serial.println("Warning: ROM is marked as CGB-only; compatibility is limited.");
        }

        const char *mode_label = priv.rom_is_cgb_only ? "[CGB ONLY]" : (priv.rom_is_cgb ? "[CGB]" : "[DMG]");
        char info[64];
        snprintf(info,
                 sizeof(info),
                 "ROM ready %s (%u KB)",
                 mode_label,
                 (unsigned)(rom_size / 1024));
        debugPrint(info);
        delay(250);
        if(priv.rom_is_cgb_only) {
          debugPrint("CGB-only ROM support is experimental");
          delay(500);
        }
        rom_ready = true;
      } else {
        clear_sd_rom_path(&priv);
        palette_apply_dmg(&priv.palette);
        priv.rom_source = RomSource::None;
        priv.embedded_rom_entry = nullptr;
        priv.embedded_rom = nullptr;
        priv.embedded_rom_size = 0;
        priv.rom_is_cgb = false;
        priv.rom_is_cgb_only = false;
        priv.rom_cgb_flag = 0;
        debugPrint("ROM open failed");
        delay(1500);
      }
    }

    if(rom_browser_cancelled) {
      show_home_menu();
      continue;
    }

    debugPrint("After filepick");

#if ENABLE_SOUND
    if(g_settings.audio_enabled) {
      audioCoreInit(audio_select_sample_rate());
    }
#endif

    ret = gb_init(&gb, &gb_rom_read, &gb_cart_ram_read, &gb_cart_ram_write, &gb_error, &priv);

    if(ret == GB_INIT_NO_ERROR) {
      break;
    }

    const char *error_message = nullptr;
    switch(ret) {
      case GB_INIT_CARTRIDGE_UNSUPPORTED:
        error_message = "Unsupported cartridge";
        break;
      case GB_INIT_INVALID_CHECKSUM:
        error_message = "ROM checksum fail";
        break;
      case GB_INIT_OUT_OF_MEMORY:
        error_message = "GB init OOM";
        break;
      default:
        error_message = "GB init error";
        break;
    }

    Serial.printf("gb_init failed with code %d\n", (int)ret);
    debugPrint(error_message);
    delay(1500);
    debugPrint("Returning to file picker...");
    delay(600);

    while(M5Cardputer.Keyboard.isPressed()) {
      M5Cardputer.update();
      delay(50);
    }

    if(priv.rom_source == RomSource::SdCard) {
      rom_cache_close(&priv.rom_cache);
    }
    priv.rom_source = RomSource::None;
    priv.embedded_rom = nullptr;
    priv.embedded_rom_size = 0;
  }

  if(ret == GB_INIT_NO_ERROR) {
    g_printer.reset(false);
    gb_init_serial(&gb, &gb_printer_serial_tx, &gb_printer_serial_rx);
  }

  if(ret == GB_INIT_NO_ERROR && priv.rom_is_cgb) {
    palette_disable_overrides(&priv.palette);
  }

  if(priv.rom_is_cgb) {
    gb_set_cgb_mode(&gb, 1);
    palette_disable_overrides(&priv.palette);


    for(size_t i = 0; i < 32; ++i) {
      const int component = (i % 4);
      uint32_t colour;
      switch(component) {
        case 0:
          colour = 0xF8F8F8;
          break;
        case 1:
          colour = 0xB8B8B8;
          break;
        case 2:
          colour = 0x686868;
          break;
        default:
          colour = 0x080808;
          break;
      }

      const uint16_t raw = rgb888_to_gb555(colour) & 0x7FFF;
      const size_t byte_index = i * 2;

      gb.display.cgb_bg_palette[i] = colour;
      gb.display.cgb_obj_palette[i] = colour;
      gb.cgb.bg_palette_data[byte_index] = raw & 0xFF;
      gb.cgb.bg_palette_data[byte_index + 1] = (raw >> 8) & 0xFF;
      gb.cgb.obj_palette_data[byte_index] = raw & 0xFF;
      gb.cgb.obj_palette_data[byte_index + 1] = (raw >> 8) & 0xFF;
    }

    gb.cgb.bg_palette_index = 0;
    gb.cgb.obj_palette_index = 0;
    gb.cgb.bg_palette_autoinc = 0;
    gb.cgb.obj_palette_autoinc = 0;
  }

#if ENABLE_MBC7

  if(gb.mbc == 7) {
    Serial.println("MBC7 cartridge detected - initializing accelerometer support");
    mbc7_cardputer_init(get_mbc7_state());
    gb.mbc7_accel_read = mbc7_cardputer_accel_read;
  }
#endif

#if ENABLE_LCD
  gb_init_lcd(&gb, &lcd_draw_line);

  gb.direct.interlace = 0;
  g_interlace_state = {};
#endif

#if ENABLE_LCD
  priv.write_fb_index = 0;
  priv.single_buffer_mode = false;
  priv.frame_queue = nullptr;
  priv.frame_buffer_free[0] = nullptr;
  priv.frame_buffer_free[1] = nullptr;
  priv.framebuffers[0] = nullptr;
  priv.framebuffers[1] = nullptr;
  memset(priv.framebuffer_row_hash, 0, sizeof(priv.framebuffer_row_hash));
  memset(priv.framebuffer_row_dirty, 0, sizeof(priv.framebuffer_row_dirty));
  const size_t fb_bytes = LCD_HEIGHT * LCD_WIDTH * sizeof(uint16_t);
  static constexpr uint32_t FB_CAPS_FAST[] = {
    MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT,
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
  };
  static constexpr uint32_t FB_CAPS_FALLBACK[] = {
    MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT,
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
    MALLOC_CAP_8BIT
  };
  auto alloc_framebuffer = [&](void) -> uint16_t* {
    for(uint32_t cap : FB_CAPS_FAST) {
      uint16_t *buf = (uint16_t *)heap_caps_malloc(fb_bytes, cap);
      if(buf != nullptr) {
        return buf;
      }
    }
    for(uint32_t cap : FB_CAPS_FALLBACK) {
      uint16_t *buf = (uint16_t *)heap_caps_malloc(fb_bytes, cap);
      if(buf != nullptr) {
        return buf;
      }
    }
    return nullptr;
  };
  for(size_t i = 0; i < 2; ++i) {
    uint16_t *buf = alloc_framebuffer();
    priv.framebuffers[i] = buf;
    if(buf == nullptr) {
      if(i == 0) {
        debugPrint("FB alloc fail");
        while(true) {
          delay(1000);
        }
      } else {
        priv.single_buffer_mode = true;
        Serial.println("Warning: single-buffer mode enabled (low memory)");
        break;
      }
    } else {
      memset(buf, 0, fb_bytes);
    }
  }
  if(priv.single_buffer_mode && priv.framebuffers[1] != nullptr) {
    heap_caps_free(priv.framebuffers[1]);
    priv.framebuffers[1] = nullptr;
    memset(priv.framebuffer_row_hash[1], 0, sizeof(priv.framebuffer_row_hash[1]));
  }
#endif

#if ENABLE_LCD
  if(g_psram_available) {
    if(!swap_fb_enabled) {
      const size_t swap_bytes = DEST_H * LCD_WIDTH * sizeof(uint16_t);
      static constexpr uint32_t swap_caps_psram[] = {
        MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
      };

      swap_fb_psram_backed = false;

      auto try_allocate_swap = [&](void) -> bool {
        for(uint32_t cap : swap_caps_psram) {
          swap_fb = (uint16_t *)heap_caps_malloc(swap_bytes, cap);
          if(swap_fb != nullptr) {
            swap_fb_enabled = true;
            swap_fb_dma_capable = (cap & MALLOC_CAP_DMA) != 0;
            swap_fb_psram_backed = true;
            memset(swap_fb, 0xFF, swap_bytes);
            display_cache_valid = false;
            memset(swap_row_hash, 0, sizeof(swap_row_hash));
            Serial.printf("Display cache allocated (%u bytes, caps=0x%X) (PSRAM)\n",
                          (unsigned)swap_bytes,
                          (unsigned)cap);
            return true;
          }
        }
        return false;
      };

      if(!try_allocate_swap()) {
        bool trimmed = false;
        while(!swap_fb_enabled) {
          size_t current_banks = priv.rom_cache.bank_count;
          if(current_banks <= 1) {
            break;
          }
          size_t target_banks = current_banks / 2;
          if(target_banks < 1) {
            target_banks = 1;
          }
          Serial.printf("Display cache allocation failed; trimming ROM cache banks %u -> %u to free PSRAM\n",
                        (unsigned)current_banks,
                        (unsigned)target_banks);
          if(rom_cache_trim_banks(&priv.rom_cache, target_banks)) {
            trimmed = true;
            g_settings.rom_cache_banks = static_cast<uint8_t>(target_banks);
            g_settings_dirty = true;
          }
          if(!try_allocate_swap()) {
            if(target_banks <= 1) {
              break;
            }
            continue;
          }
        }

        if(!swap_fb_enabled) {
          if(trimmed) {
            Serial.println("Display cache still unavailable after trimming ROM cache banks");
          } else {
            Serial.println("Display cache disabled (PSRAM allocation failed)");
          }
          swap_fb_dma_capable = false;
          swap_fb_psram_backed = false;
        } else {
          Serial.printf("ROM cache reduced to %u banks to accommodate display cache\n",
                        (unsigned)priv.rom_cache.bank_count);
        }
      }
    }
  } else {
    if(swap_fb_enabled && swap_fb != nullptr) {
      Serial.println("Releasing display cache (PSRAM unavailable)");
      heap_caps_free(swap_fb);
      swap_fb = nullptr;
    }
    swap_fb_enabled = false;
    swap_fb_dma_capable = false;
    swap_fb_psram_backed = false;
    display_cache_valid = false;
    memset(swap_row_hash, 0, sizeof(swap_row_hash));
    Serial.println("Display cache disabled (PSRAM not present)");
  }
#endif

  const bool uses_mbc7_eeprom = (ENABLE_MBC7 != 0) && (gb.mbc == 7);
  const size_t requested_cart_ram = gb_get_save_size(&gb);
  bool cart_ram_required = (requested_cart_ram > 0);

  priv.cart_ram_size = 0;
  priv.cart_ram_dirty = false;
  priv.cart_ram_loaded = false;
  priv.cart_ram_last_flush_ms = millis();
  priv.cart_save_write_failed = false;
  priv.mbc7_eeprom_dirty = false;
  priv.mbc7_eeprom_loaded = false;
  priv.mbc7_last_flush_ms = millis();
  priv.mbc7_save_write_failed = false;
  derive_save_paths(&priv, uses_mbc7_eeprom);

  priv.cart_ram = nullptr;

  if(uses_mbc7_eeprom) {
    Serial.println("MBC7 cartridge: using internal EEPROM, skipping external cart RAM buffer");
    cart_ram_required = false;
  } else if(!cart_ram_required) {
    Serial.println("Cartridge reports no external save RAM");
  }

  if(cart_ram_required) {
    priv.cart_ram = (uint8_t *)heap_caps_malloc(requested_cart_ram, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(priv.cart_ram == nullptr) {
      priv.cart_ram = (uint8_t *)heap_caps_malloc(requested_cart_ram, MALLOC_CAP_8BIT);
    }
    if(priv.cart_ram == nullptr && !priv.single_buffer_mode) {
      Serial.println("Freeing secondary framebuffer to retry cartridge RAM allocation");
      if(priv.framebuffers[1] != nullptr) {
        heap_caps_free(priv.framebuffers[1]);
        priv.framebuffers[1] = nullptr;
        memset(priv.framebuffer_row_hash[1], 0, sizeof(priv.framebuffer_row_hash[1]));
      }
      priv.single_buffer_mode = true;
      priv.write_fb_index = 0;
      priv.cart_ram = (uint8_t *)heap_caps_malloc(requested_cart_ram, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if(priv.cart_ram == nullptr) {
        priv.cart_ram = (uint8_t *)heap_caps_malloc(requested_cart_ram, MALLOC_CAP_8BIT);
      }
    }
    if(priv.cart_ram == nullptr) {
      Serial.println("Failed to allocate cartridge RAM");
      debugPrint("Cart RAM alloc fail");
    } else {
      memset(priv.cart_ram, 0xFF, requested_cart_ram);
      priv.cart_ram_size = requested_cart_ram;
      priv.cart_ram_last_flush_ms = millis();
      load_cart_ram_from_sd(&priv);
    }
  }
  if(!cart_ram_required) {
    priv.cart_ram_size = 0;
  }

#if ENABLE_MBC7
  if(uses_mbc7_eeprom) {
    priv.mbc7_last_flush_ms = millis();
    load_mbc7_eeprom_from_sd(&priv, &gb);
  }
#endif

#if ENABLE_LCD
  if(priv.single_buffer_mode) {
    priv.frame_queue = nullptr;
    if(priv.framebuffers[1] != nullptr) {
      heap_caps_free(priv.framebuffers[1]);
      priv.framebuffers[1] = nullptr;
      memset(priv.framebuffer_row_hash[1], 0, sizeof(priv.framebuffer_row_hash[1]));
    }
  } else {
    if(priv.frame_queue == nullptr) {
      priv.frame_queue = xQueueCreate(2, sizeof(uint8_t));
    }
    if(priv.frame_buffer_free[0] == nullptr) {
      priv.frame_buffer_free[0] = xSemaphoreCreateBinary();
    }
    if(priv.frame_buffer_free[1] == nullptr) {
      priv.frame_buffer_free[1] = xSemaphoreCreateBinary();
    }
    if(priv.frame_queue == nullptr || priv.frame_buffer_free[0] == nullptr || priv.frame_buffer_free[1] == nullptr) {
      Serial.println("Falling back to single-buffer mode (queue/semaphore alloc failed)");
      if(priv.frame_queue != nullptr) {
        vQueueDelete(priv.frame_queue);
      }
      if(priv.frame_buffer_free[0] != nullptr) {
        vSemaphoreDelete(priv.frame_buffer_free[0]);
      }
      if(priv.frame_buffer_free[1] != nullptr) {
        vSemaphoreDelete(priv.frame_buffer_free[1]);
      }
      priv.frame_queue = nullptr;
      priv.frame_buffer_free[0] = nullptr;
      priv.frame_buffer_free[1] = nullptr;
      if(priv.framebuffers[1] != nullptr) {
        heap_caps_free(priv.framebuffers[1]);
        priv.framebuffers[1] = nullptr;
        memset(priv.framebuffer_row_hash[1], 0, sizeof(priv.framebuffer_row_hash[1]));
      }
      priv.single_buffer_mode = true;
      Serial.println("Single-buffer mode engaged");
    } else {
      xSemaphoreGive(priv.frame_buffer_free[0]);
      xSemaphoreGive(priv.frame_buffer_free[1]);
      if(priv.frame_buffer_free[priv.write_fb_index] != nullptr) {
        xSemaphoreTake(priv.frame_buffer_free[priv.write_fb_index], portMAX_DELAY);
      }
      if(render_task_handle == nullptr) {
        BaseType_t render_created = xTaskCreatePinnedToCore(renderTask,
                                                            "RenderTask",
                                                            RENDER_TASK_STACK_SIZE,
                                                            nullptr,
                                                            tskIDLE_PRIORITY + 3,
                                                            &render_task_handle,
                                                            0);
        if(render_created != pdPASS) {
          debugPrint("Render task fail");
          while(true) {
            delay(1000);
          }
        }
      }
    }
  }
#endif

#if ENABLE_SOUND
  if(g_settings.audio_enabled) {
    audioSetup();
    if(audio_initialised) {
      BaseType_t audio_created = xTaskCreatePinnedToCore(audioTask,
                                                         "AudioTask",
                                                         AUDIO_TASK_STACK_SIZE,
                                                         nullptr,
                                                         tskIDLE_PRIORITY + 5,
                                                         &audio_task_handle,
                                                         0);
      if(audio_created != pdPASS) {
        Serial.println("Audio task creation failed; disabling audio");
        audio_task_handle = nullptr;
        audioTeardown();
      }
    } else {
      Serial.println("Audio disabled due to speaker init failure");
    }
  } else {
    Serial.println("Audio disabled by user");
  }
#endif

  debugPrint("Before loop");


  M5Cardputer.Display.clearDisplay();
  if(swap_fb_enabled && swap_fb != nullptr) {
    memset(swap_fb, 0xFF, DEST_H * LCD_WIDTH * sizeof(uint16_t));
    display_cache_valid = false;
    memset(swap_row_hash, 0, sizeof(swap_row_hash));
  }


  const double target_speed_us = 1000000.0 / VERTICAL_SYNC;

  static uint32_t frame_counter = 0;
  static bool logged_first_frame = false;
  static uint32_t watchdog_strikes = 0;
  while(1) {
    const uint64_t frame_start = micros64();

    poll_keyboard();
    const uint64_t after_poll = micros64();

    uint32_t cpu_steps = 0;
    const bool frame_completed = gb_run_frame_watchdog(&gb, GB_FRAME_STEP_BUDGET, &cpu_steps);
    const uint64_t after_emu = micros64();

  const uint8_t ime = gb.gb_ime ? 1 : 0;
  const uint8_t halt_flag = gb.gb_halt ? 1 : 0;
  const uint8_t lcdc = gb.hram_io[IO_LCDC];
  const uint8_t key1 = gb.hram_io[IO_KEY1];
  const uint8_t speed_double = gb.cgb.speed_double ? 1 : 0;

    if(frame_completed) {
      frame_counter++;
      watchdog_strikes = 0;

      if(frame_counter <= 10) {
        if(!logged_first_frame) {
          logged_first_frame = true;
          Serial.printf(
              "First frame complete: PC=%04X LY=%02X STAT=0x%02X IF=0x%02X IE=0x%02X IME=%u HALT=%u LCDC=0x%02X KEY1=0x%02X DS=%u steps=%u\n",
              (unsigned)gb.cpu_reg.pc.reg,
              (unsigned)gb.hram_io[IO_LY],
              (unsigned)gb.hram_io[IO_STAT],
              (unsigned)gb.hram_io[IO_IF],
              (unsigned)gb.hram_io[IO_IE],
              (unsigned)ime,
              (unsigned)halt_flag,
              (unsigned)lcdc,
              (unsigned)key1,
              (unsigned)speed_double,
              (unsigned)cpu_steps);
        } else {
          Serial.printf(
              "Frame %lu: PC=%04X LY=%02X STAT=0x%02X IF=0x%02X IE=0x%02X IME=%u HALT=%u LCDC=0x%02X KEY1=0x%02X DS=%u steps=%u\n",
              (unsigned long)frame_counter,
              (unsigned)gb.cpu_reg.pc.reg,
              (unsigned)gb.hram_io[IO_LY],
              (unsigned)gb.hram_io[IO_STAT],
              (unsigned)gb.hram_io[IO_IF],
              (unsigned)gb.hram_io[IO_IE],
              (unsigned)ime,
              (unsigned)halt_flag,
              (unsigned)lcdc,
              (unsigned)key1,
              (unsigned)speed_double,
              (unsigned)cpu_steps);
        }
      } else if(frame_counter <= 300 && (frame_counter % 60) == 0) {
        Serial.printf(
            "Frame %lu: PC=%04X LY=%02X STAT=0x%02X IF=0x%02X IE=0x%02X IME=%u HALT=%u LCDC=0x%02X KEY1=0x%02X DS=%u steps=%u\n",
            (unsigned long)frame_counter,
            (unsigned)gb.cpu_reg.pc.reg,
            (unsigned)gb.hram_io[IO_LY],
            (unsigned)gb.hram_io[IO_STAT],
            (unsigned)gb.hram_io[IO_IF],
            (unsigned)gb.hram_io[IO_IE],
            (unsigned)ime,
            (unsigned)halt_flag,
            (unsigned)lcdc,
            (unsigned)key1,
            (unsigned)speed_double,
            (unsigned)cpu_steps);
      }
    } else {
      watchdog_strikes++;
      if(watchdog_strikes == 1 || (watchdog_strikes % 60) == 0) {
        Serial.printf(
            "Frame watchdog tripped (count=%lu): steps=%u PC=%04X LY=%02X STAT=0x%02X IF=0x%02X IE=0x%02X IME=%u HALT=%u LCDC=0x%02X KEY1=0x%02X DS=%u\n",
            (unsigned long)watchdog_strikes,
            (unsigned)cpu_steps,
            (unsigned)gb.cpu_reg.pc.reg,
            (unsigned)gb.hram_io[IO_LY],
            (unsigned)gb.hram_io[IO_STAT],
            (unsigned)gb.hram_io[IO_IF],
            (unsigned)gb.hram_io[IO_IE],
            (unsigned)ime,
            (unsigned)halt_flag,
            (unsigned)lcdc,
            (unsigned)key1,
            (unsigned)speed_double);

        const uint8_t trace_count = gb.trace.count;
        if(trace_count > 0) {
          const uint8_t to_dump = trace_count < 12 ? trace_count : 12;
          Serial.println("Last instructions:");
          for(uint8_t i = 0; i < to_dump; ++i) {
            const uint8_t offset = (gb.trace.head + GB_DEBUG_TRACE_DEPTH - to_dump + i) % GB_DEBUG_TRACE_DEPTH;
            const gb_trace_entry &entry = gb.trace.entries[offset];
            const uint16_t bc = (static_cast<uint16_t>(entry.b) << 8) | entry.c;
            const uint16_t de = (static_cast<uint16_t>(entry.d) << 8) | entry.e;
            const uint16_t hl = (static_cast<uint16_t>(entry.h) << 8) | entry.l;
            Serial.printf(
                "  [%02u] PC=%04X OP=%02X %02X %02X A=%02X F=%02X BC=%04X DE=%04X HL=%04X SP=%04X IME=%u IF=%02X IE=%02X STAT=%02X LCDC=%02X LY=%02X\n",
                (unsigned)i,
                (unsigned)entry.pc,
                (unsigned)entry.opcode,
                (unsigned)entry.operand1,
                (unsigned)entry.operand2,
                (unsigned)entry.a,
                (unsigned)entry.f,
                (unsigned)bc,
                (unsigned)de,
                (unsigned)hl,
                (unsigned)entry.sp,
                (unsigned)entry.ime,
                (unsigned)entry.if_reg,
                (unsigned)entry.ie,
                (unsigned)entry.stat,
                (unsigned)entry.lcdc,
                (unsigned)entry.ly);
          }
        }
      }
    }

#if ENABLE_LCD
    if(frame_completed) {
      const bool frame_visible = (!gb.direct.frame_skip) || (gb.display.frame_skip_count != 0);
      if(frame_visible) {
        if(priv.single_buffer_mode || priv.frame_queue == nullptr) {
          fit_frame(priv.framebuffers[priv.write_fb_index],
                    priv.framebuffer_row_hash[priv.write_fb_index],
                    priv.framebuffer_row_dirty[priv.write_fb_index]);
        } else {
          const uint8_t render_index = priv.write_fb_index;
          if(priv.frame_queue != nullptr) {
            xQueueSend(priv.frame_queue, &render_index, portMAX_DELAY);
          }
          priv.write_fb_index ^= 1;
          if(priv.frame_buffer_free[priv.write_fb_index] != nullptr) {
            xSemaphoreTake(priv.frame_buffer_free[priv.write_fb_index], portMAX_DELAY);
          }
        }
      }
    }
#endif

    const uint64_t after_dispatch = micros64();
    const uint64_t active_duration = after_dispatch - frame_start;

    int64_t delay_us = static_cast<int64_t>(target_speed_us - static_cast<double>(active_duration));
    if(!frame_completed) {
      delay_us = 0;
    }
    bool over_budget = !frame_completed;
    if(delay_us <= 0) {
      over_budget = true;
    }
#if ENABLE_PROFILING
    uint64_t requested_delay_us = 0;
#endif
    if(delay_us > 0) {
#if ENABLE_PROFILING
      requested_delay_us = static_cast<uint64_t>(delay_us);
#endif
    } else {
      delay_us = 0;
    }

    uint64_t idle_us = 0;
    uint64_t frame_end = after_dispatch;
    if(delay_us > 0) {
      int64_t remaining_delay = delay_us;
      const uint64_t idle_start = micros64();
      if(remaining_delay >= 1000) {
        const uint32_t delay_ms = static_cast<uint32_t>(remaining_delay / 1000);
        const TickType_t delay_ticks = pdMS_TO_TICKS(delay_ms);
        if(delay_ticks > 0) {
          vTaskDelay(delay_ticks);
          remaining_delay -= static_cast<int64_t>(delay_ticks) * portTICK_PERIOD_MS * 1000;
          if(remaining_delay < 0) {
            remaining_delay = 0;
          }
        }
      }
      if(remaining_delay > 0) {
        esp_rom_delay_us(static_cast<uint32_t>(remaining_delay));
      }
      frame_end = micros64();
      idle_us = frame_end - idle_start;
    } else {
      frame_end = micros64();
    }

    const uint64_t frame_us = frame_end - frame_start;

    const uint32_t now_ms = millis();

    if(priv.cart_save_path_valid && priv.cart_ram_dirty && priv.cart_ram != nullptr &&
       priv.cart_ram_size > 0 && g_sd_mounted) {
      const uint32_t last = priv.cart_ram_last_flush_ms;
      const uint32_t elapsed = now_ms - last;
      if(elapsed >= SAVE_AUTO_FLUSH_INTERVAL_MS) {
        if(save_cart_ram_to_sd(&priv)) {
          priv.cart_ram_dirty = false;
          priv.cart_ram_last_flush_ms = now_ms;
          priv.cart_save_write_failed = false;
        } else {
          if(SAVE_AUTO_FLUSH_INTERVAL_MS > SAVE_FLUSH_RETRY_DELAY_MS) {
            priv.cart_ram_last_flush_ms = now_ms - (SAVE_AUTO_FLUSH_INTERVAL_MS - SAVE_FLUSH_RETRY_DELAY_MS);
          } else {
            priv.cart_ram_last_flush_ms = now_ms;
          }
          if(!priv.cart_save_write_failed) {
            Serial.printf("Cart RAM save flush failed; will retry (%s)\n",
                          priv.cart_save_path_valid ? priv.cart_save_path : "<invalid>");
          }
          priv.cart_save_write_failed = true;
        }
      }
    }

#if ENABLE_MBC7
    if(uses_mbc7_eeprom && priv.mbc7_eeprom_dirty && priv.mbc7_save_path_valid && g_sd_mounted) {
      const uint32_t last = priv.mbc7_last_flush_ms;
      const uint32_t elapsed = now_ms - last;
      if(elapsed >= SAVE_AUTO_FLUSH_INTERVAL_MS) {
        if(save_mbc7_eeprom_to_sd(&priv, &gb)) {
          priv.mbc7_eeprom_dirty = false;
          priv.mbc7_last_flush_ms = now_ms;
          priv.mbc7_save_write_failed = false;
        } else {
          if(SAVE_AUTO_FLUSH_INTERVAL_MS > SAVE_FLUSH_RETRY_DELAY_MS) {
            priv.mbc7_last_flush_ms = now_ms - (SAVE_AUTO_FLUSH_INTERVAL_MS - SAVE_FLUSH_RETRY_DELAY_MS);
          } else {
            priv.mbc7_last_flush_ms = now_ms;
          }
          if(!priv.mbc7_save_write_failed) {
            Serial.printf("MBC7 EEPROM flush failed; will retry (%s)\n",
                          priv.mbc7_save_path_valid ? priv.mbc7_save_path : "<invalid>");
          }
          priv.mbc7_save_write_failed = true;
        }
      }
    }
#endif

  apply_frame_skip_policy(&gb, over_budget);

#if ENABLE_PROFILING
    profiler_record_frame(frame_us,
                          after_poll - frame_start,
                          after_emu - after_poll,
                          after_dispatch - after_emu,
                          idle_us,
                          requested_delay_us,
                          over_budget,
                          frame_end);
#endif
  }
}






void loop() {

}