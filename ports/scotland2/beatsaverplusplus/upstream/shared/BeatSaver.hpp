#pragma once

#include "./_config.h"
#include "./macros.hpp"
#include "./Models/SearchPage.hpp"
#include "./Models/UserDetail.hpp"
#include "./Models/VoteSummary.hpp"

#include "./Models/PlaylistPage.hpp"
#include "./Models/PlaylistSearchPage.hpp"

#include "BeatSaver.hpp"
#include "web-utils/shared/DownloaderUtility.hpp"
#include "web-utils/shared/RatelimitedDispatcher.hpp"

#include <chrono>
#include <future>
#include <map>
#include <type_traits>

#if defined(BEATSAVER_PLUSPLUS_AUTO_INIT) && __has_include("songcore/shared/SongCore.hpp")
#include "songcore/shared/SongCore.hpp"
/// @brief automatically initialize the library if you define the macro and have songcore as a dependency as well
static __attribute__((constructor)) void beatsaver_plusplus_auto_init() {
    BeatSaver::API::Init(SongCore::API::Loading::GetPreferredCustomLevelPath());
}
#endif

/// API in this header is based on https://api.beatsaver.com/docs/index.html?url=./swagger.json

#define BEATSAVER_API_URL "https://api.beatsaver.com"
#define BEATSAVER_CDN_URL "https://cdn.beatsaver.com"

namespace BeatSaver::API {
    /// @brief method to get the downloader that beatsaverplusplus uses internally
    BEATSAVER_PLUSPLUS_EXPORT WebUtils::DownloaderUtility const& GetBeatsaverDownloader();

    /// @brief initialize with the root dir where songs should be output
    BEATSAVER_PLUSPLUS_EXPORT void Init(std::filesystem::path defaultOutputRootPath);

    /// @brief Get the root dir where songs are saved to. defaults to the SongCore/CustomLevels directory if it was never initialized
    BEATSAVER_PLUSPLUS_EXPORT std::filesystem::path GetDefaultOutputPath();

    /// @brief downloads a song zip from the url to a given path
    /// @param urlOptions the url options to download the file from
    /// @param outputPath the output directory, should not be CustomLevels, but the output path
    /// @return bool future success, if download failed or extraction of the file failed, false will return.
    std::future<bool> BEATSAVER_PLUSPLUS_EXPORT DownloadSongZipAsync(WebUtils::URLOptions urlOptions, std::filesystem::path outputPath, std::function<void(float)> progressReport = nullptr);

    /// @brief downloads a song zip from the url to a given path
    /// @param urlOptions the url options to download the file from
    /// @param outputPath the output directory, should not be CustomLevels, but the output path
    /// @return bool success, if download failed or extraction of the file failed, false will return.
    bool BEATSAVER_PLUSPLUS_EXPORT DownloadSongZip(WebUtils::URLOptions urlOptions, std::filesystem::path outputPath, std::function<void(float)> progressReport = nullptr);
#pragma region responses
    BEATSAVER_PLUSPLUS_DECLARE_SIMPLE_RESPONSE_T(Models, SearchPage);
    BEATSAVER_PLUSPLUS_DECLARE_SIMPLE_RESPONSE_T(Models, Beatmap);
    BEATSAVER_PLUSPLUS_DECLARE_SIMPLE_RESPONSE_T(Models, UserDetail);
    BEATSAVER_PLUSPLUS_DECLARE_SIMPLE_RESPONSE_T(Models, PlaylistSearchPage);
    BEATSAVER_PLUSPLUS_DECLARE_SIMPLE_RESPONSE_T(Models, PlaylistPage);

    struct BEATSAVER_PLUSPLUS_EXPORT BeatmapMapResponse : public WebUtils::GenericResponse<std::unordered_map<std::string, Models::Beatmap>> {
        bool AcceptData(std::span<uint8_t const> data) override {
            rapidjson::Document doc;
            doc.Parse((char*)data.data(), data.size());
            if (doc.HasParseError()) return false;
            try {
                auto memberEnd = doc.MemberEnd();
                BEATSAVER_PLUSPLUS_ERROR_CHECK(doc);
                std::unordered_map<std::string, Models::Beatmap> output;

                // try to find id member in the doc root, if found this is a singular map response
                auto idItr = doc.FindMember("id");
                if (idItr != doc.MemberEnd()) {
                    auto map = doc.Get<Models::Beatmap>();
                    output[map.Versions.front().Hash] = std::move(map);
                } else {
                    // we did not find "id" in doc root, so this is multiple maps
                    for (auto itr = doc.MemberBegin(); itr != memberEnd; itr++) {
                        if (itr->value.IsNull()) continue;
                        output[itr->name.Get<std::string>()] = itr->value.Get<Models::Beatmap>();
                    }
                }

                responseData = std::move(output);
            } catch (BeatSaver::JsonException const& e) {
                responseData = std::nullopt;
                return false;
            }
            return true;
        }
    };

    struct BEATSAVER_PLUSPLUS_EXPORT UserDetailArrayResponse : public WebUtils::GenericResponse<std::vector<Models::UserDetail>> {
        bool AcceptData(std::span<uint8_t const> data) override {
            rapidjson::Document doc;
            doc.Parse((char*)data.data(), data.size());
            if (doc.HasParseError()) return false;
            try {
                BEATSAVER_PLUSPLUS_ERROR_CHECK(doc);
                std::vector<Models::UserDetail> output;
                for (auto& v : doc.GetArray()) {
                    output.emplace_back(v.Get<Models::UserDetail>());
                }

                responseData = std::move(output);
            } catch (BeatSaver::JsonException const& e) {
                responseData = std::nullopt;
                return false;
            }
            return true;
        }
    };

    struct Verify {
        std::optional<std::string> error;
        bool success;
    };

    struct BEATSAVER_PLUSPLUS_EXPORT VerifyResponse : public WebUtils::GenericResponse<Verify> {
        bool AcceptData(std::span<uint8_t const> data) override {
            rapidjson::Document doc;
            doc.Parse((char*)data.data(), data.size());
            if (doc.HasParseError()) return false;
            try {
                Verify output;
                auto memberEnd = doc.MemberEnd();
                auto successItr = doc.FindMember("success");
                if (successItr != memberEnd) {
                    output.success = successItr->value.GetBool();
                }

                auto errorItr = doc.FindMember("error");
                if (errorItr != doc.MemberEnd()) {
                    output.error = errorItr->value.Get<std::string>();
                }

                responseData = std::move(output);
            } catch (BeatSaver::JsonException const& e) {
                responseData = std::nullopt;
                return false;
            }
            return true;
        }
    };

    using VoteResponse = VerifyResponse;

    struct BEATSAVER_PLUSPLUS_EXPORT ListOfVoteSummaryResponse : public WebUtils::GenericResponse<std::vector<Models::VoteSummary>> {
        bool AcceptData(std::span<uint8_t const> data) override {
            rapidjson::Document doc;
            doc.Parse((char*)data.data(), data.size());
            if (doc.HasParseError()) return false;
            try {
                BEATSAVER_PLUSPLUS_ERROR_CHECK(doc);
                std::vector<Models::VoteSummary> output;
                for (auto& v : doc.GetArray()) {
                    output.emplace_back(v.Get<Models::VoteSummary>());
                }

                responseData = std::move(output);
            } catch (BeatSaver::JsonException const& e) {
                responseData = std::nullopt;
                return false;
            }
            return true;
        }
    };

#pragma endregion // responses

    enum class BEATSAVER_PLUSPLUS_EXPORT Filter {
        /// @brief ignore this filter, meaning you get both with & without
        Ignore,
        /// @brief include this filter, meaning you only get with
        Include,
        /// @brief exclude this filter, meaning you only get without
        Exclude,
    };

    using timestamp = std::variant<std::string, std::chrono::time_point<std::chrono::system_clock>>;

    /// @brief function called with an optional T as single arg
    template<typename T>
    using finished_opt_function = std::function<void(std::optional<T>)>;

    using progress_function = std::function<void(float)>;

    template<auto T>
    struct BeatSaverResponse;

    template<auto T>
    using BeatSaverResponse_t = BeatSaverResponse<T>::t;

#define DECLARE_BEATSAVER_RESPONSE_T(func, ...) template<> struct BEATSAVER_PLUSPLUS_EXPORT BeatSaverResponse<&func> { using t = __VA_ARGS__; }

#pragma region maps
    enum class LatestSortOrder {
        FirstPublished,
        Updated,
        LastPublished,
        Created,
        Curated
    };

    /// @brief misc query options for GetLatest request
    struct BEATSAVER_PLUSPLUS_EXPORT LatestQueryOptions {
        /// @brief order by which to sort the latest maps
        std::optional<LatestSortOrder> sortOrder = std::nullopt;
        /// @brief page size for the search
        std::optional<int> pageSize = std::nullopt;
        /// @brief whether to include verified mappers
        Filter verified = Filter::Ignore;
        /// @brief whether to include auto mapped levels
        Filter automapper = Filter::Exclude;

        /// @brief only return maps before the given time
        std::optional<timestamp> before = std::nullopt;
        /// @brief only return maps after the given time
        std::optional<timestamp> after = std::nullopt;

        WebUtils::URLOptions::QueryMap GetQueries() const;
    };

    /// @brief misc query options for GetCollaborationsByUser request
    struct BEATSAVER_PLUSPLUS_EXPORT CollaborationQueryOptions {
        /// @brief only maps made before this timestamp will be returned
        std::optional<timestamp> before = std::nullopt;
        /// @brief size of page returned
        std::optional<int> pageSize = std::nullopt;

        WebUtils::URLOptions::QueryMap GetQueries() const;
    };

    /// @brief creates the necessary url options to download a beatmap info with the provided key
    /// @param key the key to get the info for
    /// @return urloptions to use with webutils, expects a return of BeatSaver::API::BeatmapResponse
    inline WebUtils::URLOptions GetBeatmapByKeyURLOptions(std::string key) {
        return WebUtils::URLOptions{
            fmt::format(BEATSAVER_API_URL "/maps/id/{}", key)
        };
    }

    DECLARE_BEATSAVER_RESPONSE_T(GetBeatmapByKeyURLOptions, BeatmapResponse);

    /// @brief get beatmap by key sync
    /// @param key beatmap key
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return BeatmapResponse
    inline auto GetBeatmapByKey(std::string key, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().Get<BeatSaverResponse_t<&GetBeatmapByKeyURLOptions>>(
            GetBeatmapByKeyURLOptions(key),
            progressReport
        );
    }

    /// @brief get beatmap by key async
    /// @param key beatmap key
    /// @param onFinished method called when request is done void(std::optional<BeatmapResponse>)
    /// @param progressReport method called to report download progress, void(float) 0-1
    inline auto GetBeatmapByKeyAsync(std::string key, finished_opt_function<BeatSaverResponse_t<&GetBeatmapByKeyURLOptions>> onFinished, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetBeatmapByKeyURLOptions>>(
            GetBeatmapByKeyURLOptions(key),
            onFinished,
            progressReport
        );
    }

    /// @brief get beatmap by key async
    /// @param key beatmap key
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return std::future<BeatmapResponse>
    inline auto GetBeatmapByKeyAsync(std::string key, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetBeatmapByKeyURLOptions>>(
            GetBeatmapByKeyURLOptions(key),
            progressReport
        );
    }

    /// @brief creates the necessary url options to download multiple beatmaps infos with the provided keys
    /// @param keys span of keys to get the beatmap infos for
    /// @return urloptions to use with webutils, expects a return of map<std::string (key), BeatSaver::API::BeatmapResponse>
    inline WebUtils::URLOptions GetBeatmapsByKeysURLOptions(std::span<std::string const> keys) {
        // this api call limits you to providing 1-50 ids, so we take a view of the span that limits this
        auto subSpan = keys.subspan(0, std::min<std::size_t>(50, keys.size()));
        return WebUtils::URLOptions{
            fmt::format(BEATSAVER_API_URL "/maps/ids/{}", fmt::join(subSpan, ","))
        };
    }

    DECLARE_BEATSAVER_RESPONSE_T(GetBeatmapsByKeysURLOptions, BeatmapMapResponse);

    /// @brief get multiple beatmaps by keys sync
    /// @param keys the beatmap keys to get info for
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return BeatmapMapResponse
    inline auto GetBeatmapsByKeys(std::span<std::string const> keys, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().Get<BeatSaverResponse_t<&GetBeatmapsByKeysURLOptions>>(
            GetBeatmapsByKeysURLOptions(keys),
            progressReport
        );
    }

    /// @brief get multiple beatmaps by keys async
    /// @param keys the beatmap keys to get info for
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @param onFinished method called when request is done void(std::optional<BeatmapMapResponse>)
    inline auto GetBeatmapsByKeysAsync(std::span<std::string const> keys, finished_opt_function<BeatSaverResponse_t<&GetBeatmapsByKeysURLOptions>> onFinished, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetBeatmapsByKeysURLOptions>>(
            GetBeatmapsByKeysURLOptions(keys),
            onFinished,
            progressReport
        );
    }

    /// @brief get multiple beatmaps by keys async
    /// @param keys the beatmap keys to get info for
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return std::future<BeatmapMapResponse>
    inline auto GetBeatmapsByKeysAsync(std::span<std::string const> keys, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetBeatmapsByKeysURLOptions>>(
            GetBeatmapsByKeysURLOptions(keys),
            progressReport
        );
    }

    /// @brief creates the necessary url options to download a beatmap info with the provided hash
    /// @param hash the hash to get the info for
    /// @return urloptions to use with webutils, expects a return of BeatSaver::API::BeatmapResponse
    inline WebUtils::URLOptions GetBeatmapByHashURLOptions(std::string hash) {
        return WebUtils::URLOptions{
            fmt::format(BEATSAVER_API_URL "/maps/hash/{}", hash)
        };
    }

    DECLARE_BEATSAVER_RESPONSE_T(GetBeatmapByHashURLOptions, BeatmapResponse);

    /// @brief get beatmap by hash sync
    /// @param hash beatmap hash
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return BeatmapResponse
    inline auto GetBeatmapByHash(std::string hash, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().Get<BeatSaverResponse_t<&GetBeatmapByHashURLOptions>>(
            GetBeatmapByHashURLOptions(hash),
            progressReport
        );
    }

    /// @brief get beatmap by hash async
    /// @param hash beatmap hash
    /// @param onFinished method called when request is done void(std::optional<BeatmapResponse>)
    /// @param progressReport method called to report download progress, void(float) 0-1
    inline auto GetBeatmapByHashAsync(std::string hash, finished_opt_function<BeatSaverResponse_t<&GetBeatmapByHashURLOptions>> onFinished, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetBeatmapByHashURLOptions>>(
            GetBeatmapByHashURLOptions(hash),
            onFinished,
            progressReport
        );
    }

    /// @brief get beatmap by hash async
    /// @param hash beatmap hash
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return std::future<BeatmapResponse>
    inline auto GetBeatmapByHashAsync(std::string hash, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetBeatmapByHashURLOptions>>(
            GetBeatmapByHashURLOptions(hash),
            progressReport
        );
    }

    /// @brief creates the necessary url options to download multiple beatmaps infos with the provided hashes
    /// @param hashes span of hashes to get the beatmap infos for
    /// @return urloptions to use with webutils, expects a return of map<std::string (hash), BeatSaver::API::BeatmapResponse>
    inline WebUtils::URLOptions GetBeatmapsByHashesURLOptions(std::span<std::string const> hashes) {
        // this api call limits you to providing 1-50 hashes, so we take a view of the span that limits this
        auto subSpan = hashes.subspan(0, std::min<std::size_t>(50, hashes.size()));
        return WebUtils::URLOptions{
            fmt::format(BEATSAVER_API_URL "/maps/hash/{}", fmt::join(subSpan, ","))
        };
    }

    DECLARE_BEATSAVER_RESPONSE_T(GetBeatmapsByHashesURLOptions, BeatmapMapResponse);

    /// @brief get multiple beatmaps by hashes sync
    /// @param hashes the beatmap hashes to get info for
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return BeatmapMapResponse
    inline auto GetBeatmapsByHashes(std::span<std::string const> hashes, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().Get<BeatSaverResponse_t<&GetBeatmapsByHashesURLOptions>>(
            GetBeatmapsByHashesURLOptions(hashes),
            progressReport
        );
    }

    /// @brief get multiple beatmaps by hashes async
    /// @param hashes the beatmap hashes to get info for
    /// @param onFinished method called when request is done void(std::optional<BeatmapMapResponse>)
    /// @param progressReport method called to report download progress, void(float) 0-1
    inline auto GetBeatmapsByHashesAsync(std::span<std::string const> hashes, finished_opt_function<BeatSaverResponse_t<&GetBeatmapsByHashesURLOptions>> onFinished, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetBeatmapsByHashesURLOptions>>(
            GetBeatmapsByHashesURLOptions(hashes),
            onFinished,
            progressReport
        );
    }

    /// @brief get multiple beatmaps by hashes async
    /// @param hashes the beatmap hashes to get info for
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return std::future<BeatmapMapResponse>
    inline auto GetBeatmapsByHashesAsync(std::span<std::string const> hashes, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetBeatmapsByHashesURLOptions>>(
            GetBeatmapsByHashesURLOptions(hashes),
            progressReport
        );
    }

    /// @brief creates the necessary url options to get a page of maps by the given uploader
    /// @param id the user id to get a page for
    /// @param page the page to get levels for
    /// @return urloptions to use with webutils, expects a return of BeatSaver::API::SearchPageResponse
    inline WebUtils::URLOptions GetBeatmapsByUserURLOptions(int id, int page = 0) {
        return WebUtils::URLOptions{
            fmt::format(BEATSAVER_API_URL "/maps/uploader/{}/{}", id, page)
        };
    }

    DECLARE_BEATSAVER_RESPONSE_T(GetBeatmapsByUserURLOptions, SearchPageResponse);

    /// @brief get beatmaps by user sync
    /// @param id user id
    /// @param page page of info to get
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return SearchPageResponse
    inline auto GetBeatmapsByUser(int id, int page = 0, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().Get<BeatSaverResponse_t<&GetBeatmapsByUserURLOptions>>(
            GetBeatmapsByUserURLOptions(id, page),
            progressReport
        );
    }

    /// @brief get beatmaps by user async
    /// @param id user id
    /// @param onFinished method called when request is done void(std::optional<SearchPageResponse>)
    /// @param page page of info to get
    /// @param progressReport method called to report download progress, void(float) 0-1
    inline auto GetBeatmapsByUserAsync(int id, finished_opt_function<BeatSaverResponse_t<&GetBeatmapsByUserURLOptions>> onFinished, int page = 0, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetBeatmapsByUserURLOptions>>(
            GetBeatmapsByUserURLOptions(id, page),
            onFinished,
            progressReport
        );
    }

    /// @brief get beatmaps by user async
    /// @param id user id
    /// @param page page of info to get
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return std::future<SearchPageResponse>
    inline auto GetBeatmapsByUserAsync(int id, int page = 0, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetBeatmapsByUserURLOptions>>(
            GetBeatmapsByUserURLOptions(id, page),
            progressReport
        );
    }

    /// @brief creates the necessary url options to get a page of collaborations by the given uploader
    /// @param id the user id to get a page for
    /// @param queryOptions misc query options
    /// @return urloptions to use with webutils, expects a return of BeatSaver::API::SearchPageResponse
    inline WebUtils::URLOptions GetCollaborationsByUserURLOptions(int id, CollaborationQueryOptions queryOptions = {}) {
        return WebUtils::URLOptions{
            fmt::format(BEATSAVER_API_URL "/maps/uploader/{}", id),
            queryOptions.GetQueries()
        };
    }

    DECLARE_BEATSAVER_RESPONSE_T(GetCollaborationsByUserURLOptions, SearchPageResponse);

    /// @brief get collaborations by user sync
    /// @param id user id
    /// @param queryOptions options to pass along
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return SearchPageResponse
    inline auto GetCollaborationsByUser(int id, CollaborationQueryOptions queryOptions = {}, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().Get<BeatSaverResponse_t<&GetCollaborationsByUserURLOptions>>(
            GetCollaborationsByUserURLOptions(id, queryOptions),
            progressReport
        );
    }

    /// @brief get collaborations by user async
    /// @param id user id
    /// @param onFinished method called when request is done void(std::optional<SearchPageResponse>)
    /// @param queryOptions options to pass along
    /// @param progressReport method called to report download progress, void(float) 0-1
    inline auto GetCollaborationsByUserAsync(int id, finished_opt_function<BeatSaverResponse_t<&GetCollaborationsByUserURLOptions>> onFinished, CollaborationQueryOptions queryOptions = {}, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetCollaborationsByUserURLOptions>>(
            GetCollaborationsByUserURLOptions(id, queryOptions),
            onFinished,
            progressReport
        );
    }

    /// @brief get collaborations by user async
    /// @param id user id
    /// @param queryOptions options to pass along
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return std::future<SearchPageResponse>
    inline auto GetCollaborationsByUserAsync(int id, CollaborationQueryOptions queryOptions = {}, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetCollaborationsByUserURLOptions>>(
            GetCollaborationsByUserURLOptions(id, queryOptions),
            progressReport
        );
    }

    /// @brief creates the necessary url options to get a page of the latest maps on beatsaver
    /// @param id the user id to get a page for
    /// @param queryOptions misc query options
    /// @return urloptions to use with webutils, expects a return of BeatSaver::API::SearchPageResponse
    inline WebUtils::URLOptions GetLatestURLOptions(LatestQueryOptions queryOptions = {}) {
        return WebUtils::URLOptions{
            fmt::format(BEATSAVER_API_URL "/maps/latest"),
            queryOptions.GetQueries()
        };
    }

    DECLARE_BEATSAVER_RESPONSE_T(GetLatestURLOptions, SearchPageResponse);

    /// @brief get latest beatmaps sync
    /// @param queryOptions misc query options
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return SearchPageResponse
    inline auto GetLatest(LatestQueryOptions queryOptions = {}, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().Get<BeatSaverResponse_t<&GetLatestURLOptions>>(
            GetLatestURLOptions(queryOptions),
            progressReport
        );
    }

    /// @brief get latest beatmaps async
    /// @param onFinished method called when request is done void(std::optional<SearchPageResponse>)
    /// @param queryOptions misc query options
    /// @param progressReport method called to report download progress, void(float) 0-1
    inline auto GetLatestAsync(finished_opt_function<BeatSaverResponse_t<&GetLatestURLOptions>> onFinished, LatestQueryOptions queryOptions = {}, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetLatestURLOptions>>(
            GetLatestURLOptions(queryOptions),
            onFinished,
            progressReport
        );
    }

    /// @brief get latest beatmaps async
    /// @param queryOptions misc query options
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return std::future<SearchPageResponse>
    inline auto GetLatestAsync(LatestQueryOptions queryOptions = {}, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetLatestURLOptions>>(
            GetLatestURLOptions(queryOptions),
            progressReport
        );
    }

    /// @brief creates the necessary url options to get a page of maps on beatsaver ordered by plays
    /// @param page the page to get
    /// @return urloptions to use with webutils, expects a return of BeatSaver::API::SearchPageResponse
    inline WebUtils::URLOptions GetPlaysURLOptions(int page = 0) {
        return WebUtils::URLOptions{
            fmt::format(BEATSAVER_API_URL "/maps/plays/{}", page)
        };
    }

    DECLARE_BEATSAVER_RESPONSE_T(GetPlaysURLOptions, SearchPageResponse);

    /// @brief get beatmaps by plays sync
    /// @param page the page to get the info for
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return SearchPageResponse
    inline auto GetPlays(int page = 0, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().Get<BeatSaverResponse_t<&GetPlaysURLOptions>>(
            GetPlaysURLOptions(page),
            progressReport
        );
    }

    /// @brief get beatmaps by plays async
    /// @param onFinished method called when request is done void(std::optional<SearchPageResponse>)
    /// @param page the page to get the info for
    /// @param progressReport method called to report download progress, void(float) 0-1
    inline auto GetPlaysAsync(finished_opt_function<BeatSaverResponse_t<&GetPlaysURLOptions>> onFinished, int page = 0, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetPlaysURLOptions>>(
            GetPlaysURLOptions(page),
            onFinished,
            progressReport
        );
    }

    /// @brief get beatmaps by plays async
    /// @param page the page to get the info for
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return std::future<SearchPageResponse>
    inline auto GetPlaysAsync(int page = 0, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetPlaysURLOptions>>(
            GetPlaysURLOptions(page),
            progressReport
        );
    }

#pragma endregion // maps

#pragma region users
    /// @brief creates the necessary url options to get a users details
    /// @param id the user id to get the details for
    /// @return urloptions to use with webutils, expects a return of BeatSaver::API::UserDetailResponse
    inline WebUtils::URLOptions GetUserByIdURLOptions(int id) {
        return WebUtils::URLOptions{
            fmt::format(BEATSAVER_API_URL "/users/id/{}", id)
        };
    }

    DECLARE_BEATSAVER_RESPONSE_T(GetUserByIdURLOptions, UserDetailResponse);

    /// @brief get user by id sync
    /// @param id the user id to get info for
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return UserDetailResponse
    inline auto GetUserById(int id, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().Get<BeatSaverResponse_t<&GetUserByIdURLOptions>>(
            GetUserByIdURLOptions(id),
            progressReport
        );
    }

    /// @brief get user by id async
    /// @param id the user id to get info for
    /// @param onFinished method called when request is done void(std::optional<UserDetailResponse>)
    /// @param progressReport method called to report download progress, void(float) 0-1
    inline auto GetUserByIdAsync(int id, finished_opt_function<BeatSaverResponse_t<&GetUserByIdURLOptions>> onFinished, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetUserByIdURLOptions>>(
            GetUserByIdURLOptions(id),
            onFinished,
            progressReport
        );
    }

    /// @brief get user by id async
    /// @param id the user id to get info for
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return std::future<UserDetailResponse>
    inline auto GetUserByIdAsync(int id, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetUserByIdURLOptions>>(
            GetUserByIdURLOptions(id),
            progressReport
        );
    }

    /// @brief creates the necessary url options to get multiple users details in one request
    /// @param ids the user ids to get the details for
    /// @return urloptions to use with webutils, expects a return of array<BeatSaver::API::UserDetailResponse>
    inline WebUtils::URLOptions GetUsersByIdsURLOptions(std::span<int const> ids) {
        auto subSpan = ids.subspan(0, std::min<std::size_t>(50, ids.size()));
        return WebUtils::URLOptions{
            fmt::format(BEATSAVER_API_URL "/users/ids/{}", fmt::join(subSpan, ","))
        };
    }

    DECLARE_BEATSAVER_RESPONSE_T(GetUsersByIdsURLOptions, UserDetailArrayResponse);

    /// @brief get users by ids sync
    /// @param ids the user ids to get info for
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return UserDetailArrayResponse
    inline auto GetUsersByIds(std::span<int const> ids, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().Get<BeatSaverResponse_t<&GetUsersByIdsURLOptions>>(
            GetUsersByIdsURLOptions(ids),
            progressReport
        );
    }

    /// @brief get users by ids async
    /// @param ids the user ids to get info for
    /// @param onFinished method called when request is done void(std::optional<UserDetailArrayResponse>)
    /// @param progressReport method called to report download progress, void(float) 0-1
    inline auto GetUsersByIdsAsync(std::span<int const> ids, finished_opt_function<BeatSaverResponse_t<&GetUsersByIdsURLOptions>> onFinished, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetUsersByIdsURLOptions>>(
            GetUsersByIdsURLOptions(ids),
            onFinished,
            progressReport
        );
    }

    /// @brief get users by ids async
    /// @param ids the user ids to get info for
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return std::future<UserDetailArrayResponse>
    inline auto GetUsersByIdsAsync(std::span<int const> ids, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetUsersByIdsURLOptions>>(
            GetUsersByIdsURLOptions(ids),
            progressReport
        );
    }

    /// @brief creates the necessary url options to get a users details
    /// @param userName the name to get the details for
    /// @return urloptions to use with webutils, expects a return of BeatSaver::API::UserDetailResponse
    inline WebUtils::URLOptions GetUserByNameURLOptions(std::string userName) {
        return WebUtils::URLOptions{
            fmt::format(BEATSAVER_API_URL "/users/name/{}", userName)
        };
    }

    DECLARE_BEATSAVER_RESPONSE_T(GetUserByNameURLOptions, UserDetailResponse);

    /// @brief get user by username sync
    /// @param userName the username to get the user for
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return UserDetailResponse
    inline auto GetUserByName(std::string userName, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().Get<BeatSaverResponse_t<&GetUserByNameURLOptions>>(
            GetUserByNameURLOptions(userName),
            progressReport
        );
    }

    /// @brief get user by username async
    /// @param userName the username to get the user for
    /// @param onFinished method called when request is done void(std::optional<UserDetailResponse>)
    /// @param progressReport method called to report download progress, void(float) 0-1
    inline auto GetUserByNameAsync(std::string userName, finished_opt_function<BeatSaverResponse_t<&GetUserByNameURLOptions>> onFinished, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetUserByNameURLOptions>>(
            GetUserByNameURLOptions(userName),
            onFinished,
            progressReport
        );
    }

    /// @brief get user by username async
    /// @param userName the username to get the user for
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return std::future<UserDetailResponse>
    inline auto GetUserByNameAsync(std::string userName, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetUserByNameURLOptions>>(
            GetUserByNameURLOptions(userName),
            progressReport
        );
    }

    /// @brief creates the necessary url options to download avatar image data
    /// @param version the beatmap version to create the url options for
    /// @return urloptions to use with webutils, expects a return of WebUtils::DataResponse, though if bsml is used WebUtils::SpriteResponse or WebUtils::TextureResponse may also be used
    inline WebUtils::URLOptions GetAvatarImageURLOptions(Models::UserDetail const& userDetail) {
        return WebUtils::URLOptions {
            userDetail.AvatarURL
        };
    }

    DECLARE_BEATSAVER_RESPONSE_T(GetAvatarImageURLOptions, WebUtils::DataResponse);

    /// @brief get avatar image sync
    /// @param userDetail the user to get the avatar image for
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return DataResponse
    inline auto GetAvatarImage(Models::UserDetail const& userDetail, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().Get<BeatSaverResponse_t<&GetAvatarImageURLOptions>>(
            GetAvatarImageURLOptions(userDetail),
            progressReport
        );
    }

    /// @brief get avatar image async
    /// @param userDetail the user to get the avatar image for
    /// @param onFinished method called when request is done void(std::optional<DataResponse>)
    /// @param progressReport method called to report download progress, void(float) 0-1
    inline auto GetAvatarImageAsync(Models::UserDetail const& userDetail, finished_opt_function<BeatSaverResponse_t<&GetAvatarImageURLOptions>> onFinished, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetAvatarImageURLOptions>>(
            GetAvatarImageURLOptions(userDetail),
            onFinished,
            progressReport
        );
    }

    /// @brief get avatar image async
    /// @param userDetail the user to get the avatar image for
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return std::future<DataResponse>
    inline auto GetAvatarImageAsync(Models::UserDetail const& userDetail, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetAvatarImageURLOptions>>(
            GetAvatarImageURLOptions(userDetail),
            progressReport
        );
    }

    /// @brief enum to determine the platform to use for the request
    enum class UserPlatform {
        Oculus,
        Steam
    };

    /// @brief struct containing auth information to use in certain requests
    struct PlatformAuth {
        /// @param platform the platform for which to verify with the user id
        UserPlatform platform;
        /// @param userId applicable user id
        std::string userId;
        /// @param proof userproof
        std::string proof;

        /// @brief serializes the struct as a json object
        static rapidjson::Value& Serialize(PlatformAuth const& instance, rapidjson::Value& json, rapidjson::Value::AllocatorType& allocator);

        /// @brief serializes the struct as a json string
        std::string SerializeToString() const;
    };

    /// @brief creates the neccesary url options and data to post to the endpoint
    /// @return pair of webutils url options and data to send with the post request, expects a return of BeatSaver::API::VerifyResponse
    inline std::pair<WebUtils::URLOptions, std::string> PostVerifyURLOptionsAndData(PlatformAuth auth) {
        return {
            WebUtils::URLOptions {
                BEATSAVER_API_URL "/users/verify",
                WebUtils::URLOptions::QueryMap{},
                WebUtils::URLOptions::HeaderMap{{"Content-Type", "application/json"}}
            },
            auth.SerializeToString()
        };
    }

    DECLARE_BEATSAVER_RESPONSE_T(PostVerifyURLOptionsAndData, VerifyResponse);

    /// @brief post verification request sync
    /// @param auth user authorization
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return VerifyResponse
    inline auto PostVerify(PlatformAuth auth, progress_function progressReport = nullptr) {
        auto [url, data] = PostVerifyURLOptionsAndData(auth);
        return GetBeatsaverDownloader().Post<BeatSaverResponse_t<&PostVerifyURLOptionsAndData>>(
            url,
            {(uint8_t*)data.data(), data.size()},
            progressReport
        );
    }

    /// @brief post verification request async
    /// @param auth user authorization
    /// @param onFinished method called when request is done void(std::optional<VerifyResponse>)
    /// @param progressReport method called to report download progress, void(float) 0-1
    inline auto PostVerifyAsync(PlatformAuth auth, finished_opt_function<BeatSaverResponse_t<&PostVerifyURLOptionsAndData>> onFinished, progress_function progressReport = nullptr) {
        auto [url, data] = PostVerifyURLOptionsAndData(auth);
        // post requests are a bit special because we need to be able to keep variables in scope
        std::thread([](auto url, auto data, auto onFinished, auto progressReport) -> auto {
            onFinished(
                GetBeatsaverDownloader().Post<BeatSaverResponse_t<&PostVerifyURLOptionsAndData>>(
                    url,
                    {(uint8_t*)data.data(), data.size()},
                    progressReport
                )
            );
        }, url, data, onFinished, progressReport).detach();
    }

    /// @brief post verification request async
    /// @param auth user authorization
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return std::future<VerifyResponse>
    inline auto PostVerifyAsync(PlatformAuth auth, progress_function progressReport = nullptr) {
        auto [url, data] = PostVerifyURLOptionsAndData(auth);
        // post requests are a bit special because we need to be able to keep variables in scope
        return std::async([](auto url, auto data, auto progressReport) -> auto {
            return GetBeatsaverDownloader().Post<BeatSaverResponse_t<&PostVerifyURLOptionsAndData>>(
                url,
                {(uint8_t*)data.data(), data.size()},
                progressReport
            );
        }, url, data, progressReport);
    }
#pragma endregion // users

#pragma region search
    enum class BEATSAVER_PLUSPLUS_EXPORT SearchSortOrder {
        Latest,
        Relevance,
        Rating,
        Curated
    };

    struct BEATSAVER_PLUSPLUS_EXPORT SearchQueryOptions {
        /// @brief query to filter on
        std::optional<std::string> query = std::nullopt;
        /// @brief page index of the results
        std::optional<int> pageIndex = std::nullopt;
        /// @brief order by which to sort
        SearchSortOrder sortOrder = SearchSortOrder::Latest;
        /// @brief whether to include automapper in the results
        Filter automapper = Filter::Exclude;
        /// @brief whether to include chroma in the results
        Filter chroma = Filter::Ignore;
        /// @brief whether to include noodle in the results
        Filter noodle = Filter::Ignore;
        /// @brief whether to include mapping ext in the results
        Filter me = Filter::Ignore;
        /// @brief whether to include cinema in the results
        Filter cinema = Filter::Ignore;
        /// @brief whether to include ranked in the results
        Filter ranked = Filter::Ignore;
        /// @brief whether to include verified in the results
        Filter verified = Filter::Ignore;
        /// @brief whether to include fullspread in the results
        Filter fullspread = Filter::Ignore;
        /// @brief start of time filter
        std::optional<timestamp> from = std::nullopt;
        /// @brief end of time filter
        std::optional<timestamp> to = std::nullopt;
        /// @brief tags to include in the search
        std::vector<std::string> includeTags = {};
        /// @brief tags to exclude in the search
        std::vector<std::string> excludeTags = {};
        /// @brief max bpm allowed
        std::optional<float> maxBpm = std::nullopt;
        /// @brief min bpm allowed
        std::optional<float> minBpm = std::nullopt;
        /// @brief max length allowed
        std::optional<int> maxDuration = std::nullopt;
        /// @brief min length allowed
        std::optional<int> minDuration = std::nullopt;
        /// @brief max nps allowed
        std::optional<float> maxNps = std::nullopt;
        /// @brief min nps allowed
        std::optional<float> minNps = std::nullopt;
        /// @brief max rating allowed
        std::optional<float> maxRating = std::nullopt;
        /// @brief min rating allowed
        std::optional<float> minRating = std::nullopt;

        WebUtils::URLOptions::QueryMap GetQueries() const;
    };

    /// @brief creates the necessary url options to search for maps
    /// @param page the page to go to for the result
    /// @param queryOptions misc query options for the search
    /// @return urloptions to use with webutils, expects a return of BeatSaver::API::SearchPageResponse
    inline WebUtils::URLOptions GetPageURLOptions(int page, SearchQueryOptions queryOptions = {}) {
        return WebUtils::URLOptions {
            fmt::format(BEATSAVER_API_URL "/search/text/{}", page),
            queryOptions.GetQueries()
        };
    }

    DECLARE_BEATSAVER_RESPONSE_T(GetPageURLOptions, SearchPageResponse);

    /// @brief get search page sync
    /// @param page page number to get
    /// @param queryOptions misc query options
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return SearchPageResponse
    inline auto GetPage(int page, SearchQueryOptions queryOptions = {}, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().Get<BeatSaverResponse_t<&GetPageURLOptions>>(
            GetPageURLOptions(page, queryOptions),
            progressReport
        );
    }

    /// @brief get search page async
    /// @param page page number to get
    /// @param onFinished method called when request is done void(std::optional<SearchPageResponse>)
    /// @param queryOptions misc query options
    /// @param progressReport method called to report download progress, void(float) 0-1
    inline auto GetPageAsync(int page, finished_opt_function<BeatSaverResponse_t<&GetPageURLOptions>> onFinished, SearchQueryOptions queryOptions = {}, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetPageURLOptions>>(
            GetPageURLOptions(page, queryOptions),
            onFinished,
            progressReport
        );
    }

    /// @brief get search page async
    /// @param page page number to get
    /// @param queryOptions misc query options
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return std::future<SearchPageResponse>
    inline auto GetPageAsync(int page, SearchQueryOptions queryOptions = {}, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetPageURLOptions>>(
            GetPageURLOptions(page, queryOptions),
            progressReport
        );
    }

    /// @brief provides a readonly span of some map feel tags that are available on beatsaver. these tags were pulled at a moment in time and are not guaranteed to still exist
    BEATSAVER_PLUSPLUS_EXPORT std::span<const std::string> GetMapFeelTags();

    /// @brief provides a readonly span of some genre tags that are available on beatsaver. these tags were pulled at a moment in time and are not guaranteed to still exist
    BEATSAVER_PLUSPLUS_EXPORT std::span<const std::string> GetGenreTags();

#pragma endregion // search

#pragma region vote
    struct VoteQueryOptions {
        /// @brief get vote information after this timestamp
        std::optional<timestamp> since = std::nullopt;

        WebUtils::URLOptions::QueryMap GetQueries() const;
    };

    /// @brief creates the necessary url options to get vote information from beatsaver
    /// @param queryOptions misc query options for the request
    /// @return urloptions to use with webutils, expects a return of BeatSaver::API::ListOfVoteSummaryResponse
    inline WebUtils::URLOptions GetVoteURLOptions(VoteQueryOptions queryOptions = {}) {
        return WebUtils::URLOptions {
            BEATSAVER_API_URL "/vote",
            queryOptions.GetQueries()
        };
    }

    DECLARE_BEATSAVER_RESPONSE_T(GetVoteURLOptions, ListOfVoteSummaryResponse);

    /// @brief get vote info sync
    /// @param queryOptions misc query options
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return ListOfVoteSummaryResponse
    inline auto GetVote(VoteQueryOptions queryOptions = {}, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().Get<BeatSaverResponse_t<&GetVoteURLOptions>>(
            GetVoteURLOptions(queryOptions),
            progressReport
        );
    }

    /// @brief get vote info async
    /// @param onFinished method called when request is done void(std::optional<ListOfVoteSummaryResponse>)
    /// @param queryOptions misc query options
    /// @param progressReport method called to report download progress, void(float) 0-1
    inline auto GetVoteAsync(finished_opt_function<BeatSaverResponse_t<&GetVoteURLOptions>> onFinished, VoteQueryOptions queryOptions = {}, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetVoteURLOptions>>(
            GetVoteURLOptions(queryOptions),
            onFinished,
            progressReport
        );
    }

    /// @brief get vote info async
    /// @param queryOptions misc query options
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return std::future<ListOfVoteSummaryResponse>
    inline auto GetVoteAsync(VoteQueryOptions queryOptions = {}, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetVoteURLOptions>>(
            GetVoteURLOptions(queryOptions),
            progressReport
        );
    }

    /// @brief creates the body data for a vote submission
    std::string BEATSAVER_PLUSPLUS_EXPORT CreateVoteData(PlatformAuth auth, bool direction, std::string hash);

    /// @brief creates the necessary url options to post vote information to beatsaver
    /// @param auth the auth for the request
    /// @param direction whether this is an up or down vote
    /// @param hash the hash of the map to vote for
    /// @return urloptions to use with webutils and data to send, expects a return of BeatSaver::API::VoteResponse
    inline std::pair<WebUtils::URLOptions, std::string> PostVoteURLOptionsAndData(PlatformAuth auth, bool direction, std::string hash) {
        return {
            WebUtils::URLOptions {
                BEATSAVER_API_URL "/vote",
                WebUtils::URLOptions::QueryMap{},
                WebUtils::URLOptions::HeaderMap{{"Content-Type", "application/json"}}
            },
            CreateVoteData(auth, direction, hash)
        };
    }

    DECLARE_BEATSAVER_RESPONSE_T(PostVoteURLOptionsAndData, VoteResponse);

    /// @brief post a vote sync
    /// @param auth the auth for the request
    /// @param direction whether this is an up or down vote
    /// @param hash the hash of the map to vote for
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return VoteResponse
    inline auto PostVote(PlatformAuth auth, bool direction, std::string hash, progress_function progressReport = nullptr) {
        auto [url, data] = PostVoteURLOptionsAndData(auth, direction, hash);
        return GetBeatsaverDownloader().Post<BeatSaverResponse_t<&PostVoteURLOptionsAndData>>(
            url,
            {(uint8_t*)data.data(), data.size()},
            progressReport
        );
    }

    /// @brief post a vote async
    /// @param auth the auth for the request
    /// @param direction whether this is an up or down vote
    /// @param hash the hash of the map to vote for
    /// @param onFinished method called when request is done void(std::optional<VoteResponse>)
    /// @param progressReport method called to report download progress, void(float) 0-1
    inline auto PostVoteAsync(PlatformAuth auth, bool direction, std::string hash, finished_opt_function<BeatSaverResponse_t<&PostVoteURLOptionsAndData>> onFinished, progress_function progressReport = nullptr) {
        auto [url, data] = PostVoteURLOptionsAndData(auth, direction, hash);
        // post requests are a bit special because we need to be able to keep variables in scope
        std::thread([](auto url, auto data, auto onFinished, auto progressReport) -> auto {
            onFinished(
                GetBeatsaverDownloader().Post<BeatSaverResponse_t<&PostVoteURLOptionsAndData>>(
                    url,
                    {(uint8_t*)data.data(), data.size()},
                    progressReport
                )
            );
        }, url, data, onFinished, progressReport).detach();
    }

    /// @brief post a vote async
    /// @param auth the auth for the request
    /// @param direction whether this is an up or down vote
    /// @param hash the hash of the map to vote for
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return std::future<VoteResponse>
    inline auto PostVoteAsync(PlatformAuth auth, bool direction, std::string hash, progress_function progressReport = nullptr) {
        auto [url, data] = PostVoteURLOptionsAndData(auth, direction, hash);
        // post requests are a bit special because we need to be able to keep variables in scope
        return std::async([](auto url, auto data, auto progressReport) -> auto {
            return GetBeatsaverDownloader().Post<BeatSaverResponse_t<&PostVoteURLOptionsAndData>>(
                url,
                {(uint8_t*)data.data(), data.size()},
                progressReport
            );
        }, url, data, progressReport);
    }

#pragma endregion // vote

#pragma region playlists
    enum class BEATSAVER_PLUSPLUS_EXPORT LatestPlaylistSortOrder {
        Updated,
        SongsUpdated,
        Created,
        Curated
    };

    /// @brief Query options for latest playlist retrieval
    struct BEATSAVER_PLUSPLUS_EXPORT LatestPlaylistsQueryOptions {
        /// @brief filter for playlists after this timestamp
        std::optional<timestamp> after = std::nullopt;
        /// @brief filter for playlists before this timestamp
        std::optional<timestamp> before = std::nullopt;
        /// @brief change the amount of responses
        std::optional<int> pageSize = std::nullopt;
        /// @brief which sorting mode to use
        std::optional<LatestPlaylistSortOrder> sort = std::nullopt;

        WebUtils::URLOptions::QueryMap GetQueries() const;
    };

    /// @brief creates the necessary url options to browse the latest playlists
    /// @param queryOptions misc query options for the request
    /// @return urloptions to use with webutils, expects a return of BeatSaver::API::PlaylistSearchPageResponse
    inline WebUtils::URLOptions GetLatestPlaylistsURLOptions(LatestPlaylistsQueryOptions queryOptions = {}) {
        return WebUtils::URLOptions {
            fmt::format(BEATSAVER_API_URL "/playlists/latest"),
            queryOptions.GetQueries()
        };
    }

    DECLARE_BEATSAVER_RESPONSE_T(GetLatestPlaylistsURLOptions, PlaylistSearchPageResponse);

    /// @brief get latest playlists sync
    /// @param queryOptions misc query options
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return PlaylistSearchPageResponse
    inline auto GetLatestPlaylists(LatestPlaylistsQueryOptions queryOptions = {}, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().Get<BeatSaverResponse_t<&GetLatestPlaylistsURLOptions>>(
            GetLatestPlaylistsURLOptions(queryOptions),
            progressReport
        );
    }

    /// @brief get latest playlists async
    /// @param onFinished method called when request is done void(std::optional<PlaylistSearchPageResponse>)
    /// @param queryOptions misc query options
    /// @param progressReport method called to report download progress, void(float) 0-1
    inline auto GetLatestPlaylistsAsync(finished_opt_function<BeatSaverResponse_t<&GetLatestPlaylistsURLOptions>> onFinished, LatestPlaylistsQueryOptions queryOptions = {}, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetLatestPlaylistsURLOptions>>(
            GetLatestPlaylistsURLOptions(queryOptions),
            onFinished,
            progressReport
        );
    }

    /// @brief get latest playlists async
    /// @param queryOptions misc query options
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return std::future<PlaylistSearchPageResponse>
    inline auto GetLatestPlaylistsAsync(LatestPlaylistsQueryOptions queryOptions = {}, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetLatestPlaylistsURLOptions>>(
            GetLatestPlaylistsURLOptions(queryOptions),
            progressReport
        );
    }

    enum class SearchPlaylistSortOrder {
        Latest,
        Relevance,
        Rating,
        Curated
    };

    /// @brief Query options for playlist searches
    struct SearchPlaylistsQueryOptions {
        /// @brief query to filter with
        std::optional<std::string> query = std::nullopt;
        /// @brief sorting order for the search
        SearchPlaylistSortOrder sortOrder = SearchPlaylistSortOrder::Latest;
        /// @brief whether to include curated maps in the results
        Filter curated = Filter::Ignore;
        /// @brief whether to include maps by verified users in the results
        Filter verified = Filter::Ignore;
        /// @brief whether to include empty playlists in the results
        std::optional<bool> includeEmpty = std::nullopt;
        /// @brief filter for playlists after this timestamp
        std::optional<timestamp> from = std::nullopt;
        /// @brief filter for playlists before this timestamp
        std::optional<timestamp> to = std::nullopt;
        /// @brief require a max nps to be filtered on
        std::optional<float> maxNps = std::nullopt;
        /// @brief require a min nps to be filtered on
        std::optional<float> minNps = std::nullopt;

        WebUtils::URLOptions::QueryMap GetQueries() const;
    };

    /// @brief creates the necessary url options to search in playlists
    /// @param page page to get
    /// @param queryOptions misc query options for the request
    /// @return urloptions to use with webutils, expects a return of BeatSaver::API::PlaylistSearchPageResponse
    inline WebUtils::URLOptions GetSearchPlaylistsURLOptions(int page = 0, SearchPlaylistsQueryOptions queryOptions = {}) {
        return WebUtils::URLOptions {
            fmt::format(BEATSAVER_API_URL "/playlists/search/{}", page),
            queryOptions.GetQueries()
        };
    }

    DECLARE_BEATSAVER_RESPONSE_T(GetSearchPlaylistsURLOptions, PlaylistSearchPageResponse);

    /// @brief search playlists sync
    /// @param page page of search results to get
    /// @param queryOptions misc query options
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return PlaylistSearchPageResponse
    inline auto GetSearchPlaylists(int page = 0, SearchPlaylistsQueryOptions queryOptions = {}, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().Get<BeatSaverResponse_t<&GetSearchPlaylistsURLOptions>>(
            GetSearchPlaylistsURLOptions(page, queryOptions),
            progressReport
        );
    }

    /// @brief search playlists async
    /// @param onFinished method called when request is done void(std::optional<PlaylistSearchPageResponse>)
    /// @param page page of search results to get
    /// @param queryOptions misc query options
    /// @param progressReport method called to report download progress, void(float) 0-1
    inline auto GetSearchPlaylistsAsync(finished_opt_function<BeatSaverResponse_t<&GetSearchPlaylistsURLOptions>> onFinished, int page = 0, SearchPlaylistsQueryOptions queryOptions = {}, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetSearchPlaylistsURLOptions>>(
            GetSearchPlaylistsURLOptions(page, queryOptions),
            onFinished,
            progressReport
        );
    }

    /// @brief search playlists async
    /// @param page page of search results to get
    /// @param queryOptions misc query options
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return std::future<PlaylistSearchPageResponse>
    inline auto GetSearchPlaylistsAsync(int page = 0, SearchPlaylistsQueryOptions queryOptions = {}, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetSearchPlaylistsURLOptions>>(
            GetSearchPlaylistsURLOptions(page, queryOptions),
            progressReport
        );
    }

    /// @brief creates the necessary url options to get the playlists for a certain user
    /// @param userID the user to get playlists for
    /// @param page the page to get
    /// @return urloptions to use with webutils and data to send, expects a return of BeatSaver::API::PlaylistSearchPageResponse
    inline WebUtils::URLOptions GetUserPlaylistsURLOptions(int userID, int page = 0) {
        return WebUtils::URLOptions {
            fmt::format(BEATSAVER_API_URL "/playlists/user/{}/{}", userID, page)
        };
    }

    DECLARE_BEATSAVER_RESPONSE_T(GetUserPlaylistsURLOptions, PlaylistSearchPageResponse);

    /// @brief get playlists for user sync
    /// @param userID the user to get playlists for
    /// @param page page of search results to get
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return PlaylistSearchPageResponse
    inline auto GetUserPlaylists(int userID, int page = 0, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().Get<BeatSaverResponse_t<&GetUserPlaylistsURLOptions>>(
            GetUserPlaylistsURLOptions(userID, page),
            progressReport
        );
    }

    /// @brief get playlists for user async
    /// @param userID the user to get playlists for
    /// @param onFinished method called when request is done void(std::optional<PlaylistSearchPageResponse>)
    /// @param page page of search results to get
    /// @param progressReport method called to report download progress, void(float) 0-1
    inline auto GetUserPlaylistsAsync(int userID, finished_opt_function<BeatSaverResponse_t<&GetUserPlaylistsURLOptions>> onFinished, int page = 0, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetUserPlaylistsURLOptions>>(
            GetUserPlaylistsURLOptions(userID, page),
            onFinished,
            progressReport
        );
    }

    /// @brief get playlists for user async
    /// @param userID the user to get playlists for
    /// @param page page of search results to get
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return std::future<PlaylistSearchPageResponse>
    inline auto GetUserPlaylistsAsync(int userID, int page = 0, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetUserPlaylistsURLOptions>>(
            GetUserPlaylistsURLOptions(userID, page),
            progressReport
        );
    }

    /// @brief creates the necessary url options to get the info for a specific playlist
    /// @param playlistID the id of the playlist
    /// @param page the page of beatmaps to get from the playlist info
    /// @return urloptions to use with webutils and data to send, expects a return of BeatSaver::API::PlaylistPageResponse
    inline WebUtils::URLOptions GetPlaylistURLOptions(int playlistID, int page = 0) {
        return WebUtils::URLOptions {
            fmt::format(BEATSAVER_API_URL "/playlists/id/{}/{}", playlistID, page)
        };
    }

    DECLARE_BEATSAVER_RESPONSE_T(GetPlaylistURLOptions, PlaylistPageResponse);

    /// @brief get playlist info sync
    /// @param playlistID the playlist to get info for
    /// @param page page of search results to get
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return PlaylistSearchPageResponse
    inline auto GetPlaylist(int playlistID, int page = 0, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().Get<BeatSaverResponse_t<&GetPlaylistURLOptions>>(
            GetPlaylistURLOptions(playlistID, page),
            progressReport
        );
    }

    /// @brief get playlist info async
    /// @param playlistID the playlist to get info for
    /// @param onFinished method called when request is done void(std::optional<PlaylistSearchPageResponse>)
    /// @param page page of search results to get
    /// @param progressReport method called to report download progress, void(float) 0-1
    inline auto GetPlaylistAsync(int playlistID, finished_opt_function<BeatSaverResponse_t<&GetPlaylistURLOptions>> onFinished, int page = 0, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetPlaylistURLOptions>>(
            GetPlaylistURLOptions(playlistID, page),
            onFinished,
            progressReport
        );
    }

    /// @brief get playlist info async
    /// @param playlistID the playlist to get info for
    /// @param page page of search results to get
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return std::future<PlaylistSearchPageResponse>
    inline auto GetPlaylistAsync(int playlistID, int page = 0, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetPlaylistURLOptions>>(
            GetPlaylistURLOptions(playlistID, page),
            progressReport
        );
    }

#pragma endregion // playlists

#pragma region download
    /// @brief helper struct containing the information for a map download
    struct BEATSAVER_PLUSPLUS_EXPORT BeatmapDownloadInfo {
        /// @brief sanitizes the folder name for the beatmap download
        static std::string SanitizeFolderName(std::string_view foldername);
        /// @brief formats and then sanitizes the folder name for the beatmap download
        static std::string SanitizeFolderName(std::string_view key, std::string_view songname, std::string_view authorname) {
            return SanitizeFolderName(fmt::format("{} ({} - {})", key, songname, authorname));
        }

        BeatmapDownloadInfo() = default;
        /// @brief info from direct values
        BeatmapDownloadInfo(std::string Key, std::string DownloadURL, std::string FolderName) : Key(Key), DownloadURL(DownloadURL), FolderName(SanitizeFolderName(FolderName)) {}
        /// @brief info from a beatmap, gets the front version to download
        BeatmapDownloadInfo(Models::Beatmap const& beatmap) : BeatmapDownloadInfo(beatmap, beatmap.Versions.front()) {}
        /// @brief info from a beatmap and version
        BeatmapDownloadInfo(Models::Beatmap const& beatmap, Models::BeatmapVersion const& version) : Key(version.Key.value_or(beatmap.Id)), DownloadURL(version.DownloadURL), FolderName(SanitizeFolderName(Key, beatmap.Metadata.SongName, beatmap.Metadata.LevelAuthorName)) {}

        /// @brief beatmap key, /beatmaps/ids/{key}
        std::string const Key;
        /// @brief download url, where the map zip is at
        std::string const DownloadURL;
        /// @brief folder name, the name to use for the folder when saving the map to disk
        std::string const FolderName;
    };

    /// @brief response to be used with webutils, can only be used with GetInto due to requiring to know where to unzip the file
    struct BEATSAVER_PLUSPLUS_EXPORT DownloadBeatmapResponse : public WebUtils::GenericResponse<std::filesystem::path> {
        DownloadBeatmapResponse(BeatmapDownloadInfo const& info) : info(info) {}
        BeatmapDownloadInfo const info;

        virtual bool AcceptData(std::span<const uint8_t> data) override;
    };

    static_assert(!std::is_default_constructible_v<DownloadBeatmapResponse>, "DownloadBeatmapResponse can't be default constructible!");

    /// @brief request to be used with webutils, should be used with the RatelimitedDispatcher
    struct BEATSAVER_PLUSPLUS_EXPORT DownloadBeatmapRequest : public WebUtils::IRequest {
        WebUtils::URLOptions url;
        DownloadBeatmapResponse response;

        DownloadBeatmapRequest(BeatmapDownloadInfo const& downloadInfo) : url(downloadInfo.DownloadURL), response(downloadInfo) {}
        virtual ~DownloadBeatmapRequest() override = default;

        virtual WebUtils::URLOptions const& get_URL() const override { return url; };
        virtual WebUtils::IResponse* get_TargetResponse() override { return &response; };
        virtual WebUtils::IResponse const* get_TargetResponse() const override { return &response; }

        __declspec(property(get=get_TargetResponse)) WebUtils::IResponse* TargetResponse;
        __declspec(property(get=get_URL)) WebUtils::URLOptions const& URL;
    };

    static_assert(!std::is_default_constructible_v<DownloadBeatmapRequest>, "DownloadBeatmapRequest can't be default constructible!");

    /// @brief creates both the url options and target beatmap response for the given download info
    /// @param info download info for a map
    /// @return urloptions to use in web utils, as well as a download beatmap response to use to automatically unzip the map into the configured path
    inline std::pair<WebUtils::URLOptions, DownloadBeatmapResponse> DownloadBeatmapURLOptionsAndResponse(BeatmapDownloadInfo info) {
        return {
            WebUtils::URLOptions(info.DownloadURL),
            DownloadBeatmapResponse(info)
        };
    }

    /// @brief creates a download request for use with the ratelimited dispatcher from webutils
    /// @param info the download info for which to create the request
    inline std::unique_ptr<DownloadBeatmapRequest> CreateDownloadBeatmapRequest(BeatmapDownloadInfo info) {
        return std::make_unique<DownloadBeatmapRequest>(info);
    }

    /// @brief method to download a beatmap synchronously
    /// @param info the download info for the map to download
    /// @param progressReport callback for reporting progress
    /// @return optional path, if set the download was succesful and the map can be found @ that path, nullopt if failed
    inline std::optional<std::filesystem::path> DownloadBeatmap(BeatmapDownloadInfo info, std::function<void(float)> progressReport = nullptr) {
        auto [options, response] = DownloadBeatmapURLOptionsAndResponse(info);
        GetBeatsaverDownloader().GetInto(options, &response, progressReport);
        return response.responseData;
    }

    /// @brief downloads a beatmap asynchronously, and calls onFinished after it concludes
    /// @param info the download info for the map to download
    /// @param onFinished the method to call once the request is done
    /// @param progressReport callback for reporting progress
    inline void DownloadBeatmapAsync(BeatmapDownloadInfo info, finished_opt_function<std::filesystem::path> onFinished, std::function<void(float)> progressReport = nullptr) {
        if (!onFinished) return;

        std::thread([](BeatmapDownloadInfo info, finished_opt_function<std::filesystem::path> onFinished, std::function<void(float)> progressReport){
            onFinished(DownloadBeatmap(info, progressReport));
        }, std::forward<BeatmapDownloadInfo>(info), std::forward<finished_opt_function<std::filesystem::path>>(onFinished), std::forward<std::function<void(float)>>(progressReport)).detach();
    }

    /// @brief creates the necessary url options to download cover image data
    /// @param version the beatmap version to create the url options for
    /// @return urloptions to use with webutils, expects a return of WebUtils::DataResponse, though if bsml is used WebUtils::SpriteResponse or WebUtils::TextureResponse may also be used
    inline WebUtils::URLOptions GetCoverImageURLOptions(Models::BeatmapVersion const& version) {
        return WebUtils::URLOptions {
            version.CoverURL
        };
    }

    DECLARE_BEATSAVER_RESPONSE_T(GetCoverImageURLOptions, WebUtils::DataResponse);

    /// @brief get cover image sync
    /// @param version the beatmap version to get the cover image for
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return DataResponse
    inline auto GetCoverImage(Models::BeatmapVersion const& version, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().Get<BeatSaverResponse_t<&GetCoverImageURLOptions>>(
            GetCoverImageURLOptions(version),
            progressReport
        );
    }

    /// @brief get cover image async
    /// @param version the beatmap version to get the cover image for
    /// @param onFinished method called when request is done void(std::optional<DataResponse>)
    /// @param progressReport method called to report download progress, void(float) 0-1
    inline auto GetCoverImageAsync(Models::BeatmapVersion const& version, finished_opt_function<BeatSaverResponse_t<&GetCoverImageURLOptions>> onFinished, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetCoverImageURLOptions>>(
            GetCoverImageURLOptions(version),
            onFinished,
            progressReport
        );
    }

    /// @brief get cover image async
    /// @param version the beatmap version to get the cover image for
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return std::future<DataResponse>
    inline auto GetCoverImageAsync(Models::BeatmapVersion const& version, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetCoverImageURLOptions>>(
            GetCoverImageURLOptions(version),
            progressReport
        );
    }

    /// @brief creates the necessary url options to download preview data
    /// @param version the beatmap version to create the url options for
    /// @return urloptions to use with webutils, expects a return of WebUtils::DataResponse
    inline WebUtils::URLOptions GetPreviewURLOptions(Models::BeatmapVersion const& version) {
        return WebUtils::URLOptions {
            version.PreviewURL
        };
    }

    DECLARE_BEATSAVER_RESPONSE_T(GetPreviewURLOptions, WebUtils::DataResponse);

    /// @brief get preview data sync
    /// @param version the beatmap version get the preview for
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return DataResponse
    inline auto GetPreview(Models::BeatmapVersion const& version, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().Get<BeatSaverResponse_t<&GetPreviewURLOptions>>(
            GetPreviewURLOptions(version),
            progressReport
        );
    }

    /// @brief get preview data async
    /// @param version the beatmap version get the preview for
    /// @param onFinished method called when request is done void(std::optional<DataResponse>)
    /// @param progressReport method called to report download progress, void(float) 0-1
    inline auto GetPreviewAsync(Models::BeatmapVersion const& version, finished_opt_function<BeatSaverResponse_t<&GetPreviewURLOptions>> onFinished, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetPreviewURLOptions>>(
            GetPreviewURLOptions(version),
            onFinished,
            progressReport
        );
    }

    /// @brief get preview data async
    /// @param version the beatmap version get the preview for
    /// @param progressReport method called to report download progress, void(float) 0-1
    /// @return std::future<DataResponse>
    inline auto GetPreviewAsync(Models::BeatmapVersion const& version, progress_function progressReport = nullptr) {
        return GetBeatsaverDownloader().GetAsync<BeatSaverResponse_t<&GetPreviewURLOptions>>(
            GetPreviewURLOptions(version),
            progressReport
        );
    }

    /// @brief download multiple beatmaps in a ratelimited fashion
    /// @param infos the beatmaps to download
    /// @param maxConcurrency maximum amount of extra threads to use
    /// @param progressReport reporter method that lets you know the progress of the downloads
    /// @return map of beatmap keys to path results, if a beatmap does not appear in here, it didn't succeed, and if the value is nullopt it didn't download
    inline std::unordered_map<std::string, std::optional<std::filesystem::path>> DownloadBeatmaps(std::span<BeatmapDownloadInfo const> infos, int maxConcurrency = 4, std::function<void(int, int)> progressReport = nullptr) {
        std::mutex resultMutex;
        std::unordered_map<std::string, std::optional<std::filesystem::path>> results;
        auto setKeyValue = [&resultMutex, &results](std::string key, std::optional<std::filesystem::path> value){
            std::unique_lock lock(resultMutex);
            results[key] = value;
        };
        WebUtils::RatelimitedDispatcher rl;
        rl.downloader = GetBeatsaverDownloader();
        rl.maxConcurrentRequests = std::max(maxConcurrency, 1);

        int total = infos.size();
        std::atomic_int completed = 0;

        rl.onRequestFinished = [setKeyValue, &progressReport, total, &completed](bool success, auto req) -> std::optional<WebUtils::RatelimitedDispatcher::RetryOptions> {
            if (!success) {
                auto targetResponse = req->TargetResponse;

                auto http = targetResponse->HttpCode;
                auto curl = targetResponse->CurlStatus;
                // don't retry if curl fails
                if (curl != 0) return std::nullopt;

                // http codes other than 2xx we want no retries
                if (http < 200 || http >= 300) return std::nullopt;

                return WebUtils::RatelimitedDispatcher::RetryOptions{std::chrono::milliseconds(50)};
            }

            completed++;

            auto beatmapReq = dynamic_cast<DownloadBeatmapRequest*>(req);
            if (!beatmapReq) return std::nullopt;

            auto key = beatmapReq->response.info.Key;
            setKeyValue(key, beatmapReq->response.responseData);
            if (progressReport) progressReport(total, completed);

            return std::nullopt;
        };

        for (auto& info : infos ) {
            rl.AddRequest(CreateDownloadBeatmapRequest(info));
        }

        rl.StartDispatchIfNeeded().wait();
        if (progressReport) progressReport(total, completed);
        return results;
    }

    /// @brief download multiple beatmaps in a ratelimited fashion
    /// @param infos the beatmaps to download
    /// @param maxConcurrency maximum amount of extra threads to use
    /// @param onFinished method called when finished, gets a map of beatmap keys to path results, if a beatmap does not appear in here, it didn't succeed, and if the value is nullopt it didn't download
    /// @param progressReport reporter method that lets you know the progress of the downloads
    inline void DownloadBeatmapsAsync(std::span<BeatmapDownloadInfo const> infos, std::function<void(std::unordered_map<std::string, std::optional<std::filesystem::path>>)> onFinished, int maxConcurrency = 4, std::function<void(int, int)> progressReport = nullptr) {
        if (!onFinished) return;

        std::thread([](std::vector<BeatmapDownloadInfo> infos, std::function<void(std::unordered_map<std::string, std::optional<std::filesystem::path>>)> onFinished, int maxConcurrency, std::function<void(int, int)> progressReport){
            onFinished(DownloadBeatmaps(infos, maxConcurrency, progressReport));
        }, std::vector(infos.begin(), infos.end()), std::forward<std::function<void(std::unordered_map<std::string, std::optional<std::filesystem::path>>)>>(onFinished), maxConcurrency, std::forward<std::function<void(int, int)>>(progressReport)).detach();
    }
#pragma endregion // download
}

#define BEATSAVER_PLUSPLUS_GET(func, ...) BeatSaver::API::GetBeatsaverDownloader().Get<BeatSaver::API::BeatSaverResponse_t<&func>>(func(__VA_ARGS__))
#define BEATSAVER_PLUSPLUS_GET_ASYNC(func, finished, ...) BeatSaver::API::GetBeatsaverDownloader().GetAsync<BeatSaver::API::BeatSaverResponse_t<&func>>(func(__VA_ARGS__), finished)
#define BEATSAVER_PLUSPLUS_GET_FUTURE(func, ...) BeatSaver::API::GetBeatsaverDownloader().GetAsync<BeatSaver::API::BeatSaverResponse_t<&func>>(func(__VA_ARGS__))
