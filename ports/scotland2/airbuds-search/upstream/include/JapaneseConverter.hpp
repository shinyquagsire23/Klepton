#pragma once

#include <cstddef>
#include <cstdint>

namespace AirbudsSearch {

struct IJapaneseConverter {
    uint32_t apiVersion = 0;
    const char* name = nullptr;
    bool (*convert)(const char* utf8, char* outBuffer, size_t outBufferSize) = nullptr;
};

using GetJapaneseConverterFn = const IJapaneseConverter* (*)();

constexpr const char* kJapaneseConverterSymbol = "airbuds_search_get_japanese_converter_v1";

} // namespace AirbudsSearch
