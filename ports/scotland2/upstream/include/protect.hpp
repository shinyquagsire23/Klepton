#pragma once
#include <fmt/format.h>
#include <sys/mman.h>
#include <fstream>
#include <sstream>
#include "log.h"

namespace modloader {

/// @brief Protects the provided address by marking it with the provided permissions
/// @param addr The address to protect
/// @param size The size to protect the range in. Must be page aligned.
/// @param perms The permissions to use for mprotect. Must be valid mprotect permission values.
inline void protect(void* addr, size_t size, int perms) {
  LOG_INFO("Protecting memory: {} (0x{:x}) with perms: {}", fmt::ptr(addr), size, perms);
  if (mprotect(addr, size, perms) != 0) {
    LOG_ERROR("Protection failed! error: {}", std::strerror(errno));
  }
}

/// @brief Protects all shared objects extracted from /proc/self/maps by marking all entries that were +x as +rx
inline void protect_all() {
  // If we look at /proc/self/maps we can see most of what we would probably care about.
  // For each of the things in /proc/self/maps, we probably want to look at the addresses of them and attempt to
  // mprotect with a READ as well as an execute.
  constexpr static auto hexBase = 16;
  LOG_VERBOSE("Protecting memory from /proc/self/maps!");
  std::ifstream procMap("/proc/self/maps");
  std::string line;
  while (std::getline(procMap, line)) {
    auto idx = line.find_first_of('-');
    if (idx == std::string::npos) {
      LOG_ERROR("Could not find '-' in line: {}", line.c_str());
      continue;
    }
    auto startAddr = std::stoul(line.substr(0, idx), nullptr, hexBase);
    auto spaceIdx = line.find_first_of(' ');
    if (spaceIdx == std::string::npos) {
      LOG_ERROR("Could not find ' ' in line: {}", line.c_str());
      continue;
    }
    auto endAddr = std::stoul(line.substr(idx + 1, spaceIdx - idx - 1), nullptr, hexBase);
    // Permissions are 4 characters
    auto perms = line.substr(spaceIdx + 1, 4);
    if (perms.find('r') == std::string::npos && perms.find('x') != std::string::npos &&
        perms.find('w') == std::string::npos) {
      LOG_VERBOSE("Line: {}", line.c_str());
      // If we have execute, and we do not have read, and we do not have write, we need to protect.
      LOG_INFO("Protecting memory: 0x{:x} - 0x{:x} with perms: {} to: +rx", startAddr, endAddr, perms.c_str());
      protect(reinterpret_cast<void*>(startAddr), endAddr - startAddr, PROT_EXEC | PROT_READ);
    }
  }
}
}  // namespace modloader
