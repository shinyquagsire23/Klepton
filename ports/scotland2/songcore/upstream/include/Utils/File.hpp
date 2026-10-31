#pragma once

#include <vector>
#include <string>
#include <string_view>
#include <filesystem>

namespace SongCore::Utils {
    std::vector<std::string> GetFolders(std::string_view path);
    std::vector<std::filesystem::path> GetFolders(std::filesystem::path path);

    std::u16string ReadText(std::string_view path);
    std::u16string ReadText(std::filesystem::path path);

    const char* ReadBytes(std::string_view path, size_t& size_out);
}
