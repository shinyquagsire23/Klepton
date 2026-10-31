#pragma once

#include <filesystem>

namespace SongCore::Utils {
    float GetLengthFromOggVorbis(std::filesystem::path path);
}
