#pragma once
#include <fmt/compile.h>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "calling-convention.hpp"
#include "hook-metadata.hpp"
#include "target-data.hpp"
#include "type-info.hpp"
#include "util.hpp"

namespace flamingo {

template <class T>
struct is_variant {
  constexpr static bool value = false;
};

template <class... TArgs>
struct is_variant<std::variant<TArgs...>> {
  constexpr static bool value = true;
};

template <class T, class E>
struct Result {
  std::variant<E, T> data;
  template <class... TArgs>
  static Result Ok(TArgs&&... args) {
    return Result{ std::variant<E, T>(std::in_place_index_t<1>{}, std::forward<TArgs>(args)...) };
  }
  template <class... TArgs>
  static Result Err(TArgs&&... args) {
    return Result{ std::variant<E, T>(std::in_place_index_t<0>{}, std::forward<TArgs>(args)...) };
  }
  // Helper function for if E is a variant (we have multiple errors and need to construct one)
  template <class ET, class... TArgs>
    requires(is_variant<E>::value)
  static Result ErrAt(TArgs&&... args) {
    return Result{ std::variant<E, T>(std::in_place_index_t<0>{},
                                      E(std::in_place_type_t<ET>{}, std::forward<TArgs>(args)...)) };
  }
  T const& value() const {
    return std::get<1>(data);
  }
  E const& error() const {
    return std::get<0>(data);
  }
  bool has_value() const {
    return data.index() == 1;
  }
};

namespace installation {

/// @brief Holds metadata about the successful install
struct Ok {
  HookHandle returned_handle;
};

/// @brief The general base type for reporting hook errors. Holds the ID of the failing hook.
struct HookErrorInfo {
  HookNameMetadata installing_hook;
  HookErrorInfo(HookNameMetadata const& m) : installing_hook(m) {}
};

/// @brief An error when the target of a hook install is null.
struct TargetIsNull : HookErrorInfo {
  TargetIsNull(HookNameMetadata const& m) : HookErrorInfo(m) {}
};
/// @brief An error when the target method is described as too small for the hook strategy being employed.
struct TargetTooSmall : HookErrorInfo {
  TargetTooSmall(HookMetadata const& m, uint_fast16_t needed)
      : HookErrorInfo(m.name_info), actual_num_insts(m.method_num_insts), needed_num_insts(needed) {}
  uint_fast16_t actual_num_insts;
  uint_fast16_t needed_num_insts;
};
/// @brief An error when the target method is impossible to install given its priorities and other hooks to install it
/// onto.
struct TargetBadPriorities : HookErrorInfo {
  // TODO: Add a bunch of stuff here
  TargetBadPriorities(HookMetadata const& m, std::string_view message) : HookErrorInfo(m.name_info), message(message) {}
  std::string message;
};
// TODO: Should we add the incoming hook IDs?

#ifndef FLAMINGO_NO_REGISTRATION_CHECKS
struct MismatchReturn : HookErrorInfo {
  MismatchReturn(HookMetadata const& m, TypeInfo existing)
      : HookErrorInfo(m.name_info), existing(existing), incoming(m.return_info) {}
  TypeInfo existing;
  TypeInfo incoming;
};

struct MismatchParam : HookErrorInfo {
  MismatchParam(HookMetadata const& m, size_t idx, TypeInfo existing)
      : HookErrorInfo(m.name_info), idx(idx), existing(existing), incoming(m.parameter_info[idx]) {}
  size_t idx{};
  TypeInfo existing{};
  TypeInfo incoming{};
};

struct MismatchParamCount : HookErrorInfo {
  MismatchParamCount(HookMetadata const& m, size_t existing)
      : HookErrorInfo(m.name_info), existing(existing), incoming(m.parameter_info.size()) {}
  size_t existing;
  size_t incoming;
};

#endif

struct MismatchTargetConv : HookErrorInfo {
  MismatchTargetConv(HookMetadata const& m, CallingConvention existing)
      : HookErrorInfo(m.name_info), existing(existing), incoming(m.convention) {}
  CallingConvention existing{};
  CallingConvention incoming{};
};

struct MismatchMidpoint : HookErrorInfo {
  MismatchMidpoint(HookMetadata const& m, bool existing)
      : HookErrorInfo(m.name_info), existing(existing), incoming(m.installation_metadata.is_midpoint) {}
  bool existing{};
  bool incoming{};
};

#ifndef FLAMINGO_NO_REGISTRATION_CHECKS
/// @brief An error when the target method has some validation failure with respect to the metadata it holds.
using TargetMismatch =
    std::variant<MismatchTargetConv, MismatchMidpoint, MismatchReturn, MismatchParam, MismatchParamCount>;
#else
/// @brief An error when the target method has some validation failure with respect to the metadata it holds.
using TargetMismatch = std::variant<MismatchTargetConv, MismatchMidpoint>;
#endif

// Can be one of many cases.
using Error = std::variant<TargetIsNull, TargetBadPriorities, TargetMismatch, TargetTooSmall>;

using Result = flamingo::Result<Ok, Error>;

}  // namespace installation

}  // namespace flamingo

// Custom formatter for flamingo::Error
template <>
class fmt::formatter<flamingo::installation::Error> {
 public:
  constexpr static auto parse(format_parse_context& ctx) {
    return ctx.begin();
  }
  template <typename Context>
  constexpr static auto format(flamingo::installation::Error const& error, Context& ctx) {
    using namespace flamingo::installation;
    return std::visit(
        flamingo::util::overload{
          [&ctx](TargetIsNull const& null_target) {
            return fmt::format_to(ctx.out(), "Null target, for hook: {}", null_target.installing_hook);
          },
          [&ctx](TargetBadPriorities const& bad_priorities) {
            return fmt::format_to(ctx.out(), "Bad priorities, for hook: {}, with message: {}",
                             bad_priorities.installing_hook, bad_priorities.message);
          },
          [&ctx](TargetMismatch const& mismatch) { return fmt::format_to(ctx.out(), "Target mismatch: {}", mismatch); },
          [&ctx](TargetTooSmall const& small_target) {
            return fmt::format_to(
                ctx.out(), "Target too small, needed: {} instructions, but have: {} instructions for hook: {}",
                small_target.needed_num_insts, small_target.actual_num_insts, small_target.installing_hook);
          } },
        error);
  }
};

// Custom formatter for flamingo::installation::TargetMismatch
template <>
class fmt::formatter<flamingo::installation::TargetMismatch> {
 public:
  constexpr auto parse(format_parse_context& ctx) {
    return ctx.begin();
  }
  template <typename Context>
  constexpr auto format(flamingo::installation::TargetMismatch const& mismatch, Context& ctx) const {
    using namespace flamingo::installation;
    return std::visit(
        flamingo::util::overload{
          [&](MismatchTargetConv const& mismatch_conv) {
            return fmt::format_to(ctx.out(), "Target has calling convention: {} but specified: {} for hook: {}",
                             mismatch_conv.existing, mismatch_conv.incoming, mismatch_conv.installing_hook);
          },
          [&](MismatchMidpoint const& mismatch_midpoint) {
            return fmt::format_to(ctx.out(), "Target has midpoint specified as: {} but specified: {} for hook: {}",
                             mismatch_midpoint.existing, mismatch_midpoint.incoming, mismatch_midpoint.installing_hook);
          },
#ifndef FLAMINGO_NO_REGISTRATION_CHECKS
          [&](MismatchReturn const& mismatch_return) {
            return fmt::format_to(ctx.out(), "Target has return type specified as: {} but specified: {} for hook: {}",
                             mismatch_return.existing, mismatch_return.incoming, mismatch_return.installing_hook);
          },
          [&](MismatchParam const& mismatch_param) {
            return fmt::format_to(ctx.out(), "Target has parameter {} type specified as: {} but specified: {} for hook: {}",
                             mismatch_param.idx, mismatch_param.existing, mismatch_param.incoming,
                             mismatch_param.installing_hook);
          },
          [&](MismatchParamCount const& mismatch_param_count) {
            return fmt::format_to(ctx.out(), "Target has {} parameters but specified: {} for hook: {}",
                             mismatch_param_count.existing, mismatch_param_count.incoming,
                             mismatch_param_count.installing_hook);
          },
#endif
        },
        mismatch);
  }
};
