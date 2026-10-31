#pragma once

#include <filesystem>

namespace SongCore::Utils {
    float GetLengthFromWavRiff(std::filesystem::path const& path);
}
