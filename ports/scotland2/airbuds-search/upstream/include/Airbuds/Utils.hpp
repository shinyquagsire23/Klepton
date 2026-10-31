#pragma once

#include <vector>

#include <web-utils/shared/WebUtils.hpp> // For rapidjson

#include "Image.hpp"

namespace airbuds {

Image getSmallestImage(const std::vector<Image>& images);

constexpr size_t getChunkCount(const size_t total, const size_t maxChunkSize) {
    return (total + maxChunkSize - 1) / maxChunkSize;
}

} // namespace airbuds