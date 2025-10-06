#include <cstddef>
#include <cstdint>
#include <cstring>

#include "embedded_rom_config.h"
#include "embedded_rom.h"

#if EMBEDDED_ROM_MULTI_SUPPORT == 0

extern const bool kEmbeddedRomPresent;
extern const bool kEmbeddedRomAutoboot;
extern const char kEmbeddedRomName[];
extern const uint8_t kEmbeddedRomData[];
extern const size_t kEmbeddedRomSize;

namespace {

static const EmbeddedRomEntry kLegacyEmbeddedRomEntry = {
    kEmbeddedRomName,
    kEmbeddedRomName,
    kEmbeddedRomData,
    kEmbeddedRomSize,
    kEmbeddedRomAutoboot
};

} // namespace

const size_t kEmbeddedRomCount = kEmbeddedRomPresent ? 1u : 0u;

const EmbeddedRomEntry *const kEmbeddedRoms[1] = {
    kEmbeddedRomPresent ? &kLegacyEmbeddedRomEntry : nullptr
};

const EmbeddedRomEntry *embedded_rom_get_autoboot() {
  if(!kEmbeddedRomPresent || !kEmbeddedRomAutoboot) {
    return nullptr;
  }
  return &kLegacyEmbeddedRomEntry;
}

const EmbeddedRomEntry *embedded_rom_get(size_t index) {
  if(!kEmbeddedRomPresent || index != 0) {
    return nullptr;
  }
  return &kLegacyEmbeddedRomEntry;
}

const EmbeddedRomEntry *embedded_rom_find(const char *id) {
  if(!kEmbeddedRomPresent || id == nullptr) {
    return nullptr;
  }
  if(kLegacyEmbeddedRomEntry.id == nullptr) {
    return nullptr;
  }
  return (strcmp(kLegacyEmbeddedRomEntry.id, id) == 0) ? &kLegacyEmbeddedRomEntry : nullptr;
}

#else

// Multi-ROM build activated; legacy shim not required.

#endif // EMBEDDED_ROM_MULTI_SUPPORT
