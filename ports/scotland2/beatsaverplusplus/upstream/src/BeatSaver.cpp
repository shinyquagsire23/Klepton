#include "BeatSaver.hpp"
#include "Utils.hpp"
#include "logging.hpp"

#include "web-utils/shared/RatelimitedDispatcher.hpp"
#include "Exceptions.hpp"
#include <chrono>
#include <ctime>
#include <future>
#include <variant>

namespace BeatSaver::API {
    WebUtils::DownloaderUtility const& GetBeatsaverDownloader() {
        static const WebUtils::DownloaderUtility downloader{.userAgent=MOD_ID "/" VERSION " (+https://github.com/RedBrumbler/BeatSaverPlusPlus)"};
        return downloader;
    }

    static std::filesystem::path _defaultOutputRoothPath = "/sdcard/ModData/com.beatgames.beatsaber/Mods/SongCore/CustomLevels";

    void Init(std::filesystem::path defaultOutputRootPath) {
        _defaultOutputRoothPath = defaultOutputRootPath;
    }

    std::filesystem::path GetDefaultOutputPath() {
        return _defaultOutputRoothPath;
    }

    std::future<bool> DownloadSongZipAsync(WebUtils::URLOptions urlOptions, std::filesystem::path outputPath, std::function<void(float)> progressReport) {
        return std::async(std::launch::any, &DownloadSongZip, std::forward<WebUtils::URLOptions>(urlOptions), std::forward<std::filesystem::path>(outputPath), std::forward<std::function<void(float)>>(progressReport));
    }

    bool DownloadSongZip(WebUtils::URLOptions urlOptions, std::filesystem::path outputPath, std::function<void(float)> progressReport) {
        auto data = GetBeatsaverDownloader().Get<WebUtils::DataResponse>(urlOptions, progressReport);
        if (!data.IsSuccessful() || !data.DataParsedSuccessful()) return false;
        auto& zipData = data.responseData.value();

        return Utils::ExtractAll(zipData, outputPath);
    }

    std::string timestamp_string(std::chrono::time_point<std::chrono::system_clock> timepoint) {
        using namespace std::chrono;
        auto since_epoch = timepoint.time_since_epoch();
        auto years_since = duration_cast<years>(since_epoch);
        auto months_since = duration_cast<months>(since_epoch - years_since);
        auto days_since = duration_cast<days>(since_epoch - years_since - months_since);
        auto hours_since = duration_cast<hours>(since_epoch - years_since - months_since - days_since);
        auto minutes_since = duration_cast<minutes>(since_epoch - years_since - months_since - days_since - hours_since);
        auto seconds_since = duration_cast<seconds>(since_epoch - years_since - months_since - days_since - hours_since - minutes_since);
        return fmt::format("{}-{}-{}T{}:{}:{}Z", years_since.count() + 1900, months_since.count() + 1, days_since.count() + 1, hours_since.count(), minutes_since.count(), seconds_since.count());
    }

    std::string timestamp_string(timestamp const& stamp) {
        if (std::holds_alternative<std::string>(stamp)) return std::get<std::string>(stamp);
        else if (std::holds_alternative<std::chrono::time_point<std::chrono::system_clock>>(stamp)) return timestamp_string(std::get<std::chrono::time_point<std::chrono::system_clock>>(stamp));
        return "";
    }

    WebUtils::URLOptions::QueryMap SearchQueryOptions::GetQueries() const {
        WebUtils::URLOptions::QueryMap queries;
        // from the docs: Options are a little weird, I may add another enum field in future to make this clearer. true = both, false = only ai, null = no ai
        switch (automapper) {
            break; case Filter::Ignore: queries.emplace("automapper", "true");
            break; case Filter::Include: queries.emplace("automapper", "false");
            break; case Filter::Exclude: break;
        }

        if (query.has_value()) queries["q"] = query.value();

        switch(sortOrder) {
            using enum SearchSortOrder;
            case Latest: { queries.emplace("sortOrder", "Latest"); } break;
            case Relevance: { queries.emplace("sortOrder", "Relevance"); } break;
            case Rating: { queries.emplace("sortOrder", "Rating"); } break;
            case Curated: { queries.emplace("sortOrder", "Curated"); } break;
        }

        if (chroma != Filter::Ignore) queries["chroma"] = std::to_string(chroma == Filter::Include);
        if (noodle != Filter::Ignore) queries["noodle"] = std::to_string(noodle == Filter::Include);
        if (me != Filter::Ignore) queries["me"] = std::to_string(me == Filter::Include);
        if (cinema != Filter::Ignore) queries["cinema"] = std::to_string(cinema == Filter::Include);
        if (ranked != Filter::Ignore) queries["ranked"] = std::to_string(ranked == Filter::Include);
        if (verified != Filter::Ignore) queries["verified"] = std::to_string(verified == Filter::Include);
        if (fullspread != Filter::Ignore) queries["fullspread"] = std::to_string(fullspread == Filter::Include);

        if (from.has_value()) queries["from"] = timestamp_string(from.value());
        if (to.has_value()) queries["to"] = timestamp_string(to.value());

        std::vector<std::string> tags;
        tags.reserve(includeTags.size() + excludeTags.size());
        for (auto t : includeTags) tags.emplace_back(t);
        for (auto t : excludeTags) tags.emplace_back(fmt::format("!{}", t));
        if (!tags.empty()) queries["tags"] = fmt::format("{}", fmt::join(tags, ","));

        if (maxBpm.has_value()) queries["maxBpm"] = std::to_string(maxBpm.value());
        if (minBpm.has_value()) queries["minBpm"] = std::to_string(minBpm.value());
        if (maxDuration.has_value()) queries["maxDuration"] = std::to_string(maxDuration.value());
        if (minDuration.has_value()) queries["minDuration"] = std::to_string(minDuration.value());
        if (maxNps.has_value()) queries["maxNps"] = std::to_string(maxNps.value());
        if (minNps.has_value()) queries["minNps"] = std::to_string(minNps.value());
        if (maxRating.has_value()) queries["maxRating"] = std::to_string(maxRating.value());
        if (minRating.has_value()) queries["minRating"] = std::to_string(minRating.value());

        return queries;
    }

    WebUtils::URLOptions::QueryMap LatestQueryOptions::GetQueries() const {
        WebUtils::URLOptions::QueryMap queries;
        // from the docs: Options are a little weird, I may add another enum field in future to make this clearer. true = both, false = only ai, null = no ai
        switch (automapper) {
            break; case Filter::Ignore: queries.emplace("automapper", "true");
            break; case Filter::Include: queries.emplace("automapper", "false");
            break; case Filter::Exclude: break;
        }

        if (sortOrder.has_value()) {
            switch (sortOrder.value()) {
                using enum LatestSortOrder;
                case FirstPublished: { queries.emplace("sortOrder", "FIRST_PUBLISHED"); } break;
                case Updated: { queries.emplace("sortOrder", "UPDATED"); } break;
                case LastPublished: { queries.emplace("sortOrder", "LAST_PUBLISHED"); } break;
                case Created: { queries.emplace("sortOrder", "CREATED"); } break;
                case Curated: { queries.emplace("sortOrder", "CURATED"); } break;
            }
        }

        if (pageSize.has_value()) queries["pageSize"] = std::to_string(std::clamp(pageSize.value(), 1, 100));
        if (verified != Filter::Ignore) queries["verified"] = std::to_string(verified == Filter::Include);

        if (before.has_value()) queries["before"] = timestamp_string(before.value());
        if (after.has_value()) queries["after"] = timestamp_string(after.value());

        return queries;
    }

    WebUtils::URLOptions::QueryMap CollaborationQueryOptions::GetQueries() const {
        WebUtils::URLOptions::QueryMap queries;

        if (before.has_value()) queries["before"] = timestamp_string(before.value());
        if (pageSize.has_value()) queries["pageSize"] = std::to_string(std::clamp(pageSize.value(), 1, 100));

        return queries;
    }

    WebUtils::URLOptions::QueryMap LatestPlaylistsQueryOptions::GetQueries() const {
        WebUtils::URLOptions::QueryMap queries;
        if (after.has_value()) queries["after"] = timestamp_string(after.value());
        if (before.has_value()) queries["before"] = timestamp_string(before.value());
        if (pageSize.has_value()) queries["pageSize"] = std::to_string(pageSize.value());

        if (sort.has_value()) {
            switch (sort.value()) {
                using enum LatestPlaylistSortOrder;
                case Updated:       queries["sort"] = "UPDATED"; break;
                case SongsUpdated:  queries["sort"] = "SONGS_UPDATED"; break;
                case Created:       queries["sort"] = "CREATED"; break;
                case Curated:       queries["sort"] = "CURATED"; break;
            }
        }

        return queries;
    }

    WebUtils::URLOptions::QueryMap SearchPlaylistsQueryOptions::GetQueries() const {
        WebUtils::URLOptions::QueryMap queries;
        if (query.has_value()) queries["q"] = query.value();
        switch (sortOrder) {
            using enum SearchPlaylistSortOrder;
            case Latest:    queries["sortOrder"] = "Latest"; break;
            case Relevance: queries["sortOrder"] = "Relevance"; break;
            case Rating:    queries["sortOrder"] = "Rating"; break;
            case Curated:   queries["sortOrder"] = "Curated"; break;
        }

        if (curated != Filter::Ignore) queries["curated"] = std::to_string(curated == Filter::Include);
        if (verified != Filter::Ignore) queries["verified"] = std::to_string(verified == Filter::Include);
        if (includeEmpty.has_value()) queries["includeEmpty"] = std::to_string(includeEmpty.value());
        if (from.has_value()) queries["from"] = timestamp_string(from.value());
        if (to.has_value()) queries["to"] = timestamp_string(to.value());
        if (maxNps.has_value()) queries["maxNps"] = std::to_string(maxNps.value());
        if (minNps.has_value()) queries["minNps"] = std::to_string(minNps.value());

        return queries;
    }

    WebUtils::URLOptions::QueryMap VoteQueryOptions::GetQueries() const {
        WebUtils::URLOptions::QueryMap queries;

        if (since.has_value()) queries["since"] = timestamp_string(since.value());

        return queries;
    }

    rapidjson::Value& PlatformAuth::Serialize(PlatformAuth const& instance, rapidjson::Value& json, rapidjson::Value::AllocatorType& allocator) {
        json.SetObject();
        switch (instance.platform) {
            using enum UserPlatform;
            case Oculus: {
                json.AddMember("oculusId", rapidjson::Value(instance.userId, allocator), allocator);
                json.AddMember("steamId", rapidjson::Value(), allocator);
            } break;
            case Steam: {
                json.AddMember("steamId", rapidjson::Value(instance.userId, allocator), allocator);
                json.AddMember("oculusId", rapidjson::Value(), allocator);
            } break;
        }

        json.AddMember("proof", rapidjson::Value(instance.proof, allocator), allocator);

        return json;
    }

    std::string PlatformAuth::SerializeToString() const {
        rapidjson::Document doc;
        doc.SetObject();

        Serialize(*this, doc, doc.GetAllocator());

        rapidjson::StringBuffer buf;
        rapidjson::Writer writer(buf);
        doc.Accept(writer);

        return {buf.GetString(), buf.GetLength()};
    }

    std::string BEATSAVER_PLUSPLUS_EXPORT CreateVoteData(PlatformAuth auth, bool direction, std::string hash) {
        rapidjson::Document doc;
        doc.SetObject();
        auto& allocator = doc.GetAllocator();

        rapidjson::Value authVal;
        PlatformAuth::Serialize(auth, authVal, allocator);

        doc.AddMember("auth", authVal, allocator);
        doc.AddMember("direction", direction, allocator);
        doc.AddMember("hash", rapidjson::StringRef(hash), allocator);

        rapidjson::StringBuffer buf;
        rapidjson::Writer writer(buf);
        doc.Accept(writer);

        return {buf.GetString(), buf.GetLength()};
    }

    std::span<const std::string> GetMapFeelTags() {
        static std::string tags[] = {
            "Accuracy",
            "Balanced",
            "Challenge",
            "Dance",
            "Fitness",
            "Speed",
            "Tech"
        };
        return tags;
    }

    std::span<const std::string> GetGenreTags() {
        static std::string tags[] = {
            "Alternative",
            "Ambient",
            "Anime",
            "Classic & Orchestral",
            "Comedy & Meme",
            "Dance",
            "Drum and Bass",
            "Dubstep",
            "Electronic",
            "Folk & Acoustic",
            "Funk & Disco",
            "Hardcore",
            "Hip Hop & Rap",
            "Holiday",
            "House",
            "Indie",
            "Instrumental",
            "J-Pop",
            "J-Rock",
            "Jazz",
            "K-Pop",
            "Kids & Family",
            "Metal",
            "Nightcore",
            "Pop",
            "Punk",
            "R&B",
            "Rock",
            "Soul",
            "SpeedCore",
            "Swing",
            "TV & Film",
            "Techno",
            "Trance",
            "Video Game",
            "Vocaloid",
        };
        return tags;
    }

    bool DownloadBeatmapResponse::AcceptData(std::span<uint8_t const> data) {
        auto basePath = GetDefaultOutputPath();
        auto targetPath = basePath / Utils::ReplaceIllegalCharsInPath(info.FolderName);
        if (Utils::ExtractAll(data, targetPath)) {
            responseData = targetPath;
            return true;
        }
        return false;
    }

    std::string BeatmapDownloadInfo::SanitizeFolderName(std::string_view str) {
        static const std::string AllowedChars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890()[]{}%&.,;=!-_ ";
        std::string folderName(str);
        for (auto& c : folderName) {
            // if you can't find this char in allowed chars, set it to _
            if (AllowedChars.find(c) == std::string::npos)
                c = '_';
        }

        return folderName;
    }
}
