#include <cstddef>
#include <cstdint>
#include <cstring>

#include "embedded_rom.h"

// Default stub used when no embedded ROMs are bundled.
// This file is safely overwritten by scripts/embed_rom.py when entries exist.

namespace {
constexpr EmbeddedRomEntry kEmptyEntry = {
    nullptr, // id
    nullptr, // name
    nullptr, // data
    0u,      // size
    false    // autoboot
};
} // namespace

const size_t kEmbeddedRomCount = 0u;
const EmbeddedRomEntry *const kEmbeddedRoms[1] = { nullptr };

const EmbeddedRomEntry *embedded_rom_get_autoboot() {
  return nullptr;
}

const EmbeddedRomEntry *embedded_rom_get(size_t index) {
  (void)index;
  return nullptr;
}

const EmbeddedRomEntry *embedded_rom_find(const char *id) {
  (void)id;
  return nullptr;
}
