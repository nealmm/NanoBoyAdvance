/*
 * Copyright (C) 2020 fleroviux
 *
 * Licensed under GPLv3 or any later version.
 * Refer to the included LICENSE file.
 */

/* TODO: add support for Big-Endian architectures. */

inline std::uint32_t CPU::ReadBIOS(std::uint32_t address) {
  int shift = (address & 3) * 8;
  
  address &= ~3;

  if (address >= 0x4000) {
    return ReadUnused(address) >> shift;
  }
  
  if (state.r15 >= 0x4000) {
    return memory.bios_opcode >> shift;
  }

  memory.bios_opcode = Read<std::uint32_t>(memory.bios, address);

  return memory.bios_opcode >> shift;
}

inline std::uint32_t CPU::ReadUnused(std::uint32_t address) {
  std::uint32_t result = 0;
  
  if (state.cpsr.f.thumb) {
    auto r15 = state.r15;
  
    switch (r15 >> 24) {
      case REGION_EWRAM:
      case REGION_PRAM:
      case REGION_VRAM:
      case REGION_ROM_W0_L:
      case REGION_ROM_W0_H:
      case REGION_ROM_W1_L:
      case REGION_ROM_W1_H:
      case REGION_ROM_W2_L:
      case REGION_ROM_W2_H: {
        result = GetPrefetchedOpcode(1) * 0x00010001;
        break;
      }
      case REGION_BIOS:
      case REGION_OAM: {
        if (r15 & 3) {
          result = GetPrefetchedOpcode(0) |
                  (GetPrefetchedOpcode(1) << 16);
        } else {
          /* FIXME: this is not correct, but also [$+6] has not been prefetched at this point. */
          result = GetPrefetchedOpcode(1) * 0x00010001;
        }
        break;
      }
      case REGION_IWRAM: {
        if (r15 & 3) {
          result = GetPrefetchedOpcode(0) |
                  (GetPrefetchedOpcode(1) << 16);
        } else {
          result = GetPrefetchedOpcode(1) |
                  (GetPrefetchedOpcode(0) << 16);
        }
        break;
      }
    }
  } else {
    result = GetPrefetchedOpcode(1);
  }
    
  return result >> ((address & 3) * 8);
}

inline auto CPU::ReadByte(std::uint32_t address, Access access) -> std::uint8_t {
  int page = address >> 24;
  int cycles = cycles16[int(access)][page];
  
  if (mmio.waitcnt.prefetch) {
    PrefetchStep(address, cycles);
  } else {
    Tick(cycles);
  }

  switch (page) {
    case REGION_BIOS: {
      return ReadBIOS(address);
    }
    case REGION_EWRAM: {
      return Read<std::uint8_t>(memory.wram, address & 0x3FFFF);
    }
    case REGION_IWRAM: {
      return Read<std::uint8_t>(memory.iram, address & 0x7FFF);
    }
    case REGION_MMIO: {
      return ReadMMIO(address);
    }
    case REGION_PRAM: {
      return Read<std::uint8_t>(memory.pram, address & 0x3FF);
    }
    case REGION_VRAM: {
      address &= 0x1FFFF;
      if (address >= 0x18000)
        address &= ~0x8000;
      return Read<std::uint8_t>(memory.vram, address);
    }
    case REGION_OAM: {
      return Read<std::uint8_t>(memory.oam, address & 0x3FF);
    }
    case REGION_ROM_W0_L:
    case REGION_ROM_W0_H:
    case REGION_ROM_W1_L:
    case REGION_ROM_W1_H:
    case REGION_ROM_W2_L:
    case REGION_ROM_W2_H: {
      address &= memory.rom.mask;
      if ((address & 0x1FFFF) == 0) {
        Tick(cycles16[int(Access::Nonsequential)][page] - 
             cycles16[int(access)][page]);
      }
      // if (IS_GPIO_ACCESS(address) && gpio->isReadable()) {
      //   return gpio->read(address);
      // }
      if (address >= memory.rom.size) {
        return address / 2;
      }
      return Read<std::uint8_t>(memory.rom.data.get(), address);
    }
    case REGION_SRAM_1:
    case REGION_SRAM_2: {
      address &= 0x0EFFFFFF;
      if (!memory.rom.backup || HasEEPROMBackup()) {
        return 0;
      }
      return memory.rom.backup->Read(address);
    }
    
    default: {
      return ReadUnused(address);
    }
  }
}

inline auto CPU::ReadHalf(std::uint32_t address, Access access) -> std::uint16_t {
  int page = address >> 24;
  int cycles = cycles16[int(access)][page];
  
  if (mmio.waitcnt.prefetch) {
    PrefetchStep(address, cycles);
  } else {
    Tick(cycles);
  }
  
  switch (page) {
    case REGION_BIOS: {
      return ReadBIOS(address);
    }
    case REGION_EWRAM: {
      return Read<std::uint16_t>(memory.wram, address & 0x3FFFF);
    }
    case REGION_IWRAM: {
      return Read<std::uint16_t>(memory.iram, address & 0x7FFF );
    }
    case REGION_MMIO: {
      return  ReadMMIO(address + 0) |
             (ReadMMIO(address + 1) << 8);
    }
    case REGION_PRAM: {
      return Read<std::uint16_t>(memory.pram, address & 0x3FF);
    }
    case REGION_VRAM: {
      address &= 0x1FFFF;
      if (address >= 0x18000)
        address &= ~0x8000;
      return Read<std::uint16_t>(memory.vram, address);
    }
    case REGION_OAM: {
      return Read<std::uint16_t>(memory.oam, address & 0x3FF);
    }
    
    /* 0x0DXXXXXX may be used to read/write from EEPROM */
    case REGION_ROM_W2_H: {
      /* Must check if this is an EEPROM access or ordinary ROM mirror read. */
      if (IsEEPROMAddress(address)) {
        /* TODO: this is not a very nice way to do this. */
        if (!dma.IsRunning()) {
          return 1;
        }
        return memory.rom.backup->Read(address);
      }
      /* falltrough */
    }
    case REGION_ROM_W0_L:
    case REGION_ROM_W0_H:
    case REGION_ROM_W1_L:
    case REGION_ROM_W1_H:
    case REGION_ROM_W2_L: {
      address &= memory.rom.mask;
      if ((address & 0x1FFFF) == 0) {
        Tick(cycles16[int(Access::Nonsequential)][page] - 
             cycles16[int(access)][page]);
      }
      // if (IS_GPIO_ACCESS(address) && gpio->isReadable()) {
      //   return  gpio->read(address) |
      //      (gpio->read(address + 1) << 8);
      // }
      if (address >= memory.rom.size)
        return address / 2;
      return Read<std::uint16_t>(memory.rom.data.get(), address);
    }
    case REGION_SRAM_1:
    case REGION_SRAM_2: {
      address &= 0x0EFFFFFF;
      if (!memory.rom.backup || HasEEPROMBackup()) {
        return 0;
      }
      return memory.rom.backup->Read(address) * 0x0101;
    }

    default: {
      return ReadUnused(address);
    }
  }
}

inline auto CPU::ReadWord(std::uint32_t address, Access access) -> std::uint32_t {
  int page = address >> 24;
  int cycles = cycles32[int(access)][page];
  
  if (mmio.waitcnt.prefetch) {
    PrefetchStep(address, cycles);
  } else {
    Tick(cycles);
  }
  
  switch (page) {
    case REGION_BIOS: {
      return ReadBIOS(address);
    }
    case REGION_EWRAM: {
      return Read<std::uint32_t>(memory.wram, address & 0x3FFFF);
    }
    case REGION_IWRAM: {
      return Read<std::uint32_t>(memory.iram, address & 0x7FFF );
    }
    case REGION_MMIO: {
      return ReadMMIO(address + 0) |
            (ReadMMIO(address + 1) << 8 ) |
            (ReadMMIO(address + 2) << 16) |
            (ReadMMIO(address + 3) << 24);
    }
    case REGION_PRAM: {
      return Read<std::uint32_t>(memory.pram, address & 0x3FF);
    }
    case REGION_VRAM: {
      address &= 0x1FFFF;
      if (address >= 0x18000)
        address &= ~0x8000;
      return Read<std::uint32_t>(memory.vram, address);
    }
    case REGION_OAM: {
      return Read<std::uint32_t>(memory.oam, address & 0x3FF);
    }
    case REGION_ROM_W0_L:
    case REGION_ROM_W0_H:
    case REGION_ROM_W1_L:
    case REGION_ROM_W1_H:
    case REGION_ROM_W2_L:
    case REGION_ROM_W2_H: {
      address &= memory.rom.mask;
      if ((address & 0x1FFFF) == 0) {
        Tick(cycles32[int(Access::Nonsequential)][page] - 
             cycles32[int(access)][page]);
      }
      // if (IS_GPIO_ACCESS(address) && gpio->isReadable()) {
      //   return  gpio->read(address)      |
      //      (gpio->read(address + 1) << 8)  |
      //      (gpio->read(address + 2) << 16) |
      //      (gpio->read(address + 3) << 24);
      // }
      if (address >= memory.rom.size) {
        return (((address + 0) / 2) & 0xFFFF) |
               (((address + 2) / 2) << 16);
      }
      return Read<std::uint32_t>(memory.rom.data.get(), address);
    }
    case REGION_SRAM_1:
    case REGION_SRAM_2: {
      address &= 0x0EFFFFFF;
      if (!memory.rom.backup || HasEEPROMBackup()) {
        return 0;
      }
      return memory.rom.backup->Read(address) * 0x01010101;
    }

    default: {
      return ReadUnused(address);
    }
  }
}

inline void CPU::WriteByte(std::uint32_t address, std::uint8_t value, Access access) {
  int page = address >> 24;
  int cycles = cycles16[int(access)][page];

  if (mmio.waitcnt.prefetch) {
    PrefetchStep(address, cycles);
  } else {
    Tick(cycles);
  }

  switch (page) {
    case REGION_EWRAM:
      Write<std::uint8_t>(memory.wram, address & 0x3FFFF, value);
      break;
    case REGION_IWRAM: {
      Write<std::uint8_t>(memory.iram, address & 0x7FFF,  value);
      break;
    }
    case REGION_MMIO: {
      WriteMMIO(address, value & 0xFF);
      break;
    }
    case REGION_PRAM: {
      Write<std::uint16_t>(memory.pram, address & 0x3FF, value * 0x0101);
      break;
    }
    case REGION_VRAM: {
      address &= 0x1FFFF;
      if (address >= 0x18000) {
        address &= ~0x8000;
      }
      if (address >= 0x10000) {
        break;
      }
      Write<std::uint16_t>(memory.vram, address, value * 0x0101);
      break;
    }
    case REGION_SRAM_1:
    case REGION_SRAM_2: {
      address &= 0x0EFFFFFF;
      if (!memory.rom.backup || HasEEPROMBackup()) {
        break;
      }
      memory.rom.backup->Write(address, value);
      break;
    }
  }
}

inline void CPU::WriteHalf(std::uint32_t address, std::uint16_t value, Access access) {
  int page = address >> 24;
  int cycles = cycles16[int(access)][page];

  if (mmio.waitcnt.prefetch) {
    PrefetchStep(address, cycles);
  } else {
    Tick(cycles);
  }

  switch (page) {
    case REGION_EWRAM: {
      Write<std::uint16_t>(memory.wram, address & 0x3FFFF, value);
      break;
    }
    case REGION_IWRAM: {
      Write<std::uint16_t>(memory.iram, address & 0x7FFF,  value);
      break;
    }
    case REGION_MMIO: {
      WriteMMIO(address + 0, (value >> 0) & 0xFF);
      WriteMMIO(address + 1, (value >> 8) & 0xFF);
      break;
    }
    case REGION_PRAM: {
      Write<std::uint16_t>(memory.pram, address & 0x3FF, value);
      break;
    }
    case REGION_VRAM: {
      address &= 0x1FFFF;
      if (address >= 0x18000) {
        address &= ~0x8000;
      }
      Write<std::uint16_t>(memory.vram, address, value);
      break;
    }
    case REGION_OAM: {
      Write<std::uint16_t>(memory.oam, address & 0x3FF, value);
      break;
    }
    // case REGION_ROM_W0_L: case REGION_ROM_W0_H:
    // case REGION_ROM_W1_L: case REGION_ROM_W1_H:
    // case REGION_ROM_W2_L: {
    //   address &= 0x1FFFFFF;
    //   if (IS_GPIO_ACCESS(address)) {
    //     gpio->write(address+0, value&0xFF);
    //     gpio->write(address+1, value>>8);
    //   }
    //   break;
    // }

    /* EEPROM write */
    case REGION_ROM_W2_H: {
      if (IsEEPROMAddress(address)) {
        /* TODO: this is not a very nice way to do this. */
        if (!dma.IsRunning()) {
          break;
        }
        memory.rom.backup->Write(address, value);
        break;
      }
    //   address &= 0x1FFFFFF;
    //   if (IS_GPIO_ACCESS(address)) {
    //     gpio->write(address+0, value&0xFF);
    //     gpio->write(address+1, value>>8);
    //     break;
    //   }
      break;
    }
    
    case REGION_SRAM_1:
    case REGION_SRAM_2: {
      address &= 0x0EFFFFFF;
      if (!memory.rom.backup || HasEEPROMBackup()) {
        break;
      }
      memory.rom.backup->Write(address + 0, value);
      memory.rom.backup->Write(address + 1, value);
      break;
    }
  }
}

inline void CPU::WriteWord(std::uint32_t address, std::uint32_t value, Access access) {
  int page = address >> 24;
  int cycles = cycles32[int(access)][page];

  if (mmio.waitcnt.prefetch) {
    PrefetchStep(address, cycles);
  } else {
    Tick(cycles);
  }

  switch (page) {
    case REGION_EWRAM: {
      Write<std::uint32_t>(memory.wram, address & 0x3FFFF, value);
      break;
    }
    case REGION_IWRAM: {
      Write<std::uint32_t>(memory.iram, address & 0x7FFF,  value);
      break;
    }
    case REGION_MMIO: {
      WriteMMIO(address + 0, (value >>  0) & 0xFF);
      WriteMMIO(address + 1, (value >>  8) & 0xFF);
      WriteMMIO(address + 2, (value >> 16) & 0xFF);
      WriteMMIO(address + 3, (value >> 24) & 0xFF);
      break;
    }
    case REGION_PRAM: {
      Write<std::uint32_t>(memory.pram, address & 0x3FF, value);
      break;
    }
    case REGION_VRAM: {
      address &= 0x1FFFF;
      if (address >= 0x18000) {
        address &= ~0x8000;
      }
      Write<std::uint32_t>(memory.vram, address, value);
      break;
    }
    case REGION_OAM: {
      Write<std::uint32_t>(memory.oam, address & 0x3FF, value);
      break;
    }

    // case REGION_ROM_W0_L: case REGION_ROM_W0_H:
    // case REGION_ROM_W1_L: case REGION_ROM_W1_H:
    // case REGION_ROM_W2_L: case REGION_ROM_W2_H: {
    //   // TODO: check if 32-bit EEPROM accesses are possible.
    //   address &= 0x1FFFFFF;
    //   if (IS_GPIO_ACCESS(address)) {
    //     gpio->write(address+0, (value>>0) &0xFF);
    //     gpio->write(address+1, (value>>8) &0xFF);
    //     gpio->write(address+2, (value>>16)&0xFF);
    //     gpio->write(address+3, (value>>24)&0xFF);
    //   }
    //   break;
    // }

    case REGION_SRAM_1:
    case REGION_SRAM_2: {
      address &= 0x0EFFFFFF;
      if (!memory.rom.backup || HasEEPROMBackup()) {
        break;
      }
      memory.rom.backup->Write(address + 0, value);
      memory.rom.backup->Write(address + 1, value);
      memory.rom.backup->Write(address + 2, value);
      memory.rom.backup->Write(address + 3, value);
      break;
    }
  }
}