#pragma once

#include <cstddef>
#include <cstdint>

struct EmbeddedRomEntry {
	const char *id;          // Stable identifier (slug) for tooling.
	const char *name;        // Friendly display name.
	const uint8_t *data;     // Pointer to ROM bytes (in PROGMEM/flash).
	size_t size;             // Unpadded ROM size in bytes.
	bool autoboot;           // True if firmware should try this ROM on boot.
};

// Returns the number of embedded ROM entries compiled into the firmware.
extern const size_t kEmbeddedRomCount;

// Array of pointers to each embedded ROM entry. The lifetime matches the
// firmware image, so the pointers remain valid for the duration of execution.
extern const EmbeddedRomEntry *const kEmbeddedRoms[];

// Returns the entry flagged for autoboot, or nullptr if none/all disabled.
const EmbeddedRomEntry *embedded_rom_get_autoboot();

// Bounds-checked pointer access into the embedded ROM list. Returns nullptr if
// index is out of range.
const EmbeddedRomEntry *embedded_rom_get(size_t index);

// Convenience helper to look up an entry by slug identifier. Returns nullptr
// if the identifier is not present.
const EmbeddedRomEntry *embedded_rom_find(const char *id);
