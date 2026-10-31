#pragma once

#include "../../_config.h"
#include "../TypeMacros.hpp"
#include "User.hpp"
#include "Metadata.hpp"
#include "Stats.hpp"
#include "BeatmapVersion.hpp"

namespace BeatSaver {
    class Beatmap;
    namespace API {
        SONGDOWNLOADER_EXPORT void DownloadBeatmapAsync(const BeatSaver::Beatmap& beatmap, std::function<void(bool)> finished, std::function<void(float)> progressUpdate);
        SONGDOWNLOADER_EXPORT void GetCoverImageAsync(const BeatSaver::Beatmap& beatmap, std::function<void(std::vector<uint8_t>)> finished, std::function<void(float)> progressUpdate);
    }

    DECLARE_JSON_STRUCT(Beatmap) {
        ERROR_CHECK
        GETTER_VALUE(std::string, Id, "id");
        GETTER_VALUE(std::string, Name, "name");
        GETTER_VALUE(std::string, Description, "description");
        GETTER_VALUE(BeatSaver::UserDetail, Uploader, "uploader");
        GETTER_VALUE(BeatSaver::Metadata, Metadata, "metadata");
        GETTER_VALUE(BeatSaver::Stats, Stats, "stats");
        GETTER_VALUE(std::string, Uploaded, "uploaded");
        GETTER_VALUE(bool, Automapper, "automapper");
        GETTER_VALUE(bool, Ranked, "ranked");
        GETTER_VALUE(bool, Qualified, "qualified");
        GETTER_VALUE(std::vector<BeatSaver::BeatmapVersion>, Versions, "versions");
        GETTER_VALUE_OPTIONAL(std::string, Curator, "curator");
        inline void DownloadLatestBeatmapAsync(std::function<void(bool)> finished, std::function<void(float)> progressUpdate) {
            BeatSaver::API::DownloadBeatmapAsync(*this, finished, progressUpdate);
        }
        inline void GetLatestCoverImageAsync(std::function<void(std::vector<uint8_t>)> finished, std::function<void(float)> progressUpdate) {
            BeatSaver::API::GetCoverImageAsync(*this, finished, progressUpdate);
        }
    };
}
