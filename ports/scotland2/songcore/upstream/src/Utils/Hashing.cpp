#include "Utils/Hashing.hpp"
#include "CustomJSONData.hpp"
#include "Utils/Cache.hpp"
#include "logging.hpp"
#include <filesystem>

#include "libcryptopp/shared/sha.h"
#include "libcryptopp/shared/hex.h"
#include "libcryptopp/shared/files.h"

using namespace GlobalNamespace;
using namespace CryptoPP;

namespace SongCore::Utils {
    std::optional<std::string> GetCustomLevelHash(std::filesystem::path const& levelPath, SongCore::CustomJSONData::CustomLevelInfoSaveDataV2* saveData) {
        auto start = std::chrono::high_resolution_clock::now();
        std::string hashHex;

        // get cached info
        auto cacheData = GetCachedInfo(levelPath);
        if(!cacheData.has_value()) return std::nullopt;

        if (cacheData->sha1.has_value()) {
            DEBUG("GetCustomLevelHash Stop Result {} from cache", *cacheData->sha1);
            return *cacheData->sha1;
        }

        auto infoPath = levelPath / "info.dat";
        if(!std::filesystem::exists(infoPath)) {
            infoPath = levelPath / "Info.dat";
            if(!std::filesystem::exists(infoPath)) return std::nullopt;
        }

        SHA1 hashType;
        std::string hashResult;
        HashFilter hashFilter(hashType, new StringSink(hashResult));

        FileSource fs(infoPath.c_str(), false);
        fs.Attach(new Redirector(hashFilter));
        fs.Pump(LWORD_MAX);
        fs.Detach();
        for(auto val : saveData->difficultyBeatmapSets) {
            if (!val) continue;
            auto difficultyBeatmaps = val->difficultyBeatmaps;
            if (!difficultyBeatmaps) continue;
            for(auto difficultyBeatmap : difficultyBeatmaps) {
                auto diffPath = levelPath / static_cast<std::string>(difficultyBeatmap->beatmapFilename);
                if(!std::filesystem::exists(diffPath)) {
                    ERROR("GetCustomLevelHash File {} did not exist", diffPath.string());
                    continue;
                }
                FileSource fs(diffPath.c_str(), false);
                fs.Attach(new Redirector(hashFilter));
                fs.Pump(LWORD_MAX);
                fs.Detach();
            }
        }

        hashFilter.MessageEnd();

        HexEncoder hexEncoder(new StringSink(hashHex));
        hexEncoder.Put((const byte*)hashResult.data(), hashResult.size());

        cacheData->sha1 = hashHex;
        SetCachedInfo(levelPath, *cacheData);

        std::chrono::milliseconds duration = duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start);
        DEBUG("GetCustomLevelHash Stop Result {} Time {}", hashHex, duration.count());
        return hashHex;
    }

    std::optional<std::string> GetCustomLevelHash(std::filesystem::path const& levelPath, SongCore::CustomJSONData::CustomBeatmapLevelSaveDataV4* saveData) {
        auto start = std::chrono::high_resolution_clock::now();
        std::string hashHex;

        // get cached info
        auto cacheData = GetCachedInfo(levelPath);
        if(!cacheData.has_value()) return std::nullopt;

        if (cacheData->sha1.has_value()) {
            DEBUG("GetCustomLevelHash Stop Result {} from cache", *cacheData->sha1);
            return *cacheData->sha1;
        }

        auto infoPath = levelPath / "info.dat";
        if(!std::filesystem::exists(infoPath)) {
            infoPath = levelPath / "Info.dat";
            if(!std::filesystem::exists(infoPath)) return std::nullopt;
        }

        auto audioPath = levelPath / static_cast<std::string>(saveData->audio.audioDataFilename);
        if(!std::filesystem::exists(audioPath)) {
            return std::nullopt;
        }

        SHA1 hashType;
        std::string hashResult;
        HashFilter hashFilter(hashType, new StringSink(hashResult));

        FileSource fs(infoPath.c_str(), false);
        fs.Attach(new Redirector(hashFilter));
        fs.Pump(LWORD_MAX);
        fs.Detach();

        FileSource fsAudio(audioPath.c_str(), false);
        fsAudio.Attach(new Redirector(hashFilter));
        fsAudio.Pump(LWORD_MAX);
        fsAudio.Detach();

        for(auto val : saveData->difficultyBeatmaps) {
            if (!val) continue;
            
            auto diffPath = levelPath / static_cast<std::string>(val->beatmapDataFilename);
            if(!std::filesystem::exists(diffPath)) {
                ERROR("GetCustomLevelHash File {} did not exist", diffPath.string());
                continue;
            }
            FileSource fs(diffPath.c_str(), false);
            fs.Attach(new Redirector(hashFilter));
            fs.Pump(LWORD_MAX);
            fs.Detach();

            auto lightPath = levelPath / static_cast<std::string>(val->lightshowDataFilename);
            if(!std::filesystem::exists(lightPath)) {
                ERROR("GetCustomLevelHash Lighting File {} did not exist", diffPath.string());
                continue;
            }
            FileSource fsLight(lightPath.c_str(), false);
            fsLight.Attach(new Redirector(hashFilter));
            fsLight.Pump(LWORD_MAX);
            fsLight.Detach();
        }

        hashFilter.MessageEnd();

        HexEncoder hexEncoder(new StringSink(hashHex));
        hexEncoder.Put((const byte*)hashResult.data(), hashResult.size());

        cacheData->sha1 = hashHex;
        SetCachedInfo(levelPath, *cacheData);

        std::chrono::milliseconds duration = duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start);
        DEBUG("GetCustomLevelHash Stop Result {} Time {}", hashHex, duration.count());
        return hashHex;
    }

    std::optional<int> GetDirectoryHash(std::filesystem::path const& directoryPath) {
        if (!std::filesystem::is_directory(directoryPath)) return std::nullopt;

        int hash = 0;
        bool hasFile = false;
        std::error_code error_code;
        auto dir_iter = std::filesystem::directory_iterator(directoryPath, error_code);

        if (error_code) {
            WARNING("Failed to get directory iterator for directory {}: {}", directoryPath.string(), error_code.message());
            return std::nullopt;
        }

        for (auto const& entry : dir_iter) {
            if(!entry.is_directory()) {
                hasFile = true;
                hash ^= entry.file_size() ^ std::chrono::duration_cast<std::chrono::seconds>(std::filesystem::last_write_time(entry).time_since_epoch()).count();
            }
        }

        if(!hasFile)
            return std::nullopt;
        return hash;
    }
}
