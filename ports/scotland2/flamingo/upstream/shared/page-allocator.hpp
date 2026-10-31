#pragma once
#include <fmt/format.h>
#include <sys/mman.h>
#include <algorithm>
#include <cstdint>
#include <span>
#include "util.hpp"

namespace flamingo {
enum struct [[clang::flag_enum]] PageProtectionType : int {
  kNone = PROT_NONE,
  kRead = PROT_READ,
  kWrite = PROT_WRITE,
  kExecute = PROT_EXEC,
};

inline PageProtectionType operator|(PageProtectionType lhs, PageProtectionType rhs) {
  return static_cast<PageProtectionType>(static_cast<int>(lhs) | static_cast<int>(rhs));
}
inline PageProtectionType& operator|=(PageProtectionType& lhs, PageProtectionType rhs) {
  return lhs = lhs | rhs;
}
inline PageProtectionType operator&(PageProtectionType lhs, PageProtectionType rhs) {
  return static_cast<PageProtectionType>(static_cast<int>(lhs) & static_cast<int>(rhs));
}
inline PageProtectionType& operator&=(PageProtectionType& lhs, PageProtectionType rhs) {
  return lhs = lhs & rhs;
}

struct Page {
  constexpr static uint_fast16_t PageSize = 4096;
  void* ptr;
  uint_fast16_t used_size;
  PageProtectionType protection;

  static auto PageAlign(auto ptr) {
    return reinterpret_cast<decltype(ptr)>(reinterpret_cast<uint64_t>(ptr) & (~(static_cast<size_t>(PageSize) - 1)));
  }
};

// Holds a pointer with a size and a protection.
// Provides a way of protecting the memory at this pointer by page aligning
template <class T>
struct PointerWrapper {
  std::span<T> addr;
  PageProtectionType protection;

  PointerWrapper(std::span<T> addr, PageProtectionType prot) : addr(addr), protection(prot) {}
  PointerWrapper(PointerWrapper const&) = default;

  void Protect() const {
    // If we have nothing in the address, don't bother protecting
    if (addr.empty()) return;
    auto const page_aligned = Page::PageAlign(addr.data());
    auto const page_offset = reinterpret_cast<uint64_t>(addr.data()) % Page::PageSize;
    if (::mprotect(page_aligned, addr.size_bytes() + page_offset, static_cast<int>(protection)) != 0) {
      // Log error on mprotect!
      FLAMINGO_ABORT("Failed to mark ptr at: {} (page aligned: {}) with size: {} with permissions: {}. err: {}",
                     fmt::ptr(addr.data()), fmt::ptr(page_aligned), page_offset + addr.size_bytes(),
                     static_cast<int>(protection), std::strerror(errno));
    }
  }
  /// @brief Returns a subspan that is of the (potentially shrunken) size.
  PointerWrapper<T> Subspan(size_t n) const {
    return PointerWrapper(addr.first(std::min(n, addr.size())), protection);
  }
};

PointerWrapper<uint32_t> Allocate(uint_fast16_t alignment, uint_fast16_t size, PageProtectionType protection);

}  // namespace flamingo
