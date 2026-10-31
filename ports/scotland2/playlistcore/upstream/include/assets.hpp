#pragma once

#include "metacore/shared/assets.hpp"

#define DECLARE_ASSET(name, binary)       \
    const IncludedAsset name {            \
        Externs::_binary_##binary##_start, \
        Externs::_binary_##binary##_end    \
    };

#define DECLARE_ASSET_NS(namespaze, name, binary) \
    namespace namespaze { DECLARE_ASSET(name, binary) }

namespace IncludedAssets {
    namespace Externs {
        extern "C" uint8_t _binary_LevelPack_png_start[];
        extern "C" uint8_t _binary_LevelPack_png_end[];
    }

    // LevelPack.png
    DECLARE_ASSET(LevelPack_png, LevelPack_png);
}
