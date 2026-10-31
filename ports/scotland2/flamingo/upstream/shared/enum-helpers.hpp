#pragma once
#include <cstdint>
#include <type_traits>

namespace flamingo::enum_helpers {

/// @brief Adds the provided enumeration flag to the given enumeration value.
template <auto flag>
requires(std::is_enum_v<decltype(flag)>) constexpr auto AddFlag(auto lhs) noexcept {
    return decltype(lhs)(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(flag));
}

/// @brief Checks if a given enumeration value has the provided enumeration flag.
template <auto flag>
requires(std::is_enum_v<decltype(flag)>) constexpr auto HasFlag(auto lhs) noexcept {
    return (static_cast<uint32_t>(lhs) & static_cast<uint32_t>(flag)) != 0;
}

}  // namespace flamingo::enum_helpers
