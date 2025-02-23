#pragma once

#include <memory/range.hpp>

namespace memory {

// Memory sizes
static constexpr u32 BIOS_SIZE       = 512  * 1024;      // All BIOS images are 512KB
static constexpr u32 RAM_SIZE        = 2048 * 1024;  // Main RAM, 2 MB
static constexpr u32 SCRATCHPAD_SIZE = 1    * 1024;      // Scratch pad, 1KB
static constexpr u32 SPU_SIZE        = 0x280;
static constexpr u32 EXPANSION_1_SIZE = 1024 * 1024;

namespace map {

// Memory map (physical addresses)
static constexpr Range RAM{ 0x00000000, RAM_SIZE };
static constexpr Range BIOS{ 0x1FC00000, BIOS_SIZE };
static constexpr Range SPU{ 0x1F801C00, SPU_SIZE };
static constexpr Range MEM_CONTROL1{ 0x1F801000, 0x24 };
static constexpr Range MEM_CONTROL2{ 0x1F801060, 4 };
static constexpr Range MEM_CONTROL3{ 0xFFFE0130, 4 };
static constexpr Range EXPANSION_1{ 0x1F000000, EXPANSION_1_SIZE };
static constexpr Range EXPANSION_2{ 0x1F802000, 0x42 };
static constexpr Range IRQ_CONTROL{ 0x1F801070, 8 };
static constexpr Range TIMERS{ 0x1F801100, 0x2C };
static constexpr Range DMA{ 0x1F801080, 0x80 };
static constexpr Range GPU{ 0x1F801810, 8 };
static constexpr Range SCRATCHPAD{ 0x1F800000, SCRATCHPAD_SIZE };
static constexpr Range JOYPAD{ 0x1F801040, 0x10 };
static constexpr Range SIO{ 0x1F801050, 0x10 };
static constexpr Range CDROM{ 0x1F801800, 4 };

}  // namespace map

inline u32 mask_region(u32 addr) {
  constexpr u32 region_mask[8] = {
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,  // KUSEG: 2048MB, already physaddr, no need to mask
    0x7FFFFFFF,                                      // KSEG0: 512MB, mask top bit
    0x1FFFFFFF,                                      // KSEG1: 512MB, mask top 3 bits
    0xFFFFFFFF, 0xFFFFFFFF                           // KSEG1: 1024MB, already physaddr, no need to mask
  };
  // Use addr's top 3 bits to determine the region and index into region_map
  return addr & region_mask[addr >> 29];
}

}  // namespace memory
