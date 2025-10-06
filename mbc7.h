/**
 * MBC7 (Memory Bank Controller 7) Support
 * 
 * MBC7 includes:
 * - 2-axis accelerometer (ADXL202E)
 * - 256 byte EEPROM (93LC56)
 * 
 * This implementation provides callback mechanism for accelerometer data
 * to be provided by the platform (e.g., M5Stack Cardputer gyro/accelerometer).
 */

#ifndef MBC7_H
#define MBC7_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
struct gb_s;

/**
 * MBC7 Accelerometer state
 */
struct mbc7_accel_s {
    uint16_t x;  // X-axis value (centered at 0x81D0)
    uint16_t y;  // Y-axis value (centered at 0x81D0)
    uint8_t latched;  // Whether values are latched
};

/**
 * MBC7 EEPROM state (93LC56 - 256 bytes, 128 x 16-bit words)
 */
struct mbc7_eeprom_s {
    uint16_t data[128];  // 128 words of 16-bit data
    
    // Pin states
    uint8_t cs;   // Chip Select
    uint8_t clk;  // Clock
    uint8_t di;   // Data In
    uint8_t do_out; // Data Out
    
    // Internal state
    uint8_t state;
    uint16_t shift_reg;
    uint8_t bit_count;
    uint8_t address;
    uint8_t write_enabled;
    uint8_t busy;
};

/**
 * Complete MBC7 state
 */
struct mbc7_s {
    struct mbc7_accel_s accel;
    struct mbc7_eeprom_s eeprom;
    uint8_t ram_enable_1;  // 0x0000-0x1FFF enable
    uint8_t ram_enable_2;  // 0x4000-0x5FFF enable (must be 0x40)
};

// EEPROM command states
enum mbc7_eeprom_state_e {
    EEPROM_IDLE = 0,
    EEPROM_COMMAND,
    EEPROM_READ,
    EEPROM_WRITE,
    EEPROM_WAIT_READY
};

// EEPROM commands (10-bit)
#define EEPROM_CMD_READ   0x2  // 10xAAAAAAA
#define EEPROM_CMD_EWEN   0x0  // 0011xxxxxx (Enable Write/Erase)
#define EEPROM_CMD_EWDS   0x0  // 0000xxxxxx (Disable Write/Erase)
#define EEPROM_CMD_WRITE  0x1  // 01xAAAAAAA
#define EEPROM_CMD_ERASE  0x3  // 11xAAAAAAA
#define EEPROM_CMD_ERAL   0x0  // 0010xxxxxx (Erase All)
#define EEPROM_CMD_WRAL   0x0  // 0001xxxxxx (Write All)

// Accelerometer center value
#define MBC7_ACCEL_CENTER 0x81D0
#define MBC7_ACCEL_GRAVITY_EFFECT 0x70

/**
 * Callback type for getting accelerometer data from platform
 * 
 * @param gb - Game Boy context
 * @param x_out - Pointer to store X-axis value (-1.0 to 1.0, 0 = center)
 * @param y_out - Pointer to store Y-axis value (-1.0 to 1.0, 0 = center)
 * @return 1 if accelerometer data is available, 0 otherwise
 */
typedef int (*mbc7_accel_read_t)(struct gb_s *gb, float *x_out, float *y_out);

/**
 * Initialize MBC7 state
 */
void mbc7_init(struct mbc7_s *mbc7);

/**
 * Reset EEPROM state
 */
void mbc7_eeprom_reset(struct mbc7_eeprom_s *eeprom);

/**
 * Update EEPROM pin state and process commands
 */
void mbc7_eeprom_update(struct mbc7_eeprom_s *eeprom, uint8_t value);

/**
 * Latch accelerometer data
 */
void mbc7_accel_latch(struct mbc7_s *mbc7, mbc7_accel_read_t read_callback, struct gb_s *gb);

/**
 * Convert float acceleration (-1.0 to 1.0) to MBC7 format
 */
static inline uint16_t mbc7_accel_float_to_value(float accel) {
    // Center value is 0x81D0
    // Each g of acceleration affects value by ~0x70
    int16_t offset = (int16_t)(accel * (float)MBC7_ACCEL_GRAVITY_EFFECT);
    return (uint16_t)((int16_t)MBC7_ACCEL_CENTER + offset);
}

/**
 * Poll EEPROM ready/DO state (auto-clears busy flag once observed)
 */
uint8_t mbc7_eeprom_poll_do(struct mbc7_eeprom_s *eeprom);

#ifdef __cplusplus
}
#endif

#endif // MBC7_H
