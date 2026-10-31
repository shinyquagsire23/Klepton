#include "installer.hpp"
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <list>
#include <map>
#include <span>
#include <variant>
#include "fixups.hpp"
#include "hook-data.hpp"
#include "hook-installation-result.hpp"
#include "hook-metadata.hpp"
#include "page-allocator.hpp"
#include "target-data.hpp"
#include "util.hpp"
#ifdef SC2_AOT_HOOKS
#include <mutex>
extern "C" void *sc2_aot_original(const void *);
extern "C" void *sc2_aot_current(const void *);
extern "C" void sc2_aot_begin();
extern "C" void sc2_aot_end();
struct AOTTransaction {
  AOTTransaction() { sc2_aot_begin(); }
  ~AOTTransaction() { sc2_aot_end(); }
};
#endif

/// @brief This function is assigned to the orig of a hook when the hook in question has no fixups written.
extern "C" void no_fixups() {
  FLAMINGO_ABORT(
      "CALL TO ORIG ON FUNCTION WHERE NO ORIG IS PRESENT! THIS WOULD NORMALLY RESULT IN A REALLY ANNOYING JUMP TO 0!");
}
namespace {
using namespace flamingo;

/// @brief The set of all targets hooked. An ordered map so we can perform large-scale walks by doing binary search.
inline static std::map<TargetDescriptor, TargetData> targets;
#ifdef SC2_AOT_HOOKS
inline static std::recursive_mutex install_mutex;
#endif

Result<std::list<HookInfo>::iterator, installation::TargetBadPriorities> find_suitable_priority_location_for(
    std::list<HookInfo>& hooks, HookMetadata const& hook_to_install) {
  using ResultT = Result<std::list<HookInfo>::iterator, installation::TargetBadPriorities>;
  // Install onto the target, respecting priorities.
  // Note that we may need to recompile some callbacks/fixups to change things
  // 1. Topological sort on our hooks that exist here by priority
  // - Find a suitable location where we can fit (note that we MAY need to recompile and move hooks around in order to
  // do this)
  // - First, walk all the hooks for a viable location, if we can find one. If we cannot, then we have to recompile
  // hooks.
  // TODO: Above
  // Also, if we have a final priority, we need to be the final hook, unless that hook is itself already marked as
  // final.
  if (hook_to_install.priority.is_final) {
    if (!hooks.empty() && hooks.back().metadata.priority.is_final) {
      // We cannot install here, we have a conflict
      return ResultT::Err(installation::TargetBadPriorities{
        hook_to_install, fmt::format("Cannot install a 'final' hook after another 'final' hook with name: {}", hooks.back().metadata.name_info) });
    }
    // Select the end to install at
    return ResultT::Ok(hooks.end());
  }
  // Otherwise, just install it at the front.
  return ResultT::Ok(hooks.begin());
}

Result<std::monostate, installation::TargetMismatch> validate_install_metadata(TargetMetadata& existing,
                                                                               HookMetadata const& incoming) {
  using ResultT = Result<std::monostate, installation::TargetMismatch>;
  // 1. Take the min of num_insts/verify they are equivalent
  existing.method_num_insts = std::min(existing.method_num_insts, incoming.method_num_insts);
  // 2. Validate calling convention matches
  if (existing.convention != incoming.convention) {
    return ResultT::ErrAt<installation::MismatchTargetConv>(incoming, existing.convention);
  }
  // 3. Validate midpoint matches
  if (existing.metadata.is_midpoint != incoming.installation_metadata.is_midpoint) {
    return ResultT::ErrAt<installation::MismatchMidpoint>(incoming, existing.metadata.is_midpoint);
  }
  // 4. Ensure parameter_info and return_info are matching (ifdef guarded)
#ifndef FLAMINGO_NO_REGISTRATION_CHECKS
  if (existing.return_info != incoming.return_info) {
    return ResultT::ErrAt<installation::MismatchReturn>(incoming, existing.return_info);
  }
  if (existing.parameter_info.size() != incoming.parameter_info.size()) {
    return ResultT::ErrAt<installation::MismatchParamCount>(incoming, existing.parameter_info.size());
  }
  for (size_t i = 0; i < existing.parameter_info.size(); i++) {
    if (existing.parameter_info[i] != incoming.parameter_info[i]) {
      return ResultT::ErrAt<installation::MismatchParam>(incoming, i, existing.parameter_info[i]);
    }
  }
#endif
  return ResultT::Ok();
}

}  // namespace

namespace flamingo {
installation::Result Install(HookInfo&& hook) {
#ifdef SC2_AOT_HOOKS
  AOTTransaction transaction;
  std::lock_guard guard(install_mutex);
  if (hook.metadata.installation_metadata.is_midpoint)
    return installation::Result::ErrAt<installation::TargetMismatch>(installation::MismatchMidpoint{hook.metadata,false});
  auto prepared_original=sc2_aot_current(hook.target);
  if (hook.target && !prepared_original) {
    FLAMINGO_DEBUG("Target {} has zero prepared instructions; prepare this exact entry before signing",fmt::ptr(hook.target));
    // Preserve the original error variant ABI: this target has no hookable
    // prepared instructions, regardless of its unmodified method extent.
    auto unavailable=hook.metadata; unavailable.method_num_insts=0;
    return installation::Result::ErrAt<installation::TargetTooSmall>(unavailable,1);
  }
#endif
  // Null targets to install to are prohibited, but null hook functions are allowed (and will most likely cause
  // horrible crashes when called)
  if (hook.target == nullptr) {
    return installation::Result::Err(installation::TargetIsNull{ hook.metadata.name_info });
  }
  TargetDescriptor target_info{ hook.target };
  auto hooked_target = targets.find(target_info);
  if (hooked_target == targets.end()) {
    // To make the first hook, we need to create the TargetData
    // For leapfrog hooks, we need to do something special anyways.
    // TODO: Support leapfrog hooks (where the installation space is fewer than 4U)
    // If we have an orig, we need to have an instruction to jump back to
#ifdef SC2_AOT_HOOKS
    auto const method_size = 1U;
#else
    auto const method_size = Fixups::kNormalFixupInstCount + (hook.orig_ptr != nullptr ? 1 : 0);
#endif
    if (hook.metadata.method_num_insts < method_size) {
      return installation::Result::ErrAt<installation::TargetTooSmall>(hook.metadata, method_size);
    }
    // The initial protection of the page that holds the target
    auto target_initial_protection = PageProtectionType::kExecute | PageProtectionType::kRead;
#ifndef SC2_AOT_HOOKS
    if (hook.metadata.installation_metadata.write_prot) {
      target_initial_protection |= PageProtectionType::kWrite;
    }
#endif
    auto target_pointer = PointerWrapper<uint32_t>(
        std::span<uint32_t>(reinterpret_cast<uint32_t*>(hook.target),
                            reinterpret_cast<uint32_t*>(hook.target) + hook.metadata.method_num_insts),
        target_initial_protection);
    auto result = targets.emplace(
        target_info, TargetData{ .metadata =
                                     TargetMetadata{
                                       .target = target_pointer,
                                       .convention = hook.metadata.convention,
                                       .metadata = hook.metadata.installation_metadata,
                                       .method_num_insts = hook.metadata.method_num_insts,
#ifndef FLAMINGO_NO_REGISTRATION_CHECKS
                                       .parameter_info = hook.metadata.parameter_info,
                                       .return_info = hook.metadata.return_info,
#endif
                                     },
                                 .fixups = Fixups{
                                   // Our fixup target is a subspan the same size as our install size
                                   .target = { target_pointer.Subspan(method_size) },
                                   .fixup_inst_destination =
#ifdef SC2_AOT_HOOKS
                                       PointerWrapper<uint32_t>{std::span<uint32_t>(static_cast<uint32_t*>(prepared_original),8),
                                                                 PageProtectionType::kExecute | PageProtectionType::kRead},
#else
                                       Allocate(kHookAlignment,
                                                std::min(Page::PageSize, hook.metadata.method_num_insts *
                                                                             sizeof(uint32_t) * kNumFixupsPerInst),
                                                PageProtectionType::kExecute | PageProtectionType::kRead),
#endif
                                 } });
    auto& target_data = result.first->second;
    hook.assign_orig(reinterpret_cast<void*>(&no_fixups));
    // Always copy over our original instructions to our .fixups instance
    target_data.fixups.CopyOriginalInsts();
    // If we want to make an orig, we fill it out now
    if (hook.metadata.installation_metadata.need_orig) {
      target_data.fixups.PerformFixupsAndCallback();
      hook.assign_orig(target_data.fixups.fixup_inst_destination.addr.data());
    }
    // Add the hook itself to the set of hooks we have, taking ownership
    auto const hook_data_result = target_data.hooks.emplace(target_data.hooks.end(), std::move(hook));
    // Now actually INSTALL the hook at target to point to the first hook in target_data.hooks
    target_data.fixups.target.WriteJump(hook_data_result->hook_ptr);
    return installation::Result::Ok(flamingo::installation::Ok{ HookHandle{ .hook_location = hook_data_result } });
  }
#ifdef SC2_AOT_HOOKS
  if (sc2_aot_current(hook.target)!=hooked_target->second.hooks.front().hook_ptr)
    return installation::Result::ErrAt<installation::TargetBadPriorities>(hook.metadata,"A later hook engine owns this entry; interleaved mutation is unsupported");
#endif
  auto installation_checks = validate_install_metadata(hooked_target->second.metadata, hook.metadata);
  if (!installation_checks.has_value()) {
    return installation::Result::ErrAt<installation::TargetMismatch>(installation_checks.error());
  }

  auto location_or_err = find_suitable_priority_location_for(hooked_target->second.hooks, hook.metadata);
  if (!location_or_err.has_value()) {
    return installation::Result::ErrAt<installation::TargetBadPriorities>(location_or_err.error());
  }
  auto const location = location_or_err.value();
  // 2. Assuming we found a reasonable location to install, insert our new hook before this location, and then adjust
  // those around us to match.
  auto const hook_data_result = hooked_target->second.hooks.emplace(location, std::move(hook));
#ifdef SC2_AOT_HOOKS
  // Initialize this hook's original before publishing a path that reaches it.
  if (std::next(hook_data_result)==hooked_target->second.hooks.end())
    hook_data_result->assign_orig(hooked_target->second.fixups.fixup_inst_destination.addr.data());
  else
    hook_data_result->assign_orig(std::next(hook_data_result)->hook_ptr);
#endif
  // - This is done by looking to the left and right of our target iterator to insert at:
  // -- If left does not exist: Rewrite the jump from the target to us; else rewrite the left's orig final jump to us
  if (hook_data_result == hooked_target->second.hooks.begin()) {
    hooked_target->second.fixups.target.WriteJump(hook_data_result->hook_ptr);
  } else {
    std::prev(hook_data_result)->assign_orig(hook_data_result->hook_ptr);
  }
  // -- If right does not exist: OUR orig calls the overall fixups; else jump to their hook_ptr
#ifndef SC2_AOT_HOOKS
  if (std::next(hook_data_result) == hooked_target->second.hooks.end()) {
    hook_data_result->assign_orig(hooked_target->second.fixups.fixup_inst_destination.addr.data());
  } else {
    hook_data_result->assign_orig(std::next(hook_data_result)->hook_ptr);
  }
#endif
  // TODO: Make assign_orig calls respect if we actually want an orig or not and add tests for this
  return installation::Result::Ok(flamingo::installation::Ok{ HookHandle{ .hook_location = hook_data_result } });
}

Result<bool, installation::Error> Reinstall(TargetDescriptor target) {
#ifdef SC2_AOT_HOOKS
  AOTTransaction transaction;
  std::lock_guard guard(install_mutex);
#endif
  using RetType = Result<bool, installation::Error>;
  auto itr = targets.find(target);
  if (itr == targets.end()) {
    return RetType::Ok(false);
  }
#ifdef SC2_AOT_HOOKS
  if (sc2_aot_current(target.target)!=itr->second.hooks.front().hook_ptr)
    return RetType::ErrAt<installation::TargetBadPriorities>(itr->second.hooks.front().metadata,"A later hook engine owns this entry; interleaved mutation is unsupported");
#endif
  // Reinstall the orig by calling PerformFixupsAndCallback() again (as needed)
  itr->second.fixups.CopyOriginalInsts();
  if (itr->second.metadata.metadata.need_orig) {
    itr->second.fixups.PerformFixupsAndCallback();
  }
  // Perform the write of the jump to the first hook
  itr->second.fixups.target.WriteJump(itr->second.hooks.begin()->hook_ptr);
  // Note that we do NOT reconstruct all of the inner hook pointers between each hook.
  // This is done as a partial optimization, but at some point we should revisit this (and adjust the docstring comment
  // to match)
  // TODO: Above
  return RetType::Ok(true);
}

Result<bool, bool> Uninstall(HookHandle handle) {
#ifdef SC2_AOT_HOOKS
  AOTTransaction transaction;
  std::lock_guard guard(install_mutex);
#endif
  using RetType = Result<bool, bool>;
  // Find the target entry. Note that this assumes the handle is not invalidated.
  auto target_entry = targets.find(TargetDescriptor(handle.hook_location->target));
  if (target_entry == targets.end()) {
    return RetType::Err(false);
  }
#ifdef SC2_AOT_HOOKS
  if (sc2_aot_current(handle.hook_location->target)!=target_entry->second.hooks.front().hook_ptr)
    return RetType::Err(true);
#endif
  // 1. If it is the only hook, destroys the fixups, uninstalls the hook by replacing the original instructions. Note
  // that this also destroys leapfrog hooks.
  if (target_entry->second.hooks.size() == 1) {
    target_entry->second.fixups.Uninstall();
    // At this point the original memory at our target is restored, we are safe to clear out the target entry here and
    // return
    // TODO: Invalidate leapfrog entries
    // TODO: Cleanup whatever dangling pointers we would have here (the fixup pointer being one of them)
    targets.erase(target_entry);
    return RetType::Ok(false);
  }
  // 2. If this is the first hook in a set of many, rewrites the target to jump to the hook past this one. Note that
  // this MAY also break leapfrog hooks, if this hook was installed as a branch but the next hook needs to be larger.
  if (handle.hook_location == target_entry->second.hooks.begin()) {
    target_entry->second.fixups.target.WriteJump(std::next(handle.hook_location)->hook_ptr);
  }
  // 3. If this is the last hook, makes the previous hook's orig point to the fixups directly, or to the no_fixups
  // function.
  else if (std::next(handle.hook_location) == target_entry->second.hooks.end()) {
    std::prev(handle.hook_location)
        ->assign_orig(target_entry->second.metadata.metadata.need_orig
                          ? target_entry->second.fixups.fixup_inst_destination.addr.data()
                          : reinterpret_cast<void*>(&no_fixups));
  }
  // 4. If this is a hook in the middle, the hook before us's orig will point to the next hook's hook function.
  else {
    std::prev(handle.hook_location)->assign_orig(std::next(handle.hook_location)->hook_ptr);
  }
  // After all that is done, the iterator is removed from the list of all hooks, and if empty, the entry from the
  // targets map is destroyed. Note that this invalidates all other held HookHandles to the SAME entry. Other entries
  // will not be invalidated.
  target_entry->second.hooks.erase(handle.hook_location);
  return RetType::Ok(true);
}

std::span<uint32_t> OriginalInstsFor(TargetDescriptor target) {
#ifdef SC2_AOT_HOOKS
  AOTTransaction transaction;
  std::lock_guard guard(install_mutex);
#endif
  auto itr = targets.find(target);
  if (itr != targets.end()) {
    return itr->second.fixups.original_instructions;
  }
  return {};
}

Result<TargetMetadata, std::monostate> MetadataFor(TargetDescriptor target) {
#ifdef SC2_AOT_HOOKS
  AOTTransaction transaction;
  std::lock_guard guard(install_mutex);
#endif
  auto itr = targets.find(target);
  if (itr != targets.end()) {
    return Result<TargetMetadata, std::monostate>::Ok(itr->second.metadata);
  }
  return Result<TargetMetadata, std::monostate>::Err();
}

Result<std::span<uint32_t const>, std::monostate> FixupPointerFor(TargetDescriptor target) {
#ifdef SC2_AOT_HOOKS
  AOTTransaction transaction;
  std::lock_guard guard(install_mutex);
#endif
  auto itr = targets.find(target);
  if (itr != targets.end()) {
    return Result<std::span<uint32_t const>, std::monostate>::Ok(itr->second.fixups.fixup_inst_destination.addr);
  }
  return Result<std::span<uint32_t const>, std::monostate>::Err();
}

}  // namespace flamingo
