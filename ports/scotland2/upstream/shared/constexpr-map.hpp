#pragma once
#include <array>
#include <cstdint>
#include <utility>

namespace modloader {

template <class K, class V, std::size_t Sz>
struct ConstexprMap {
  constexpr explicit ConstexprMap(std::array<std::pair<K, V>, Sz> const& arr) : arr(arr) {}

  std::array<std::pair<K, V>, Sz> arr;
};

}  // namespace modloader
