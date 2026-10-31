#pragma once

#include "../../_config.h"
#include "../TypeMacros.hpp"
#include "Difficulty.hpp"

namespace BeatSaver {
    class Beatmap;
    class BeatmapVersion;
    namespace API {
        SONGDOWNLOADER_EXPORT void DownloadBeatmapAsync(const BeatSaver::Beatmap& beatmap, const BeatSaver::BeatmapVersion& beatmapVer, std::function<void(bool)> finished, std::function<void(float)> progressUpdate);
        SONGDOWNLOADER_EXPORT void GetCoverImageAsync(const BeatSaver::BeatmapVersion& beatmap, std::function<void(std::vector<uint8_t>)> finished, std::function<void(float)> progressUpdate);
    }

    DECLARE_JSON_STRUCT(BeatmapVersion) {
        ERROR_CHECK
        GETTER_VALUE(std::string, CreatedAt, "createdAt");
        GETTER_VALUE_OPTIONAL(int, SageScore, "sageScore");
        GETTER_VALUE(std::vector<BeatSaver::BeatmapDifficulty>, Diffs, "diffs");
        GETTER_VALUE_OPTIONAL(std::string, Feedback, "feedback");
        GETTER_VALUE(std::string, Hash, "hash");
        GETTER_VALUE_OPTIONAL(std::string, Key, "key");
        GETTER_VALUE(std::string, State, "state"); // Enum with values Uploaded, Testplay, Published, Feedback
        //GETTER_VALUE(BeatSaver::Instant, TestplayAt, "testplayAt");
        //GETTER_VALUE(BeatSaver::Testplays, Uploaded, "uploaded");
        GETTER_VALUE(std::string, DownloadURL, "downloadURL");
        GETTER_VALUE(std::string, CoverURL, "coverURL");
        GETTER_VALUE(std::string, PreviewURL, "previewURL");
        inline void DownloadBeatmapAsync(const BeatSaver::Beatmap& beatmap, std::function<void(bool)> finished, std::function<void(float)> progressUpdate) {
            BeatSaver::API::DownloadBeatmapAsync(beatmap, *this, finished, progressUpdate);
        }
        inline void GetCoverImageAsync(std::function<void(std::vector<uint8_t>)> finished, std::function<void(float)> progressUpdate) {
            BeatSaver::API::GetCoverImageAsync(*this, finished, progressUpdate);
        }
    };
}
