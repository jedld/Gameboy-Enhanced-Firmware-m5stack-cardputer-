/**
 * MBC7 Accelerometer Emulation for M5Stack Cardputer
 * 
 * The Cardputer ships with a BMI270 6-axis IMU. We wire the real
 * accelerometer straight into the Peanut-GB MBC7 hook so motion-enabled
 * carts (Kirby Tilt 'n' Tumble) work out of the box. A keyboard based
 * fallback remains available for environments without IMU access.
 */

#ifndef MBC7_CARDPUTER_H
#define MBC7_CARDPUTER_H

#ifndef ENABLE_MBC7
#define ENABLE_MBC7 1
#endif

#include <stdint.h>

// Include peanut_gb.h only for the struct definition we need
// Don't include the full implementation to avoid dependency issues
struct gb_s;

#if ENABLE_MBC7
#include "M5Cardputer.h"
#endif

/**
 * MBC7 Virtual Accelerometer State
 */
struct mbc7_cardputer_s {
    float tilt_x;           // Current X tilt (-1.0 to 1.0)
    float tilt_y;           // Current Y tilt (-1.0 to 1.0)
    bool enabled;           // Whether MBC7 emulation is active
    bool use_hardware_imu;  // Whether to use BMI270 hardware IMU or keyboard simulation
    float imu_offset_x;     // IMU zero reference for X axis (m/s^2)
    float imu_offset_y;     // IMU zero reference for Y axis (m/s^2)
    float imu_offset_z;     // IMU zero reference for Z axis (m/s^2)
    float imu_filtered_x;   // Low-pass filtered raw accelerometer X
    float imu_filtered_y;   // Low-pass filtered raw accelerometer Y
    float imu_filtered_z;   // Low-pass filtered raw accelerometer Z
    bool imu_filter_initialised; // Whether filter has been primed with a sample
    bool imu_offset_valid;  // True once we captured a reference posture
    bool calibration_key_down; // Tracks calibration hotkey edge detection
    uint32_t sample_interval_us; // Minimum interval between IMU samples
    uint64_t last_sample_us; // Timestamp of last update
};

// Tilt sensitivity (how much tilt changes per update)
#define MBC7_TILT_SPEED 0.05f
#define MBC7_TILT_MIN -1.0f
#define MBC7_TILT_MAX 1.0f

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize MBC7 cardputer support
 */
void mbc7_cardputer_init(struct mbc7_cardputer_s *state);

/**
 * Update virtual accelerometer based on keyboard input
 */
void mbc7_cardputer_update(struct mbc7_cardputer_s *state);

/**
 * Callback function for peanut_gb to read accelerometer data
 */
int mbc7_cardputer_accel_read(struct gb_s *gb, float *x_out, float *y_out);

/**
 * Draw tilt indicator on screen (for debugging/visualization)
 */
void mbc7_cardputer_draw_indicator(struct mbc7_cardputer_s *state, int x, int y);

/**
 * Get global MBC7 state (for initialization and access)
 */
struct mbc7_cardputer_s* get_mbc7_state();

#ifdef __cplusplus
}
#endif

#endif // MBC7_CARDPUTER_H
