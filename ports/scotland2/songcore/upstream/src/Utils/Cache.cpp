#include "Utils/Cache.hpp"
#include "Utils/Hashing.hpp"
#include "Utils/File.hpp"
#include "logging.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <fstream>

#include "paper2_scotland2/shared/utfcpp/source/utf8.h"

namespace SongCore::Utils {
    rapidjson::Value CachedSongData::Serialize(rapidjson::Document::AllocatorType& allocator) const {
        rapidjson::Value val;
        val.SetObject();

        val.AddMember("directoryHash", directoryHash, allocator);
        if (sha1.has_value()) val.AddMember("sha1", rapidjson::Value(sha1->c_str(), sha1->size(), allocator), allocator);
        if (songDuration.has_value()) val.AddMember("songDuration", songDuration.value(), allocator);

        return val;
    }

    bool CachedSongData::Deserialize(rapidjson::Value const& value) {
        bool foundEverything = true;
        auto memberEnd = value.MemberEnd();
        auto directoryHashItr = value.FindMember("directoryHash");
        if (directoryHashItr != memberEnd && directoryHashItr->value.IsInt()) {
            directoryHash = directoryHashItr->value.GetInt();
        } else {
            foundEverything = false;
        }

        auto sha1Itr = value.FindMember("sha1");
        if (sha1Itr != memberEnd && sha1Itr->value.IsString()) {
            sha1 = sha1Itr->value.Get<std::string>();
        } // optional so foundEverything unaffected

        auto songDurationItr = value.FindMember("songDuration");
        if (songDurationItr != memberEnd && songDurationItr->value.IsFloat()) {
            songDuration = songDurationItr->value.GetFloat();
        } // optional so foundEverything unaffected

        return foundEverything;
    }

    static std::shared_mutex _cacheMutex;
    static std::unordered_map<std::string, CachedSongData> _cachedSongData;
    static std::filesystem::path _cachePath = "/sdcard/ModData/com.beatgames.beatsaber/Mods/SongCore/CachedSongData.json";

    std::optional<CachedSongData> GetCachedInfo(std::filesystem::path const& levelPath) {
        auto dirHashOpt = Utils::GetDirectoryHash(levelPath);
        if (!dirHashOpt.has_value()) {
            WARNING("Can't get cached info for {} because directory hash could not be calculated!", levelPath.string());
            return std::nullopt;
        }

        auto directoryHash = *dirHashOpt;
        std::shared_lock<std::shared_mutex> lock(_cacheMutex);
        auto itr = _cachedSongData.find(levelPath);
        // if found and dir hash matches, we found a correct value
        if (itr != _cachedSongData.end() && itr->second.directoryHash == directoryHash) return itr->second;
        lock.unlock();

        // make a new entry and set it in the map, and then return that
        CachedSongData newCacheEntry;
        newCacheEntry.directoryHash = directoryHash;
        SetCachedInfo(levelPath, newCacheEntry);
        return newCacheEntry;
    }

    void SetCachedInfo(std::filesystem::path const& levelPath, CachedSongData const& newInfo) {
        std::unique_lock<std::shared_mutex> lock(_cacheMutex);
        _cachedSongData[levelPath] = newInfo;
    }

    void RemoveCachedInfo(std::filesystem::path const& levelPath) {
        std::shared_lock<std::shared_mutex> shared_lock(_cacheMutex);
        auto itr = _cachedSongData.find(levelPath);
        if (itr == _cachedSongData.end()) return;
        shared_lock.unlock();

        std::unique_lock<std::shared_mutex> unique_lock(_cacheMutex);
        _cachedSongData.erase(itr);
    }

    void ClearSongInfoCache() {
        std::unique_lock<std::shared_mutex> lock(_cacheMutex);
        _cachedSongData.clear();
    }

    void SaveSongInfoCache() {
        rapidjson::Document doc;
        doc.SetObject();
        auto& allocator = doc.GetAllocator();

        std::shared_lock<std::shared_mutex> lock(_cacheMutex);
        for (auto& [levelPath, data] : _cachedSongData) {
            // skip saving levels that no longer exist
            if (!std::filesystem::exists(levelPath)) continue;

            rapidjson::Value memberName(levelPath.c_str(), levelPath.size(), allocator);
            doc.AddMember(memberName, data.Serialize(allocator), allocator);
        }
        lock.unlock();

        rapidjson::StringBuffer buff;
        rapidjson::Writer writer(buff);

        std::ofstream cacheFile(_cachePath, std::ios::out);
        doc.Accept(writer);

        cacheFile.write(buff.GetString(), buff.GetLength());
    }

    bool LoadSongInfoCache() {
        // if the file doesn't exist, load should fail
        if (!std::filesystem::exists(_cachePath)) return false;

        bool foundEverything = true;
        auto text = utf8::utf16to8(ReadText(_cachePath));

        rapidjson::Document doc;
        doc.Parse(text);
        auto memberEnd = doc.MemberEnd();

        std::unique_lock<std::shared_mutex> lock(_cacheMutex);
        _cachedSongData.clear();
        for (auto itr = doc.MemberBegin(); itr != memberEnd; itr++) {
            std::filesystem::path levelPath = itr->name.Get<std::string>();
            // skip levels that no longer exist
            if (!std::filesystem::exists(levelPath)) continue;
            if (!_cachedSongData[levelPath].Deserialize(itr->value)) foundEverything = false;
        }
        lock.unlock();

        return foundEverything;
    }
}
