#pragma once

#include <cstddef>
#include <type_traits>
#include <fmt/format.h>
#include <fmt/compile.h>

namespace flamingo {
/// @brief Represents the type info for representing a type in a hook
struct TypeInfo {
  friend struct HookInfo;
  // TODO: Add more members here, like name. Ideally some form of relaxed type checking?
  std::size_t size{};

 private:
  template <class T>
  [[nodiscard]] inline static TypeInfo from() {
    if constexpr (std::is_reference_v<T>) {
      return TypeInfo{
        .size = sizeof(void*),
      };
    } else if constexpr (std::is_void_v<T>) {
      return TypeInfo{
        .size = 0,
      };
    } else {
      return TypeInfo{
        .size = sizeof(T),
      };
    }
  }
};

inline bool operator==(TypeInfo const& lhs, TypeInfo const& rhs) {
  return lhs.size == rhs.size;
}
}  // namespace flamingo

// Custom formatter for flamingo::TypeInfo
template <>
class fmt::formatter<flamingo::TypeInfo> {
 public:
  constexpr auto parse(format_parse_context& ctx) {
    return ctx.begin();
  }
  template <typename Context>
  constexpr auto format(flamingo::TypeInfo const& info, Context& ctx) const {
    return fmt::format_to(ctx.out(), "(size={})", info.size);
  }
};
