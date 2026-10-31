#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Most flamingo API calls also require the result to be used in some way.
#define FLAMINGO_C_EXPORT __attribute__((visibility("default"))) __attribute__((warn_unused_result))
#define FLAMINGO_C_EXPORT_VOID __attribute__((visibility("default")))
// The flamingo C API

#ifdef __cplusplus
extern "C" {
#endif

/// @brief The installation result types
typedef enum {
  FLAMINGO_INSTALL_OK,
  FLAMINGO_INSTALL_TARGET_NULL,
  FLAMINGO_INSTALL_BAD_PRIORITIES,
  FLAMINGO_INSTALL_MISMATCH_CALLING_CONVENTION,
  FLAMINGO_INSTALL_MISMATCH_MIDPOINT,
  FLAMINGO_INSTALL_TOO_SMALL,
#ifndef FLAMINGO_NO_REGISTRATION_CHECKS
  FLAMINGO_INSTALL_MISMATCH_RETURN,
  FLAMINGO_INSTALL_MISMATCH_PARAM,
  FLAMINGO_INSTALL_MISMATCH_PARAM_COUNT,
#endif
} FlamingoInstallationType;

/// @brief Opaque pointer around a flamingo::HookHandle
typedef struct FlamingoHookHandle FlamingoHookHandle;

/// @brief Opaque pointer around a flamingo::installation::Error
typedef struct FlamingoInstallErrorData FlamingoInstallErrorData;

/// @brief Returned from an installation.
/// The handle of the union is legal only when result == FLAMINGO_INSTALL_OK, otherwise it holds an error type.
/// The error type can be formatted to a string using flamingo_format_error. Getting the actual failure info is
/// currently unsupported. The lifetime of the handle is until flamingo_uninstall_hook is called. The lifetime of the
/// error data is until flamingo_format_error is called.
typedef struct {
  FlamingoInstallationType result;
  union {
    FlamingoHookHandle* handle;
    FlamingoInstallErrorData* data;
  } value;
} FlamingoInstallationResult;

/// @brief Returned from a reinstallation. If success is false, the value of any_hooks_reinstalled is undefined.
/// If success is true, the union describes if any hooks were present at the specified target for a reinstall.
/// If success is false, the union describes the error that occurred during the reinstall. The lifetime of the error
/// data is until flamingo_format_error is called.
typedef struct {
  bool success;
  union {
    bool any_hooks_reinstalled;
    FlamingoInstallErrorData* data;
  } value;
} FlamingoReinstallResult;

/// @brief Returned from an uninstall. If success is false, the value of any_hooks_remain is undefined.
/// If success is true, the union describes if any hooks remain after the uninstallation at this target.
/// If success is false, the union describes the error that occurred during the uninstall. A false value indicates that
/// no hook was found with the provided handle. A true value indicates that a remapping failure occurred.
typedef struct {
  bool success;
  union {
    bool any_hooks_remain;
    bool remap_failure;
  } value;
} FlamingoUninstallResult;

/// @brief The calling conventions of the target used for hook validation.
/// It is assumed that the hook function you provide has the same calling convention.
typedef enum {
  FLAMINGO_CDECL,
  FLAMINGO_FASTCALL,
  FLAMINGO_THISCALL,
} FlamingoCallingConvention;

/// @brief Opaque pointer around a flamingo::HookNameMetadata
typedef struct FlamingoNameInfo FlamingoNameInfo;

/// @brief Opaque pointer around a flamingo::HookPriority
typedef struct FlamingoHookPriority FlamingoHookPriority;

/// @brief Opaque pointer around a flamingo::InstallationMetadata
typedef struct FlamingoInstallationMetadata FlamingoInstallationMetadata;

/// @brief Opaque pointer around a flamingo::TypeInfo
typedef struct FlamingoTypeInfo FlamingoTypeInfo;

/// @brief Returned from a call to query if a region is hooked, and what the original instructions at that location are.
/// Should not be stored for long-term use, since the lifetime of the result is tied to the lifetime of the hooks at
/// this location.
typedef struct {
  /// @brief The size of the hook present at this address, in number of instructions.
  size_t hook_size;
  /// @brief A non-owning pointer to the hook's original instructions.
  /// Safe to dereference up to 'hook_size' for as long as there is at least one hook at this location that is not
  /// uninstalled, and no reinstall takes place. If no hook is present at this target, this pointer will exactly equal
  /// the addr pointer and size will be 0.
  uint32_t const* original_instructions;
} FlamingoOriginalInstructionsResult;

/// @brief Creates a flamingo::HookNameMetadata from the provided parameters. The return is an opaque pointer.
/// The returned pointer's lifetime is until a different flamingo API call is made that CONSUMES the FlamingoNameInfo*.
/// This is primarily used to give hooks names and to describe priorities for installation.
/// The lifetime of the result is until it is consumed by a call to flamingo_install_hook*, or flamingo_make_priority.
FLAMINGO_C_EXPORT FlamingoNameInfo* flamingo_make_name(char const* name_str);

/// @brief Creates a flamingo::HookMetadata from the provided parameters.
/// The parameters are arrays of FlamingoNameInfo that must be dereferencable up to num_befores and num_afters
/// respectively. The parameters are CONSUMED, that is, the pointers are no longer valid after this API call. This is
/// used to give hooks priority information in flamingo_install_hook_full*
/// @param is_final Whether this hook should be the final hook (closest to the original function). This takes precedence
/// over all other priorities.
/// The lifetime of the result is until it is consumed by a call to flamingo_install_hook*.
FLAMINGO_C_EXPORT FlamingoHookPriority* flamingo_make_priority(FlamingoNameInfo** before_names, size_t num_befores,
                                                               FlamingoNameInfo** after_names, size_t num_afters,
                                                               bool is_final);

/// @brief Creates a flamingo::InstallationMetadata from the provided parameters.
/// This is used to describe metadata that should hint to flamingo to install correctly.
/// Note that these are HINTS and are not strictly required for flamingo to follow, though in practice it will. This
/// will be changed to strong guarantees in a future version of flamingo.
/// @param make_fixups Whether fixups should be generated for this hook. If false, orig cannot be called safely.
/// @param is_midpoint Whether this hook is in the middle of a function call instead of at the beginning. Note that this
/// will usually mean a different scratch register should be used, and that the branching logic may be incorrect.
/// @param write_prot Whether to also mark the page where the target is as writable.
/// The lifetime of the result is until it is consumed by a call to flamingo_install_hook*.
FLAMINGO_C_EXPORT FlamingoInstallationMetadata* flamingo_make_install_metadata(bool make_fixups, bool is_midpoint,
                                                                               bool write_prot);

/// @brief Creates a flamingo::TypeInfo from the provided parameters.
/// This is used for type checking hook installs to ensure multiple installs over the same target agree upon the
/// parameters provided. For void types, it is expected to provide a type size of 0.
/// The lifetime of the result is until it is consumed by a call to flamingo_install_hook*.
FLAMINGO_C_EXPORT FlamingoTypeInfo* flamingo_make_type_info(char const* name, size_t size);

/// @brief Returns a FlamingoOriginalInstructionsResult.
/// The hook_size of the returned instance will be 0 if the provided address is not the START of an installed hook.
/// The returned original_instructions pointer is safe to read in the range [0..hook_size).
/// This function is commonly used for ensuring correct results when going through an xref trace, or validating real
/// instructions.
FLAMINGO_C_EXPORT FlamingoOriginalInstructionsResult flamingo_orig_for(uint32_t const* addr);

/// @brief Install a hook at the provided target to call the provided hook function, with an optionally non-null
/// orig_pointer that will be assigned to the fixups region, and a name. Returns the installation result to be used in
/// uninstalls or for errors.
/// @param hook_function The function to call from the hook. If this is null, will branch to 0.
/// @param target The target to install the hook to.
/// @param orig_pointer A pointer to a function to populate after the install with fixups to call for a trampoline. If
/// null, no fixups will be generated.
/// @param num_insts The number of instructions of the target function that are safe to mutate.
/// @param convention The calling convention of the target function. The hook_function must match the calling
/// convention.
/// @param name_info The name of the hook, made through flamingo_make_name.
/// @param priority The priorities to respect for the hook, made through flamingo_make_priority.
/// @param install_metadata Extra installation metadata to specifiy.
/// The lifetime of the result is until it is consumed by a call to flamingo_uninstall_hook or flamingo_format_error,
/// depending on the type of the result.
FLAMINGO_C_EXPORT FlamingoInstallationResult flamingo_install_hook_full(void* hook_function, uint32_t* target,
                                                                        void** orig_pointer, uint16_t num_insts,
                                                                        FlamingoCallingConvention convention,
                                                                        FlamingoNameInfo* name_info,
                                                                        FlamingoHookPriority* priority,
                                                                        FlamingoInstallationMetadata* install_metadata);

/// @brief Exactly the same as flamingo_install_hook_full, except: The number of instructions is 10, calling convention
/// is Cdecl, and there are no flags set in installation_metadata.
/// @param hook_function The function to call from the hook. If this is null, will branch to 0.
/// @param target The target to install the hook to.
/// @param orig_pointer A pointer to a function to populate after the install with fixups to call for a trampoline. If
/// null, no fixups will be generated.
/// @param name_info The name of the hook, made through flamingo_make_name.
/// The lifetime of the result is until it is consumed by a call to flamingo_uninstall_hook or flamingo_format_error,
/// depending on the type of the result.
FLAMINGO_C_EXPORT FlamingoInstallationResult flamingo_install_hook(void* hook_function, uint32_t* target,
                                                                   void** orig_pointer, FlamingoNameInfo* name_info);

/// @brief Exactly the same as flamingo_install_hook_full, except: The number of instructions is 10, calling convention
/// is Cdecl, there are no flags set in installation_metadata, and there is no name for the hook (the empty string will
/// be used instead).
/// @param hook_function The function to call from the hook. If this is null, will branch to 0.
/// @param target The target to install the hook to.
/// @param orig_pointer A pointer to a function to populate after the install with fixups to call for a trampoline. If
/// null, no fixups will be generated.
/// The lifetime of the result is until it is consumed by a call to flamingo_uninstall_hook or flamingo_format_error,
/// depending on the type of the result.
FLAMINGO_C_EXPORT FlamingoInstallationResult flamingo_install_hook_no_name(void* hook_function, uint32_t* target,
                                                                           void** orig_pointer);

#ifndef FLAMINGO_NO_REGISTRATION_CHECKS
/// @brief Install a hook at the provided target to call the provided hook function, with an optionally non-null
/// orig_pointer that will be assigned to the fixups region, and a name. Returns the installation result to be used in
/// uninstalls or for errors.
/// Additionally checks the provided return_info and parameter_info, made from calls to flamingo_make_type_info.
/// @param hook_function The function to call from the hook. If this is null, will branch to 0.
/// @param target The target to install the hook to.
/// @param orig_pointer A pointer to a function to populate after the install with fixups to call for a trampoline. If
/// null, no fixups will be generated.
/// @param num_insts The number of instructions of the target function that are safe to mutate.
/// @param convention The calling convention of the target function. The hook_function must match the calling
/// convention.
/// @param name_info The name of the hook, made through flamingo_make_name.
/// @param priority The priorities to respect for the hook, made through flamingo_make_priority.
/// @param install_metadata Extra installation metadata to specifiy.
/// @param return_info The return type info to check, made through flamingo_make_type_info.
/// @param parameter_info An array of type infos to check, made through flamingo_make_type_info.
/// @param num_params The length of the parameter_info array.
/// The lifetime of the result is until it is consumed by a call to flamingo_uninstall_hook or flamingo_format_error,
/// depending on the type of the result.
FLAMINGO_C_EXPORT FlamingoInstallationResult
flamingo_install_hook_full_checked(void* hook_function, uint32_t* target, void** orig_pointer, uint16_t num_insts,
                                   FlamingoCallingConvention convention, FlamingoNameInfo* name_info,
                                   FlamingoHookPriority* priority, FlamingoInstallationMetadata* install_metadata,
                                   FlamingoTypeInfo* return_info, FlamingoTypeInfo** parameter_info, size_t num_params);

/// @brief Exactly the same as flamingo_install_hook_full_checked, except: The number of instructions is 10, calling
/// convention is Cdecl, and there are no flags set in installation_metadata. Additionally checks the provided
/// return_info and parameter_info, made from calls to flamingo_make_type_info.
/// @param hook_function The function to call from the hook. If this is null, will branch to 0.
/// @param target The target to install the hook to.
/// @param orig_pointer A pointer to a function to populate after the install with fixups to call for a trampoline. If
/// null, no fixups will be generated.
/// @param name_info The name of the hook, made through flamingo_make_name.
/// @param return_info The return type info to check, made through flamingo_make_type_info.
/// @param parameter_info An array of type infos to check, made through flamingo_make_type_info.
/// @param num_params The length of the parameter_info array.
/// The lifetime of the result is until it is consumed by a call to flamingo_uninstall_hook or flamingo_format_error,
/// depending on the type of the result.
FLAMINGO_C_EXPORT FlamingoInstallationResult
flamingo_install_hook_checked(void* hook_function, uint32_t* target, void** orig_pointer, FlamingoNameInfo* name_info,
                              FlamingoTypeInfo* return_info, FlamingoTypeInfo** parameter_info, size_t num_params);
#endif

/// @brief Reinstall the top hook onto the target again. Used if the original function changed for any reason (ex, it
/// was re-JIT'd). If no hooks are present on the target, returns success: true, any_hooks_reinstalled: false.
FLAMINGO_C_EXPORT FlamingoReinstallResult flamingo_reinstall_hook(uint32_t* target);

/// @brief Given a handle to a successfully installed hook, uninstalls this hook at that location, returning if it
/// succeeded and if there are other hooks left at that target. After this call, the provided FlamingoHookHandle is
/// invalid.
FLAMINGO_C_EXPORT FlamingoUninstallResult flamingo_uninstall_hook(FlamingoHookHandle* handle);

/// @brief Given an installation error, formats a human-readable error message and writes it to the provided string, not
/// exceeding the size provided.
FLAMINGO_C_EXPORT_VOID void flamingo_format_error(FlamingoInstallErrorData* error, char* buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif
