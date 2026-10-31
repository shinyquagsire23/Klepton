#include "capi.h"
#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>
#include "calling-convention.hpp"
#include "hook-data.hpp"
#include "hook-installation-result.hpp"
#include "hook-metadata.hpp"
#include "installer.hpp"
#include "target-data.hpp"
#include "type-info.hpp"
#include "util.hpp"

namespace {

flamingo::CallingConvention convert_calling_conv(FlamingoCallingConvention conv) {
  switch (conv) {
    case FlamingoCallingConvention::FLAMINGO_CDECL:
      return flamingo::CallingConvention::Cdecl;
    case FLAMINGO_FASTCALL:
      return flamingo::CallingConvention::Fastcall;
    case FLAMINGO_THISCALL:
      return flamingo::CallingConvention::Thiscall;
  }
}

FlamingoInstallationType type_from_mismatch(flamingo::installation::TargetMismatch const& mismatch) {
  using namespace flamingo::installation;
  return std::visit(flamingo::util::overload{
                      [](MismatchTargetConv const&) { return FLAMINGO_INSTALL_MISMATCH_CALLING_CONVENTION; },
                      [](MismatchMidpoint const&) { return FLAMINGO_INSTALL_MISMATCH_MIDPOINT; },
#ifndef FLAMINGO_NO_REGISTRATION_CHECKS
                      [](MismatchReturn const&) { return FLAMINGO_INSTALL_MISMATCH_RETURN; },
                      [](MismatchParam const&) { return FLAMINGO_INSTALL_MISMATCH_PARAM; },
                      [](MismatchParamCount const&) { return FLAMINGO_INSTALL_MISMATCH_PARAM_COUNT; },
#endif
                    },
                    mismatch);
}

FlamingoInstallErrorData* make_error_data(flamingo::installation::Error const& error) {
  return reinterpret_cast<FlamingoInstallErrorData*>(new flamingo::installation::Error{ error });
}

FlamingoInstallationResult convert_install_result(flamingo::installation::Result const& result) {
  using namespace flamingo::installation;
  if (result.has_value()) {
    return FlamingoInstallationResult{
      .result = FLAMINGO_INSTALL_OK,
      .value = { .handle =
                     reinterpret_cast<FlamingoHookHandle*>(new flamingo::HookHandle(result.value().returned_handle)) },
    };
  }
  auto const& error = result.error();
  // Convert the error to a FlamingoInstallationType
  auto result_type =
      std::visit(flamingo::util::overload{
                   [](TargetIsNull const&) { return FLAMINGO_INSTALL_TARGET_NULL; },
                   [](TargetBadPriorities const&) { return FLAMINGO_INSTALL_BAD_PRIORITIES; },
                   [](TargetMismatch const& mismatch) { return type_from_mismatch(mismatch); },
                   [](TargetTooSmall const&) { return FLAMINGO_INSTALL_TOO_SMALL; },
                 },
                 error);
  return FlamingoInstallationResult{
    .result = result_type,
    .value = { .data = make_error_data(error) },
  };
}

FlamingoReinstallResult convert_reinstall_result(flamingo::Result<bool, flamingo::installation::Error> const& result) {
  if (result.has_value()) {
    return FlamingoReinstallResult{
      .success = true,
      .value = {.any_hooks_reinstalled = result.value()},
    };
  }
  return FlamingoReinstallResult{
    .success = result.has_value(),
    .value {.data = make_error_data(result.error())},
  };
}

FlamingoUninstallResult convert_uninstall_result(flamingo::Result<bool, bool> const& result) {
  if (result.has_value()) {
    return FlamingoUninstallResult{
      .success = true,
      .value = {.any_hooks_remain = result.value()},
    };
  }
  return FlamingoUninstallResult{
    .success = result.has_value(),
    .value {.remap_failure = result.error()},
  };
}
}  // namespace

FLAMINGO_C_EXPORT FlamingoNameInfo* flamingo_make_name(char const* name_str) {
  return reinterpret_cast<FlamingoNameInfo*>(new flamingo::HookNameMetadata{ .name = name_str });
}

FLAMINGO_C_EXPORT FlamingoHookPriority* flamingo_make_priority(FlamingoNameInfo** before_names, size_t num_befores,
                                                               FlamingoNameInfo** after_names, size_t num_afters, bool is_final) {
  // Iterate the befores and afters, consume their pointers to make new instances for the before set
  auto result = new flamingo::HookPriority();
  result->befores.resize(num_befores);
  for (size_t i = 0; i < num_befores; i++) {
    auto value = reinterpret_cast<flamingo::HookNameMetadata*>(before_names[i]);
    new (&result->befores[i]) flamingo::HookNameMetadata(*value);
    delete value;
  }
  result->afters.resize(num_afters);
  for (size_t i = 0; i < num_afters; i++) {
    auto value = reinterpret_cast<flamingo::HookNameMetadata*>(after_names[i]);
    new (&result->afters[i]) flamingo::HookNameMetadata(*value);
    delete value;
  }
  result->is_final = is_final;
  return reinterpret_cast<FlamingoHookPriority*>(result);
}

FLAMINGO_C_EXPORT FlamingoInstallationMetadata* flamingo_make_install_metadata(bool make_fixups, bool is_midpoint,
                                                                               bool write_prot) {
  return reinterpret_cast<FlamingoInstallationMetadata*>(new flamingo::InstallationMetadata{
    // This value is overriden by the install itself
    .need_orig = make_fixups,
    .is_midpoint = is_midpoint,
    .write_prot = write_prot,
  });
}

FLAMINGO_C_EXPORT FlamingoTypeInfo* flamingo_make_type_info(char const* name, size_t size) {
  // TODO: Implement usage of name
  static_cast<void>(name);
  return reinterpret_cast<FlamingoTypeInfo*>(new flamingo::TypeInfo{ .size = size });
}

FLAMINGO_C_EXPORT FlamingoOriginalInstructionsResult flamingo_orig_for(uint32_t const* addr) {
  auto result = flamingo::OriginalInstsFor(flamingo::TargetDescriptor{ const_cast<uint32_t*>(addr) });
  return FlamingoOriginalInstructionsResult{
    .hook_size = result.size(),
    .original_instructions = result.empty() ? addr : result.data(),
  };
}

FLAMINGO_C_EXPORT FlamingoInstallationResult
flamingo_install_hook_full(void* hook_function, uint32_t* target, void** orig_pointer, uint16_t num_insts,
                           FlamingoCallingConvention convention, FlamingoNameInfo* name_info,
                           FlamingoHookPriority* priority, FlamingoInstallationMetadata* install_metadata) {
  auto flamingo_name_info = reinterpret_cast<flamingo::HookNameMetadata*>(name_info);
  auto flamingo_priority = reinterpret_cast<flamingo::HookPriority*>(priority);
  auto flamingo_install_metadata = reinterpret_cast<flamingo::InstallationMetadata*>(install_metadata);
  auto result = flamingo::Install(flamingo::HookInfo(
      hook_function, target, orig_pointer, num_insts, convert_calling_conv(convention), std::move(*flamingo_name_info),
      std::move(*flamingo_priority), std::move(*flamingo_install_metadata)));
  // Delete the otherwise dangling pointers
  delete flamingo_name_info;
  delete flamingo_priority;
  delete flamingo_install_metadata;
  return convert_install_result(result);
}

FLAMINGO_C_EXPORT FlamingoInstallationResult flamingo_install_hook(void* hook_function, uint32_t* target,
                                                                   void** orig_pointer, FlamingoNameInfo* name_info) {
  auto flamingo_name_info = reinterpret_cast<flamingo::HookNameMetadata*>(name_info);
  auto result =
      flamingo::Install(flamingo::HookInfo(hook_function, target, orig_pointer, std::move(*flamingo_name_info)));
  // Delete the otherwise dangling pointers
  delete flamingo_name_info;
  return convert_install_result(result);
}

FLAMINGO_C_EXPORT FlamingoInstallationResult flamingo_install_hook_no_name(void* hook_function, uint32_t* target,
                                                                           void** orig_pointer) {
  auto result = flamingo::Install(flamingo::HookInfo(hook_function, target, orig_pointer));
  return convert_install_result(result);
}

#ifndef FLAMINGO_NO_REGISTRATION_CHECKS

FLAMINGO_C_EXPORT FlamingoInstallationResult flamingo_install_hook_full_checked(
    void* hook_function, uint32_t* target, void** orig_pointer, uint16_t num_insts,
    FlamingoCallingConvention convention, FlamingoNameInfo* name_info, FlamingoHookPriority* priority,
    FlamingoInstallationMetadata* install_metadata, FlamingoTypeInfo* return_info, FlamingoTypeInfo** parameter_info,
    size_t num_params) {
  auto flamingo_name_info = reinterpret_cast<flamingo::HookNameMetadata*>(name_info);
  auto flamingo_priority = reinterpret_cast<flamingo::HookPriority*>(priority);
  auto flamingo_install_metadata = reinterpret_cast<flamingo::InstallationMetadata*>(install_metadata);
  auto flamingo_return_info = reinterpret_cast<flamingo::TypeInfo*>(return_info);
  std::vector<flamingo::TypeInfo> flamingo_parameter_info{};
  flamingo_parameter_info.resize(num_params);
  for (size_t i = 0; i < num_params; i++) {
    auto p = reinterpret_cast<flamingo::TypeInfo*>(parameter_info[i]);
    new (&flamingo_parameter_info[i]) flamingo::TypeInfo(*p);
    delete p;
  }
  auto result = flamingo::Install(
      flamingo::HookInfo(hook_function, target, orig_pointer, num_insts, convert_calling_conv(convention),
                         std::move(*flamingo_name_info), std::move(*flamingo_priority), *flamingo_install_metadata,
                         std::move(flamingo_parameter_info), std::move(*flamingo_return_info)));
  // Delete the otherwise dangling pointers
  delete flamingo_name_info;
  delete flamingo_priority;
  delete flamingo_install_metadata;
  delete flamingo_return_info;
  return convert_install_result(result);
}

FLAMINGO_C_EXPORT FlamingoInstallationResult
flamingo_install_hook_checked(void* hook_function, uint32_t* target, void** orig_pointer, FlamingoNameInfo* name_info,
                              FlamingoTypeInfo* return_info, FlamingoTypeInfo** parameter_info, size_t num_params) {
  auto flamingo_name_info = reinterpret_cast<flamingo::HookNameMetadata*>(name_info);
  auto flamingo_return_info = reinterpret_cast<flamingo::TypeInfo*>(return_info);
  std::vector<flamingo::TypeInfo> flamingo_parameter_info{};
  flamingo_parameter_info.resize(num_params);
  for (size_t i = 0; i < num_params; i++) {
    auto p = reinterpret_cast<flamingo::TypeInfo*>(parameter_info[i]);
    new (&flamingo_parameter_info[i]) flamingo::TypeInfo(*p);
    delete p;
  }
  auto result =
      flamingo::Install(flamingo::HookInfo(hook_function, target, orig_pointer, std::move(*flamingo_name_info),
                                           std::move(flamingo_parameter_info), std::move(*flamingo_return_info)));
  // Delete the otherwise dangling pointers
  delete flamingo_name_info;
  delete flamingo_return_info;
  return convert_install_result(result);
}
#endif

FLAMINGO_C_EXPORT FlamingoReinstallResult flamingo_reinstall_hook(uint32_t* target) {
  return convert_reinstall_result(flamingo::Reinstall(flamingo::TargetDescriptor{ .target = target }));
}

FLAMINGO_C_EXPORT FlamingoUninstallResult flamingo_uninstall_hook(FlamingoHookHandle* handle) {
  auto value = reinterpret_cast<flamingo::installation::Ok*>(handle);
  auto result = flamingo::Uninstall(value->returned_handle);
  delete value;
  return convert_uninstall_result(result);
}

FLAMINGO_C_EXPORT_VOID void flamingo_format_error(FlamingoInstallErrorData* error, char* buffer, size_t buffer_size) {
  auto install_error = reinterpret_cast<flamingo::installation::Error*>(error);
  auto [out, _] = fmt::format_to_n(buffer, buffer_size - 1, "{}", *install_error);
  *out = '\0';  // Suffix with a null
  delete install_error;
}
