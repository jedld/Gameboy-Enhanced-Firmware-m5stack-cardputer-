/**
 * minigb_apu is released under the terms listed within the LICENSE file.
 *
 * minigb_apu emulates the audio processing unit (APU) of the Game Boy. This
 * project is based on MiniGBS by Alex Baines: https://github.com/baines/MiniGBS
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default sample rate requested from the speaker driver. */
#define AUDIO_DEFAULT_SAMPLE_RATE  44100U

#define DMG_CLOCK_FREQ		4194304.0
#define SCREEN_REFRESH_CYCLES	70224.0
#define VERTICAL_SYNC		(DMG_CLOCK_FREQ/SCREEN_REFRESH_CYCLES)

/** Query the currently configured sample rate (Hz). */
uint32_t audio_get_sample_rate(void);

/** Override the active sample rate (Hz) at runtime. */
void audio_set_sample_rate(uint32_t sample_rate);

/** Number of stereo frames generated for each video frame. */
uint32_t audio_samples_per_frame(void);

/** Convenience helper returning the interleaved sample count (L+R). */
uint32_t audio_samples_per_buffer(void);

/**
 * Fill allocated buffer "data" with "len" number of 32-bit floating point
 * samples (native endian order) in stereo interleaved format.
 */
void audio_callback(void *ptr, uint8_t *data, int len);

/**
 * Read audio register at given address "addr".
 */
uint8_t audio_read(const uint16_t addr);

/**
 * Write "val" to audio register at given address "addr".
 */
void audio_write(const uint16_t addr, const uint8_t val);

/**
 * Initialise audio driver.
 */
void audio_init(void);

#ifdef __cplusplus
}
#endif
