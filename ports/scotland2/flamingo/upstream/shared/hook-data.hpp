#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "calling-convention.hpp"
#include "hook-metadata.hpp"
#include "type-info.hpp"

namespace flamingo {

/// @brief Represents a hook that a user of this library will use.
/// On install, we collect this information into a single TargetInfo structure, which contains a collection of multiple
/// Hook references. We map target --> TargetInfo and every time we have a new hook installed there, we move orig
/// pointers around accordingly. To do priorities, we have to track that state within a given TargetInfo's hooks to
/// determine a suitable location to install.
struct HookInfo {
  // TODO: friend struct this to something?
  friend struct TargetData;
  template <class R, class... TArgs>
  using HookFuncType = R (*)(TArgs...);

  /// @brief The default number of instructions a target has
  constexpr static uint16_t kDefaultNumInsts = 5U;

  // Helper constructor for the bare minimum
  template <class R, class... TArgs>
  HookInfo(HookFuncType<R, TArgs...> hook_func, void* target, HookFuncType<R, TArgs...>* orig_ptr)
      : HookInfo(hook_func, target, orig_ptr, kDefaultNumInsts, CallingConvention::Cdecl,
                 HookNameMetadata{ .name = "" }, HookPriority{},
                 InstallationMetadata{ .need_orig = orig_ptr != nullptr, .is_midpoint = false, .write_prot = false }) {}

  // Helper function to make it really easy to set installation metadata
  template <class R, class... TArgs>
  HookInfo(HookFuncType<R, TArgs...> hook_func, void* target, HookFuncType<R, TArgs...>* orig_ptr,
           InstallationMetadata&& metadata)
      : HookInfo(hook_func, target, orig_ptr, kDefaultNumInsts, CallingConvention::Cdecl,
                 HookNameMetadata{ .name = "" }, HookPriority{}, std::forward<InstallationMetadata>(metadata)) {}

  // TODO: Do we want to allow for specific register overrides instead of x17? For allowing for clever midpoint hooks?
  template <class R, class... TArgs>
  HookInfo(HookFuncType<R, TArgs...> hook_func, void* target, HookFuncType<R, TArgs...>* orig_ptr, uint16_t num_insts,
           CallingConvention conv, HookNameMetadata&& name_info, HookPriority&& priority,
           InstallationMetadata const& install_metadata)
      : target(target),
        orig_ptr(reinterpret_cast<void**>(orig_ptr)),
        hook_ptr(reinterpret_cast<void*>(hook_func)),
        metadata(HookMetadata{
          .convention = conv,
          .installation_metadata = install_metadata,
          .method_num_insts = num_insts,
          .name_info = name_info,
          .priority = priority,
#ifndef FLAMINGO_NO_REGISTRATION_CHECKS
          .parameter_info = { TypeInfo::from<TArgs>()... },
          .return_info = TypeInfo::from<R>(),
#endif
        }) {
  }

  HookInfo(void* hook_func, void* target, void** orig_ptr, uint16_t num_insts, CallingConvention conv,
           HookNameMetadata&& name_info, HookPriority&& priority, InstallationMetadata const& install_metadata)
      : target(target),
        orig_ptr(orig_ptr),
        hook_ptr(hook_func),
        metadata(HookMetadata{
          .convention = conv,
          .installation_metadata = install_metadata,
          .method_num_insts = num_insts,
          .name_info = name_info,
          .priority = priority,
#ifndef FLAMINGO_NO_REGISTRATION_CHECKS
          .parameter_info = {},
          .return_info = {},
#endif
        }) {
  }

  HookInfo(void* hook_func, void* target, void** orig_ptr, uint16_t num_insts, CallingConvention conv,
           HookNameMetadata&& name_info, HookPriority&& priority, InstallationMetadata const& install_metadata,
           std::vector<TypeInfo>&& params, TypeInfo&& return_info)
      : target(target),
        orig_ptr(orig_ptr),
        hook_ptr(hook_func),
        metadata(HookMetadata{
          .convention = conv,
          .installation_metadata = install_metadata,
          .method_num_insts = num_insts,
          .name_info = name_info,
          .priority = priority,
#ifndef FLAMINGO_NO_REGISTRATION_CHECKS
          .parameter_info = std::move(params),
          .return_info = return_info,
#endif
        }) {
  }

  HookInfo(void* hook_func, void* target, void** orig_ptr, HookNameMetadata&& name_info, std::vector<TypeInfo>&& params,
           TypeInfo&& return_info)
      : HookInfo(hook_func, target, orig_ptr, kDefaultNumInsts, CallingConvention::Cdecl, std::move(name_info),
                 HookPriority{},
                 InstallationMetadata{ .need_orig = orig_ptr != nullptr, .is_midpoint = false, .write_prot = false },
                 std::move(params), std::move(return_info)) {}

  HookInfo(void* hook_func, void* target, void** orig_ptr)
      : HookInfo(hook_func, target, orig_ptr, kDefaultNumInsts, CallingConvention::Cdecl,
                 HookNameMetadata{ .name = "" }, HookPriority{},
                 InstallationMetadata{ .need_orig = orig_ptr != nullptr, .is_midpoint = false, .write_prot = false }) {}

  HookInfo(void* hook_func, void* target, void** orig_ptr, HookNameMetadata&& name_info)
      : HookInfo(hook_func, target, orig_ptr, kDefaultNumInsts, CallingConvention::Cdecl, std::move(name_info),
                 HookPriority{},
                 InstallationMetadata{ .need_orig = orig_ptr != nullptr, .is_midpoint = false, .write_prot = false }) {}

  HookInfo(void* hook_func, void* target, void** orig_ptr, HookNameMetadata&& name_info, HookPriority&& priority)
      : HookInfo(hook_func, target, orig_ptr, kDefaultNumInsts, CallingConvention::Cdecl, std::move(name_info),
                 std::move(priority),
                 InstallationMetadata{ .need_orig = orig_ptr != nullptr, .is_midpoint = false, .write_prot = false }) {}

  void assign_orig(void* ptr) {
    if (orig_ptr != nullptr) *orig_ptr = ptr;
  }

  void* target;
  void** orig_ptr;
  void* hook_ptr;
  HookMetadata metadata;
};

}  // namespace flamingo
