#pragma once

#include <cstdint>
#include <list>
#include <optional>

#include "fixups.hpp"
#include "hook-data.hpp"
#include "hook-metadata.hpp"

namespace flamingo {

struct TargetDescriptor {
  void* target;
};

/// @brief Represents the metadata associated with a target. POD, constructed from HookMetadata
struct TargetMetadata {
  PointerWrapper<uint32_t> target;
  CallingConvention convention;
  InstallationMetadata metadata;
  uint16_t method_num_insts;
#ifndef FLAMINGO_NO_REGISTRATION_CHECKS
  std::vector<TypeInfo> parameter_info;
  TypeInfo return_info;
#endif
};

inline bool operator<(TargetDescriptor const& lhs, TargetDescriptor const& rhs) {
  return reinterpret_cast<uintptr_t>(lhs.target) < reinterpret_cast<uintptr_t>(rhs.target);
}

/// @brief Represents the status of a particular address
/// If hooked, will contain the same members as a hook, but additionally with a list of Hooks
/// The idea being we can O(1) install hooks (and uninstall via iterator)
struct TargetData {
  TargetMetadata metadata;
  Fixups fixups;
  std::list<HookInfo> hooks{};
};

/// @brief A handle to an installed hook. Used for uninstalls.
struct [[nodiscard("HookHandle instances must be used for uninstalls or explicitly thrown away")]] HookHandle {
  std::list<HookInfo>::iterator hook_location;
};

}  // namespace flamingo
