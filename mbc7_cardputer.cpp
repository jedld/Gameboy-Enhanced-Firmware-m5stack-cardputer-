/**
 * MBC7 Accelerometer Emulation Implementation for M5Stack Cardputer
 */

#include "mbc7_cardputer.h"
#include <algorithm>
#include <cmath>
#include <esp_timer.h>

namespace {
constexpr float kGravity = 9.80665f;          // Standard gravity in m/s^2
constexpr float kFilterAlpha = 0.18f;         // Low-pass filter coefficient
constexpr float kDeadzone = 0.05f;            // Deadzone in normalised tilt units
constexpr float kMaxTiltG = 1.20f;            // Clamp raw g-value before normalisation

static float apply_deadzone(float value) {
    const float abs_val = std::fabs(value);
    if(abs_val < kDeadzone) {
        return 0.0f;
    }

    const float sign = (value < 0.0f) ? -1.0f : 1.0f;
    const float scaled = std::max(0.0f, std::min(1.0f, (abs_val - kDeadzone) / (kMaxTiltG - kDeadzone)));
    return sign * scaled;
}

static void calibrate_offsets(struct mbc7_cardputer_s *state,
                              float fx,
                              float fy,
                              float fz,
                              bool verbose) {
    state->imu_offset_x = fx;
    state->imu_offset_y = fy;
    state->imu_offset_z = fz;
    state->imu_offset_valid = true;
    if(verbose) {
        Serial.println("MBC7: IMU zero point captured");
    }
}
} // namespace

// We only need the struct declaration from the header, not the full implementation
// The gb_s struct is forward declared in mbc7_cardputer.h

// Global state (can be stored in priv field of gb_s if preferred)
static struct mbc7_cardputer_s g_mbc7_state = {};

void mbc7_cardputer_init(struct mbc7_cardputer_s *state) {
    state->tilt_x = 0.0f;
    state->tilt_y = 0.0f;
    state->enabled = true;
    state->use_hardware_imu = false;
    state->imu_offset_x = 0.0f;
    state->imu_offset_y = 0.0f;
    state->imu_offset_z = 0.0f;
    state->imu_filtered_x = 0.0f;
    state->imu_filtered_y = 0.0f;
    state->imu_filtered_z = 0.0f;
    state->imu_filter_initialised = false;
    state->imu_offset_valid = false;
    state->calibration_key_down = false;
    state->sample_interval_us = 5000; // ~200 Hz sampling
    state->last_sample_us = 0;

    if(!M5.Imu.isEnabled()) {
        bool imu_ready = false;
        if(M5.In_I2C.isEnabled()) {
            imu_ready = M5.Imu.begin(&M5.In_I2C, M5.getBoard());
        }
        if(!imu_ready && M5.Ex_I2C.isEnabled()) {
            imu_ready = M5.Imu.begin(&M5.Ex_I2C);
        }
        if(!imu_ready) {
            imu_ready = M5.Imu.begin();
        }
        state->use_hardware_imu = imu_ready && M5.Imu.isEnabled();
    } else {
        state->use_hardware_imu = true;
    }

    if(state->use_hardware_imu) {
        Serial.println("MBC7: Hardware IMU (BMI270) detected and enabled");
        m5::IMU_Class::imu_data_t data;
        M5.Imu.update();
        M5.Imu.getImuData(&data);
        state->imu_filtered_x = data.accel.x;
        state->imu_filtered_y = data.accel.y;
        state->imu_filtered_z = data.accel.z;
        state->imu_filter_initialised = true;
        calibrate_offsets(state, state->imu_filtered_x, state->imu_filtered_y, state->imu_filtered_z, false);
    } else {
        Serial.println("MBC7: Using keyboard simulation (IMU not available)");
    }
}

void mbc7_cardputer_update(struct mbc7_cardputer_s *state) {
    if (!state->enabled) return;

    const uint64_t now_us = esp_timer_get_time();
    const uint64_t last_us = state->last_sample_us;
    if(last_us != 0 && (now_us - last_us) < state->sample_interval_us) {
        return;
    }
    const float dt = (last_us == 0) ? (state->sample_interval_us / 1000000.0f)
                                    : ((now_us - last_us) / 1000000.0f);
    state->last_sample_us = now_us;

    M5Cardputer.update();

    const bool calibration_pressed = M5Cardputer.Keyboard.isKeyPressed(' ') ||
                                     M5Cardputer.Keyboard.isKeyPressed('0');

    if (state->use_hardware_imu && M5.Imu.isEnabled()) {
        m5::IMU_Class::imu_data_t data;
        M5.Imu.update();
        M5.Imu.getImuData(&data);

        const float ax = data.accel.x;
        const float ay = data.accel.y;
        const float az = data.accel.z;

        if (!state->imu_filter_initialised) {
            state->imu_filtered_x = ax;
            state->imu_filtered_y = ay;
            state->imu_filtered_z = az;
            state->imu_filter_initialised = true;
        } else {
            state->imu_filtered_x += (ax - state->imu_filtered_x) * kFilterAlpha;
            state->imu_filtered_y += (ay - state->imu_filtered_y) * kFilterAlpha;
            state->imu_filtered_z += (az - state->imu_filtered_z) * kFilterAlpha;
        }

        if (!state->imu_offset_valid) {
            calibrate_offsets(state,
                              state->imu_filtered_x,
                              state->imu_filtered_y,
                              state->imu_filtered_z,
                              false);
        }

        if (calibration_pressed && !state->calibration_key_down) {
            calibrate_offsets(state,
                              state->imu_filtered_x,
                              state->imu_filtered_y,
                              state->imu_filtered_z,
                              true);
        }
        state->calibration_key_down = calibration_pressed;

        const float norm_x = std::max(-kMaxTiltG, std::min(kMaxTiltG,
                      (state->imu_filtered_y - state->imu_offset_y) / kGravity));
        const float norm_y = std::max(-kMaxTiltG, std::min(kMaxTiltG,
                      -(state->imu_filtered_x - state->imu_offset_x) / kGravity));

        state->tilt_x = std::max(-1.0f, std::min(1.0f, apply_deadzone(norm_x)));
        state->tilt_y = std::max(-1.0f, std::min(1.0f, apply_deadzone(norm_y)));

    } else {
        state->use_hardware_imu = false;
        state->calibration_key_down = calibration_pressed;

        // Keyboard-based tilt simulation (fallback)
        const float scale = dt * 60.0f;
        const float decay = MBC7_TILT_SPEED * 0.3f * scale;
        const float step = MBC7_TILT_SPEED * scale;

        if (state->tilt_x > decay) {
            state->tilt_x -= decay;
        } else if (state->tilt_x < -decay) {
            state->tilt_x += decay;
        } else {
            state->tilt_x = 0.0f;
        }

        if (state->tilt_y > decay) {
            state->tilt_y -= decay;
        } else if (state->tilt_y < -decay) {
            state->tilt_y += decay;
        } else {
            state->tilt_y = 0.0f;
        }

        if (M5Cardputer.Keyboard.isKeyPressed('w') || M5Cardputer.Keyboard.isKeyPressed(';')) {
            state->tilt_y = std::max(state->tilt_y - step, MBC7_TILT_MIN);
        }

        if (M5Cardputer.Keyboard.isKeyPressed('s') || M5Cardputer.Keyboard.isKeyPressed('.')) {
            state->tilt_y = std::min(state->tilt_y + step, MBC7_TILT_MAX);
        }

        if (M5Cardputer.Keyboard.isKeyPressed('a') || M5Cardputer.Keyboard.isKeyPressed(',')) {
            state->tilt_x = std::max(state->tilt_x - step, MBC7_TILT_MIN);
        }

        if (M5Cardputer.Keyboard.isKeyPressed('d') || M5Cardputer.Keyboard.isKeyPressed('/')) {
            state->tilt_x = std::min(state->tilt_x + step, MBC7_TILT_MAX);
        }

        if (calibration_pressed) {
            state->tilt_x = 0.0f;
            state->tilt_y = 0.0f;
        }
    }
}

int mbc7_cardputer_accel_read(struct gb_s *gb, float *x_out, float *y_out) {
    // Update tilt state
    mbc7_cardputer_update(&g_mbc7_state);
    
    // Return current tilt values
    *x_out = g_mbc7_state.tilt_x;
    *y_out = g_mbc7_state.tilt_y;
    
    return 1;  // Data available
}

void mbc7_cardputer_draw_indicator(struct mbc7_cardputer_s *state, int center_x, int center_y) {
    if (!state->enabled) return;
    
    // Draw a simple crosshair indicator showing tilt
    int radius = 20;
    
    // Calculate indicator position based on tilt
    int ind_x = center_x + (int)(state->tilt_x * radius);
    int ind_y = center_y + (int)(state->tilt_y * radius);
    
    // Draw crosshair (requires M5.Display or equivalent)
    // Note: Adjust based on your display library
    M5Cardputer.Display.drawCircle(center_x, center_y, radius, TFT_DARKGREY);
    M5Cardputer.Display.drawLine(center_x - radius, center_y, center_x + radius, center_y, TFT_DARKGREY);
    M5Cardputer.Display.drawLine(center_x, center_y - radius, center_x, center_y + radius, TFT_DARKGREY);
    
    // Draw indicator dot
    M5Cardputer.Display.fillCircle(ind_x, ind_y, 3, TFT_RED);
    
    // Optional: Draw text showing values
    M5Cardputer.Display.setCursor(center_x - 40, center_y + radius + 5);
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setTextColor(TFT_WHITE);
    char buf[32];
    snprintf(buf, sizeof(buf), "X:%.2f Y:%.2f", state->tilt_x, state->tilt_y);
    M5Cardputer.Display.print(buf);
}

/**
 * External access to global state for visualization
 */
extern "C" struct mbc7_cardputer_s* get_mbc7_state() {
    return &g_mbc7_state;
}
