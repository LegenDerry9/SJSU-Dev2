#pragma once

#include <chrono>
#include "L1_Peripheral/eeprom.hpp"

#include "L0_Platform/lpc40xx/LPC40xx.h"
#include "L1_Peripheral/lpc40xx/system_controller.hpp"

namespace sjsu
{
namespace lpc40xx
{
class Eeprom final : public sjsu::Eeprom
{
 public:
  inline static LPC_EEPROM_TypeDef * eeprom_register = LPC_EEPROM;

  static constexpr uint32_t kAddressMask = 0b1111'1111'1100;

  static constexpr uint8_t kProgramStatusBit   = 28;
  static constexpr uint8_t kReadWriteStatusBit = 26;

  // EEPROM Command codes
  static constexpr uint8_t kRead32Bits    = 0b010;
  static constexpr uint8_t kWrite32Bits   = 0b101;
  static constexpr uint8_t kEraseProgram  = 0b110;

  // Max timeout for program/write operations
  static constexpr uint16_t kMaxTimeout = 20;

  // Getting actual SystemController Object
  static constexpr sjsu::lpc40xx::SystemController kLpc40xxSystemController =
      sjsu::lpc40xx::SystemController();

  // Constructor has an optional SystemController parameter which can be
  // specified for testing purposes. Otherwise, the actual SystemController
  // object is used (kLpc40xxSystemController)
  explicit constexpr Eeprom(const sjsu::SystemController & system_controller =
                             kLpc40xxSystemController)
      : system_controller_(system_controller)
  {
  }

  // Initializing the EEPROM requires setting the wait state register,
  // and the clock divider register
  void Initialize() const override
  {
    const float kSystemClock    = static_cast<float>
                                    (system_controller_.GetSystemFrequency());
    constexpr float kEepromClk  = 375'000;  // EEPROM runs at 375 kHz
    constexpr float kNanosecond = 1E-9f;

    // The EEPROM is turned on by default, but in case it was somehow
    // turned off, we turn it on by writing 0 to the PWRDWN register
    eeprom_register->PWRDWN = 0;

    // Initialzie EEPROM wait state register with number of wait states
    // for each of its internal phases
    // Phase 3 (15 ns)
    eeprom_register->WSTATE |=
        static_cast<uint8_t>((15 * kNanosecond * kSystemClock) + 1);
    // Phase 2 (55 ns)
    eeprom_register->WSTATE |=
        (static_cast<uint8_t>((55 * kNanosecond * kSystemClock) + 1)) << 8;
    // Phase 1 (35 ns)
    eeprom_register->WSTATE |=
        (static_cast<uint8_t>((35 * kNanosecond * kSystemClock) + 1)) << 16;

    // Initialize EEPROM clock
    eeprom_register->CLKDIV = static_cast<uint8_t>(kSystemClock / kEepromClk);
  }

  // The EEPROM can only be interfaced in 32-bit mode, which means that the
  // address used must be a multiple of 4. The address entered will be cut
  // down to a multiple of 4.
  void Write(const uint8_t * wdata, uint32_t full_address,
             size_t count) const override
  {
    constexpr uint8_t kMax6Bits = 0b11'1111;

    // Writing a 32-bit number to an address that isn't a multiple of 4 will
    // cause a hard fault, thus this safety is put into place to avoid that.
    full_address &= kAddressMask;

    // Because the EEPROM uses 32-bit communication, write_data will be casted
    // into a uint32_t *
    const uint32_t * write_data = reinterpret_cast<const uint32_t *>(wdata);

    // The first 6 bits in the address (MSB) dictate which page is being written
    // to in the EEPROM, and the last 6 bits (LSB) dictate offset in the page
    uint8_t page_count  = static_cast<uint8_t>(full_address >> 6);
    uint8_t page_offset = full_address & 0b0000'0011'1111;

    uint16_t address;

    for (size_t i = 0; i*4 < count; i++)
    {
      // Page offset is incremented by 4 because we're writing 32 bits
      constexpr uint8_t kOffsetInterval = 4;

      address = static_cast<uint16_t>((page_count << 6) + page_offset);

      eeprom_register->ADDR  = address;
      eeprom_register->CMD   = kWrite32Bits;
      eeprom_register->WDATA = write_data[i];

      // Poll status register bit to see when writing is finished
      auto check_register = [&] ()
      {
        return !(eeprom_register->INT_STATUS >> kReadWriteStatusBit & 1);
      };

      Wait(std::chrono::milliseconds(kMaxTimeout), check_register);

      // Clear write interrupt
      eeprom_register->INT_CLR_STATUS = (1 << kReadWriteStatusBit);

      page_offset = static_cast<uint8_t>(page_offset + kOffsetInterval);

      // If the 64 byte page buffer fills up, then it must be programmed to the
      // EEPROM before continuing.
      if (page_offset > kMax6Bits)
      {
        Program(address);
        page_count++;
        page_offset = 0;
      }
    }

    // Program final information so that it's stored in the EEPROM
    Program(address);
  }

  // The EEPROM is accessed through a 64 byte page buffer, and after it fills
  // up, it must be programmed to the actual EEPROM. This function handles that.
  void Program(uint32_t address) const
  {
    eeprom_register->ADDR = address;
    eeprom_register->CMD  = kEraseProgram;

    // Poll status register bit to see when writing is finished
    auto check_register = [&] ()
    {
      return !(eeprom_register->INT_STATUS >> kProgramStatusBit & 1);
    };

    // Wait((!(eeprom_register->INT_STATUS >> kReadWriteStatusBit)) & 1);
    Wait(std::chrono::milliseconds(kMaxTimeout), check_register);

    // Clear program interrupt
    eeprom_register->INT_CLR_STATUS = (1 << kProgramStatusBit);
  }

  // This function will return however much 32-bit data from the EEPROM
  // starting at address and continuing on for how large count is. The EEPROM
  // can only be interfaced in 32-bit mode, which means that the address used
  // must be a multiple of 4. The address entered will be cut down to a
  // multiple of 4.
  void Read(uint8_t * rdata, uint32_t address, size_t count) const override
  {
    // Writing a 32-bit number to an address that isn't a multiple of 4 will
    // cause a hard fault, thus this safety is put into place to avoid that.
    address &= kAddressMask;

    // Because the EEPROM uses 32-bit communication, read_data will be casted
    // into a uint32_t *
    uint32_t * read_data = reinterpret_cast<uint32_t *>(rdata);

    for (uint16_t index = 0; (index * 4) < count; index++)
    {
      eeprom_register->ADDR = address + (index * 4);
      eeprom_register->CMD  = kRead32Bits;
      read_data[index]      = eeprom_register->RDATA;
    }
  }

 private:
  const sjsu::SystemController & system_controller_;
};
}  // namespace lpc40xx
}  // namespace sjsu
