/**
 * MBC7 Implementation
 */

#include "mbc7.h"
#include <string.h>

void mbc7_init(struct mbc7_s *mbc7) {
    memset(mbc7, 0, sizeof(struct mbc7_s));
    
    // Initialize accelerometer to center position
    mbc7->accel.x = MBC7_ACCEL_CENTER;
    mbc7->accel.y = MBC7_ACCEL_CENTER;
    mbc7->accel.latched = 0;
    
    // Initialize EEPROM
    mbc7_eeprom_reset(&mbc7->eeprom);
    
    // RAM is disabled by default
    mbc7->ram_enable_1 = 0;
    mbc7->ram_enable_2 = 0;
}

void mbc7_eeprom_reset(struct mbc7_eeprom_s *eeprom) {
    memset(eeprom->data, 0xFF, sizeof(eeprom->data));
    
    eeprom->cs = 0;
    eeprom->clk = 0;
    eeprom->di = 0;
    /* EEPROM DO line idles high to signal ready state */
    eeprom->do_out = 1;
    
    eeprom->state = EEPROM_IDLE;
    eeprom->shift_reg = 0;
    eeprom->bit_count = 0;
    eeprom->address = 0;
    eeprom->write_enabled = 0;
    eeprom->busy = 0;
}

static void mbc7_eeprom_process_command(struct mbc7_eeprom_s *eeprom) {
    // Command format: start bit (1) + opcode (2 bits) + address/data (7 bits)
    uint16_t cmd = eeprom->shift_reg;
    uint8_t opcode = (cmd >> 7) & 0x3;
    uint8_t addr = cmd & 0x7F;
    
    switch (opcode) {
        case EEPROM_CMD_READ:  // 10
            if (addr < 128) {
                eeprom->address = addr;
                eeprom->shift_reg = eeprom->data[addr];
                eeprom->bit_count = 17; // include dummy 0 bit before data
                eeprom->state = EEPROM_READ;
                eeprom->do_out = 0;
            }
            break;
            
        case EEPROM_CMD_WRITE:  // 01
            if (eeprom->write_enabled && addr < 128) {
                eeprom->address = addr;
                eeprom->state = EEPROM_WRITE;
                eeprom->bit_count = 0;
            }
            break;
            
        case EEPROM_CMD_ERASE:  // 11
            if (eeprom->write_enabled && addr < 128) {
                eeprom->data[addr] = 0xFFFF;
                eeprom->busy = 1;
                eeprom->state = EEPROM_WAIT_READY;
            }
            break;
            
        case EEPROM_CMD_EWEN:  // 00
            // Check for specific commands
            if ((cmd & 0x1FF) == 0x0C0) {  // 0011xxxxxx - EWEN
                eeprom->write_enabled = 1;
            } else if ((cmd & 0x1FF) == 0x000) {  // 0000xxxxxx - EWDS
                eeprom->write_enabled = 0;
            } else if ((cmd & 0x1FF) == 0x080) {  // 0010xxxxxx - ERAL
                if (eeprom->write_enabled) {
                    memset(eeprom->data, 0xFF, sizeof(eeprom->data));
                    eeprom->busy = 1;
                    eeprom->state = EEPROM_WAIT_READY;
                }
            } else if ((cmd & 0x1FF) == 0x040) {  // 0001xxxxxx - WRAL
                if (eeprom->write_enabled) {
                    eeprom->state = EEPROM_WRITE;
                    eeprom->bit_count = 0;
                    eeprom->address = 0xFF;  // Special marker for WRAL
                }
            }
            break;
    }
}

void mbc7_eeprom_update(struct mbc7_eeprom_s *eeprom, uint8_t value) {
    uint8_t new_cs = (value >> 7) & 1;
    uint8_t new_clk = (value >> 6) & 1;
    uint8_t new_di = (value >> 1) & 1;
    
    // CS rising edge - start operation
    if (new_cs && !eeprom->cs) {
        eeprom->state = EEPROM_COMMAND;
        eeprom->shift_reg = 0;
        eeprom->bit_count = 0;
        eeprom->do_out = 0;
    }
    
    // CS falling edge - end operation
    if (!new_cs && eeprom->cs) {
        eeprom->state = EEPROM_IDLE;
        eeprom->bit_count = 0;
    }
    
    // Clock rising edge - shift data
    if (new_cs && new_clk && !eeprom->clk) {
        switch (eeprom->state) {
            case EEPROM_COMMAND:
                // Shift in command bits
                eeprom->shift_reg = (eeprom->shift_reg << 1) | new_di;
                eeprom->bit_count++;
                
                // After 10 bits, process command
                if (eeprom->bit_count >= 10) {
                    mbc7_eeprom_process_command(eeprom);
                }
                break;
                
            case EEPROM_READ:
                // Shift out data
                if (eeprom->bit_count == 17) {
                    // Dummy 0 bit before data per 93LC56 behaviour
                    eeprom->do_out = 0;
                    eeprom->bit_count--;
                } else if (eeprom->bit_count > 0) {
                    eeprom->do_out = (eeprom->shift_reg >> 15) & 1;
                    eeprom->shift_reg <<= 1;
                    eeprom->bit_count--;
                    if (eeprom->bit_count == 0 && eeprom->cs) {
                        // Continue sequential read when CS stays asserted
                        eeprom->address = (uint8_t)((eeprom->address + 1) & 0x7F);
                        eeprom->shift_reg = eeprom->data[eeprom->address];
                        eeprom->bit_count = 16;
                    }
                } else {
                    eeprom->state = EEPROM_IDLE;
                }
                break;
                
            case EEPROM_WRITE:
                // Shift in data
                eeprom->shift_reg = (eeprom->shift_reg << 1) | new_di;
                eeprom->bit_count++;
                
                if (eeprom->bit_count >= 16) {
                    // Write complete
                    if (eeprom->address == 0xFF) {
                        // WRAL - write to all addresses
                        for (int i = 0; i < 128; i++) {
                            eeprom->data[i] = eeprom->shift_reg;
                        }
                    } else if (eeprom->address < 128) {
                        eeprom->data[eeprom->address] = eeprom->shift_reg;
                    }
                    eeprom->busy = 1;
                    eeprom->state = EEPROM_WAIT_READY;
                }
                break;
                
            case EEPROM_WAIT_READY:
                // Simulate programming time (just set ready immediately)
                eeprom->busy = 0;
                eeprom->do_out = 1;  // RDY signal
                eeprom->state = EEPROM_IDLE;
                break;
                
            default:
                break;
        }
    }
    
    // Update pin states
    eeprom->cs = new_cs;
    eeprom->clk = new_clk;
    eeprom->di = new_di;

    /* When idle and not busy the DO line should remain high */
    if(eeprom->state == EEPROM_IDLE && !eeprom->busy) {
        eeprom->do_out = 1;
    }
}

void mbc7_accel_latch(struct mbc7_s *mbc7, mbc7_accel_read_t read_callback, struct gb_s *gb) {
    if (read_callback) {
        float x, y;
        if (read_callback(gb, &x, &y)) {
            mbc7->accel.x = mbc7_accel_float_to_value(x);
            mbc7->accel.y = mbc7_accel_float_to_value(y);
            mbc7->accel.latched = 1;
        }
    } else {
        // No callback - use default center values
        mbc7->accel.x = MBC7_ACCEL_CENTER;
        mbc7->accel.y = MBC7_ACCEL_CENTER;
        mbc7->accel.latched = 1;
    }
}

uint8_t mbc7_eeprom_poll_do(struct mbc7_eeprom_s *eeprom) {
    if(eeprom->busy) {
        /* Complete pending operation immediately for now */
        eeprom->busy = 0;
        eeprom->state = EEPROM_IDLE;
    }

    /* For now, always signal READY to satisfy polling loops. */
    eeprom->do_out = 1;
    return 1;
}
