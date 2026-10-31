#pragma once

#include "GlobalNamespace/BeatmapLevel.hpp"
#include "GlobalNamespace/BeatmapLevelPack.hpp"
#include "UnityEngine/Texture2D.hpp"

namespace PlaylistCore::Utils {
    bool IsWipLevel(GlobalNamespace::BeatmapLevel* level);

    void RemoveAllBMBFSuffixes();

    bool UniqueFileName(std::string_view fileName, std::string_view compareDirectory);

    std::string GetNewPlaylistPath(std::string title);

    std::string GetBase64ImageType(std::string_view base64);

    std::string ProcessImage(UnityEngine::Texture2D* texture, bool returnPngString);
}
