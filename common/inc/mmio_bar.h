/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#ifndef CARDINAL_MMIO_BAR_H
#define CARDINAL_MMIO_BAR_H

#include <stdint.h>
#include <types.h>

/**
 * DEFINE_BAR_ACCESSORS(prefix, state_type)
 *
 * Generates 8 MMIO BAR accessor functions for a device whose state struct is
 * `state_type` and whose BAR base pointer is the `bar` field of that struct.
 * The generated functions are:
 *
 *   uint8_t  prefix##_read8 (state_type *dev, int off)
 *   uint16_t prefix##_read16(state_type *dev, int off)
 *   uint32_t prefix##_read32(state_type *dev, int off)
 *   uint64_t prefix##_read64(state_type *dev, int off)
 *   void     prefix##_write8 (state_type *dev, int off, uint8_t  val)
 *   void     prefix##_write16(state_type *dev, int off, uint16_t val)
 *   void     prefix##_write32(state_type *dev, int off, uint32_t val)
 *   void     prefix##_write64(state_type *dev, int off, uint64_t val)
 *
 * Each accessor PANICs on a misaligned `off`.
 */
#define DEFINE_BAR_ACCESSORS(prefix, state_type)                                \
    uint64_t prefix##_read64(state_type *dev, int off) {                        \
        if (off % sizeof(uint64_t) != 0)                                        \
            PANIC("Invalid offset");                                             \
        return ((uint64_t *)dev->bar)[off / sizeof(uint64_t)];                  \
    }                                                                            \
    uint32_t prefix##_read32(state_type *dev, int off) {                        \
        if (off % sizeof(uint32_t) != 0)                                        \
            PANIC("Invalid offset");                                             \
        return ((uint32_t *)dev->bar)[off / sizeof(uint32_t)];                  \
    }                                                                            \
    uint16_t prefix##_read16(state_type *dev, int off) {                        \
        if (off % sizeof(uint16_t) != 0)                                        \
            PANIC("Invalid offset");                                             \
        return ((uint16_t *)dev->bar)[off / sizeof(uint16_t)];                  \
    }                                                                            \
    uint8_t prefix##_read8(state_type *dev, int off) {                          \
        if (off % sizeof(uint8_t) != 0)                                         \
            PANIC("Invalid offset");                                             \
        return ((uint8_t *)dev->bar)[off / sizeof(uint8_t)];                    \
    }                                                                            \
    void prefix##_write64(state_type *dev, int off, uint64_t val) {             \
        if (off % sizeof(uint64_t) != 0)                                        \
            PANIC("Invalid offset");                                             \
        ((uint64_t *)dev->bar)[off / sizeof(uint64_t)] = val;                   \
    }                                                                            \
    void prefix##_write32(state_type *dev, int off, uint32_t val) {             \
        if (off % sizeof(uint32_t) != 0)                                        \
            PANIC("Invalid offset");                                             \
        ((uint32_t *)dev->bar)[off / sizeof(uint32_t)] = val;                   \
    }                                                                            \
    void prefix##_write16(state_type *dev, int off, uint16_t val) {             \
        if (off % sizeof(uint16_t) != 0)                                        \
            PANIC("Invalid offset");                                             \
        ((uint16_t *)dev->bar)[off / sizeof(uint16_t)] = val;                   \
    }                                                                            \
    void prefix##_write8(state_type *dev, int off, uint8_t val) {               \
        if (off % sizeof(uint8_t) != 0)                                         \
            PANIC("Invalid offset");                                             \
        ((uint8_t *)dev->bar)[off / sizeof(uint8_t)] = val;                     \
    }

#endif /* CARDINAL_MMIO_BAR_H */
