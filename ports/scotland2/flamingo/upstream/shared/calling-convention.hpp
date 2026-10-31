#pragma once

#include <fmt/format.h>
#include <fmt/compile.h>

namespace flamingo {
/// @brief Represents the calling convention for a given hook.
/// Used primarily for type checking
enum struct CallingConvention { Cdecl, Fastcall, Thiscall };
}  // namespace flamingo

// Custom formatter for flamingo::CallingConvention
// TODO: Maybe consider pulling in magic_enum?
template <>
class fmt::formatter<flamingo::CallingConvention> {
 public:
  constexpr auto parse(format_parse_context& ctx) {
    return ctx.begin();
  }
  template <typename Context>
  constexpr auto format(flamingo::CallingConvention const& conv, Context& ctx) const {
    switch (conv) {
      case flamingo::CallingConvention::Cdecl:
        return fmt::format_to(ctx.out(), "Cdecl");
      case flamingo::CallingConvention::Fastcall:
        return fmt::format_to(ctx.out(), "Fastcall");
      case flamingo::CallingConvention::Thiscall:
        return fmt::format_to(ctx.out(), "Thiscall");
    }
  }
};
