#pragma once

#include <span>
#include <string>

// NOTE: This is 64 bit specific!
// For 32 bit support, this file will need to support Elf32_Shdr*, etc.
namespace elf_utils {

  template <typename T>
  T& readAtOffset(std::span<uint8_t> f, uint64_t offset) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return *reinterpret_cast<T*>(&f[offset]);
  }

  template <typename T>
  std::span<T> readManyAtOffset(std::span<uint8_t> f, uint64_t offset, size_t amount, size_t size) noexcept {
    uint8_t* begin = &readAtOffset<uint8_t>(f, offset);
    uint8_t* end = begin + (amount * size);
    return std::span<T>(reinterpret_cast<T*>(begin), reinterpret_cast<T*>(end));
  }
  
  void* getSymbol(std::span<uint8_t> f, std::string_view symbol_name);

  uintptr_t baseAddr(char const* soname);
}  // namespace