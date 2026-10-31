#pragma once

#include <cstdint>
#include <span>
#include <variant>
#include "hook-data.hpp"
#include "hook-installation-result.hpp"
#include "target-data.hpp"
#include "util.hpp"

namespace flamingo {

constexpr static auto kHookAlignment = 16U;
constexpr static auto kNumFixupsPerInst = 4U;

/// @brief To install a hook, we require a constructed HookInfo. We want to hold exclusive ownership, so we require an
/// rvalue (we may also forward params?). Because a HookInfo is just data, we go find our TargetInfo that matches our
/// target. Then, we attempt to install that HookInfo onto the TargetData, mutating the TargetData (but not invalidating
/// other HookInfo references within the list). We update the shared information within the HookInfo and perform the
/// install as necessary. Priorities use named IDs for cleaer ordering (before x, after y). This may require a full
/// reassmebly of the list!
[[nodiscard]] FLAMINGO_EXPORT installation::Result Install(HookInfo&& hook);

/// @brief Called on a target to reinstall all targets present at that location.
/// A reinstall is done by re-performing orig fixups at the target, and rewriting a jump to the first hook.
/// All other hooks remain unchanged.
/// This function returns Ok(true) if all hooks were reinstalled correctly, Ok(false) if there were no hooks to
/// reinstall, and Error(...) otherwise.
[[nodiscard]] FLAMINGO_EXPORT Result<bool, installation::Error> Reinstall(TargetDescriptor target);

/// @brief Called on an installed hook to uninstall it from the set of all hooks.
/// Note that uninstalling a hook never requires a recompile of the set, as no priorities are altered.
/// Functionally, this behaves as follows:
/// 1. If it is the only hook, destroys the fixups, uninstalls the hook by replacing the original instructions. Note
/// that this also destroys leapfrog hooks.
/// 2. If this is the first hook in a set of many, rewrites the target to jump to the hook past this one. Note that this
/// MAY also break leapfrog hooks, if this hook was installed as a branch but the next hook needs to be larger.
/// 3. If this is the last hook, makes the previous hook's orig point to the fixups directly, or to the no_fixups
/// function.
/// 4. If this is a hook in the middle, the hook before us's orig will point to the next hook's hook function.
/// After all that is done, the iterator is removed from the list of all hooks, and if empty, the entry from the targets
/// map is destroyed. Note that this invalidates all other held HookHandles to the SAME entry. Other entries will not be
/// invalidated.
/// @returns Ok(true) if the target remains, Ok(false) if the full target was removed from the map, Error(false) if no
/// target was found from this handle, Error(true) if a remapping failure happened.
[[nodiscard]] FLAMINGO_EXPORT Result<bool, bool> Uninstall(HookHandle handle);

/// @brief Returns the original instructions for a specified target, if it is the start of a known hook.
/// If the target is not hooked, returns an empty span.
std::span<uint32_t> FLAMINGO_EXPORT OriginalInstsFor(TargetDescriptor target);

/// @brief Returns the TargetMetadata for a provided TargetDescriptor.
/// If the target is not hooked, returns an error Result.
[[nodiscard]] FLAMINGO_EXPORT Result<TargetMetadata, std::monostate> MetadataFor(TargetDescriptor target);

/// @brief Returns the Fixups span for a provided TargetDescriptor.
/// If the target is not hooked, returns an error Result.
[[nodiscard]] FLAMINGO_EXPORT Result<std::span<uint32_t const>, std::monostate> FixupPointerFor(
    TargetDescriptor target);

}  // namespace flamingo
