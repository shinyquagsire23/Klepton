#pragma once

#include <optional>
#include <string>
#include <filesystem>
#include "beatsaber-hook/shared/config/rapidjson-utils.hpp"

namespace SongCore::Utils {
    struct CachedSongData {
        int directoryHash;
        std::optional<std::string> sha1 = std::nullopt;
        std::optional<float> songDuration = std::nullopt;

        /// @brief serializes the data to a rapidjson value using the provided allocator
        rapidjson::Value Serialize(rapidjson::Document::AllocatorType& allocator) const;

        /// @brief deserializes the songdata from a provided value
        /// @return true if deserialization was succesful, false if something required wasn't found
        bool Deserialize(rapidjson::Value const& value);
    };

    /// @brief gets the cached info for the level
    /// @return optional song info entry, if the info isn't able to provided this returns nullopt (i.e. no song found at path)
    std::optional<CachedSongData> GetCachedInfo(std::filesystem::path const& levelPath);

    /// @brief sets the cached info for a path
    void SetCachedInfo(std::filesystem::path const& levelPath, CachedSongData const& newInfo);

    /// @brief just removes cached info if it exists
    void RemoveCachedInfo(std::filesystem::path const& levelPath);

    /// @brief clears all entries from song info cache
    void ClearSongInfoCache();

    /// @brief saves the current state of the cache to disk storage
    void SaveSongInfoCache();

    /// @brief loads the current state of the cache from disk storage
    /// @return boolean whether cache loaded succesfully
    bool LoadSongInfoCache();
}
