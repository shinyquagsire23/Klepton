// Make a page
// Pages should have sizes and are otherwise 4kb
// If we do some allocation and determine that we didnt need the full set of allocation for our
// page, we want to give it back. For now, though, we should just allocate out the pages regardless
// And call a function to finalize our allocation pool or something on the allocator
// Note that in order to properly handle deallocation as well as finalization among other things, we need to handle:
// 1. When an allocation is "complete", shrink the allocation
// 2. Shrinking of allocations need to be done in such a way that future allocations are not broken. i.e. bump allocator
// 3. Deallocations need to be done in such a way that full pages are not destroyed
#include "page-allocator.hpp"
#include <fmt/format.h>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include "util.hpp"

namespace {
constexpr auto AlignUp(auto offset, auto alignment) {
  // We can't align to a size that is greater than our allocation
  __builtin_assume(alignment < flamingo::Page::PageSize);
  // Alignment must be a power of 2
  __builtin_assume((alignment != 0) && ((alignment & (alignment - 1)) == 0));
  if (offset % alignment != 0) {
    return offset + (alignment - (offset % alignment));
  }
  return offset;
}

// We don't want to rely on the dlopen constructor calling this, we will allocate it on first call to Allocate.
// Hence, it's a pointer that we directly manage.
std::unordered_multimap<flamingo::PageProtectionType, flamingo::Page>* all_pages;
}  // namespace

namespace flamingo {

PointerWrapper<uint32_t> Allocate(uint_fast16_t alignment, uint_fast16_t size, PageProtectionType protection) {
  // We assume that size is never > the size of a Page
  // Note: This is NOT a thread safe allocator (for now)
  __builtin_assume(size <= Page::PageSize);
  if (all_pages == nullptr) {
    all_pages = new std::unordered_multimap<PageProtectionType, Page>{};
  }
  // We allocate first by trying to find a matching page that has space
  for (auto& [perms, page] : *all_pages) {
    if (perms == protection) {
      // If we match the protection bits we set
      // Check to see if we have enough free space for an allocation
      auto start_offset = AlignUp(page.used_size, alignment);
      if (Page::PageSize - start_offset >= size) {
        // We have enough space to allocate within an existing page
        page.used_size = start_offset + size;
        return PointerWrapper(
            std::span<uint32_t>{
              reinterpret_cast<uint32_t*>(&reinterpret_cast<uint8_t*>(page.ptr)[start_offset]),
              reinterpret_cast<uint32_t*>(&reinterpret_cast<uint8_t*>(page.ptr)[start_offset + size]) },
            protection);
      }
    }
  }
  // No page exists that has matching permissions and has enough space
  // Make one.
  void* ptr;
  if (::posix_memalign(&ptr, Page::PageSize, Page::PageSize) != 0) {
    // Log error on memalign allocation!
    FLAMINGO_ABORT("Failed to allocate page of size: {} for size: {} with protection: {}. err: {}", Page::PageSize,
                   size, static_cast<int>(protection), std::strerror(errno));
  }
  // Mark full page for protection
  if (::mprotect(ptr, Page::PageSize, static_cast<int>(protection)) != 0) {
    // Log error on mprotect!
    FLAMINGO_ABORT("Failed to mark allocated page at: {} with permissions: {}. err: {}", fmt::ptr(ptr),
                   static_cast<int>(protection), std::strerror(errno));
  }
  auto const page = all_pages->emplace(protection, Page{ .ptr = ptr, .used_size = size, .protection = protection });
  FLAMINGO_DEBUG("Allocated fixup page with ptr: {} with size: {}", fmt::ptr(ptr), size);
  return PointerWrapper(
      std::span<uint32_t>{ reinterpret_cast<uint32_t*>(&reinterpret_cast<uint8_t*>(page->second.ptr)[0]),
                           reinterpret_cast<uint32_t*>(&reinterpret_cast<uint8_t*>(page->second.ptr)[size]) },
      protection);
}

}  // namespace flamingo
