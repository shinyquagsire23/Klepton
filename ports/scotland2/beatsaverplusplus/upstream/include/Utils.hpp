#pragma once

#include <string>
#include <filesystem>
#include <ranges>

namespace BeatSaver::Utils {
    std::string ReplaceIllegalCharsInPath(std::string path);

    bool ExtractAll(std::span<uint8_t const> zipData, std::filesystem::path const& outputPath);

    std::optional<std::vector<uint8_t>> GetData(std::string dataURL);
}
