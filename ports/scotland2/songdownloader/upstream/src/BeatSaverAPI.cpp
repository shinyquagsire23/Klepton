#include "BeatSaverAPI.hpp"

#include "CustomLogger.hpp"
#include <mutex>
#include <memory>

#include "Utils/WebUtils.hpp"
#include "Utils/FileUtils.hpp"

#include "zip.h"

#include "songcore/shared/SongCore.hpp"

#include "playlistcore/shared/PlaylistCore.hpp"
#include "playlistcore/shared/Utils.hpp"

#include "Exceptions.hpp"

#define BASE_URL "beatsaver.com"
#define FALLBACK_URL "beatmaps.io"
#define API_URL "https://api." BASE_URL
#define CDN_URL "https://cdn." BASE_URL "/"
#define FILE_DOWNLOAD_TIMEOUT 64

namespace BeatSaver::API {

    std::string exception;
    static std::mutex exceptionMutex;
    static void SetException(std::string value) {
        std::lock_guard<std::mutex> lock(exceptionMutex);
        exception = std::move(value);
    }
    std::string GetException() {
        std::lock_guard<std::mutex> lock(exceptionMutex);
        return exception;
    }

    std::optional<BeatSaver::Beatmap> GetBeatmapByKey(std::string key) {
        SetException({});
        auto json = WebUtils::GetJSON(fmt::format(API_URL "/maps/id/{}", key));
        if (!json.has_value())
            return std::nullopt;
        try {
            BeatSaver::Beatmap beatmap;
            Beatmap::Deserialize(&beatmap, json.value());
            return beatmap;
        }
        catch (const std::exception& e) {
            LOG_ERROR("{}", e.what());
            SetException(e.what());
            return std::nullopt;
        }
    }

    std::optional<BeatSaver::Beatmap> GetBeatmapByHash(std::string hash) {
        SetException({});
        auto json = WebUtils::GetJSON(fmt::format(API_URL "/maps/hash/{}", hash));
        if (!json.has_value())
            return std::nullopt;
        try {
            BeatSaver::Beatmap beatmap;
            Beatmap::Deserialize(&beatmap, json.value());
            return beatmap;
        }
        catch (const std::exception& e) {
            LOG_ERROR("{}", e.what());
            SetException(e.what());
            return std::nullopt;
        }
    }

    std::optional<BeatSaver::UserDetail> GetUserById(int id) {
        SetException({});
        auto json = WebUtils::GetJSON(fmt::format(API_URL "/users/id/{}", id));
        if (!json.has_value())
            return std::nullopt;
        try {
            BeatSaver::UserDetail user;
            UserDetail::Deserialize(&user, json.value());
            return user;
        }
        catch (const std::exception& e) {
            LOG_ERROR("{}", e.what());
            SetException(e.what());
            return std::nullopt;
        }
    }

    std::optional<BeatSaver::UserDetail> GetUserByName(std::string username) {
        SetException({});
        auto json = WebUtils::GetJSON(fmt::format(API_URL "/users/name/{}", username));
        if (!json.has_value())
            return std::nullopt;
        try {
            BeatSaver::UserDetail user;
            UserDetail::Deserialize(&user, json.value());
            return user;
        }
        catch (const std::exception& e) {
            LOG_ERROR("{}", e.what());
            SetException(e.what());
            return std::nullopt;
        }
    }

    std::optional<BeatSaver::Page> SearchPaged(std::string query, int pageIndex, std::string sortOrder, std::string automapper, std::string ME, std::string NE, std::string Chroma) {
        SetException({});
        std::stringstream searchStream;
        searchStream << API_URL "/search/text/" << pageIndex;
        searchStream << "?sortOrder=" << sortOrder;
        if (!automapper.empty()) searchStream << "&automapper=" << automapper;
        if (!query.empty()) searchStream << "&q=" << query;
        if (!ME.empty()) searchStream << "&me=" << ME;
        if (!NE.empty()) searchStream << "&noodle=" << NE;
        if (!Chroma.empty()) searchStream << "&chroma=" << Chroma;
        auto searchPath = searchStream.str();
        auto json = WebUtils::GetJSON(searchPath);
        if (!json.has_value())
            return std::nullopt;
        try {
            BeatSaver::Page page;
            Page::Deserialize(&page, json.value());
            return page;
        }
        catch (const std::exception& e) {
            LOG_ERROR("{}", e.what());
            SetException(e.what());
            return std::nullopt;
        }
    }

    std::optional<BeatSaver::Page> LatestPaged(bool automapper, std::string after) {
        SetException({});
        std::optional<rapidjson::Document> json;
        if (after.empty()) {
            json = WebUtils::GetJSON(fmt::format(API_URL "/maps/latest?automapper={}", automapper));
        }
        else {
            json = WebUtils::GetJSON(fmt::format(API_URL "/maps/latest?after={}&automapper={}", after, automapper));
        }
        if (!json.has_value())
            return std::nullopt;
        try {
            BeatSaver::Page page;
            Page::Deserialize(&page, json.value());
            return page;
        }
        catch (const std::exception& e) {
            LOG_ERROR("{}", e.what());
            SetException(e.what());
            return std::nullopt;
        }
    }

    std::optional<BeatSaver::Page> PlaysPaged(int pageIndex) {
        SetException({});
        auto json = WebUtils::GetJSON(fmt::format(API_URL "/maps/plays/{}", pageIndex));
        if (!json.has_value())
            return std::nullopt;
        try {
            BeatSaver::Page page;
            Page::Deserialize(&page, json.value());
            return page;
        }
        catch (const std::exception& e) {
            LOG_ERROR("{}", e.what());
            SetException(e.what());
            return std::nullopt;
        }
    }

    bool DownloadBeatmap(const BeatSaver::Beatmap& beatmap) {
        if (beatmap.GetVersions().empty()) { return false; }
        auto levelsPath = SongCore::API::Loading::GetPreferredCustomLevelPath();
        auto targetFolderName = FileUtils::FixIlegalName(fmt::format("{} ({} - {})", beatmap.GetId(), beatmap.GetMetadata().GetSongName(), beatmap.GetMetadata().GetLevelAuthorName()));
        auto targetFolder = levelsPath / targetFolderName;
        std::string data;
        if (beatmap.GetVersions().empty() || WebUtils::Get(beatmap.GetVersions().front().GetDownloadURL(), FILE_DOWNLOAD_TIMEOUT, data)!=200 || data.empty()) return true;
        int args = 2;
        int statusCode = zip_stream_extract(data.data(), data.length(), targetFolder.c_str(), +[](const char* name, void* arg) -> int {
            return 0;
            }, &args);
        return statusCode;
    }

    std::vector<uint8_t> GetPreview(const BeatSaver::Beatmap& beatmap) {
        if (beatmap.GetVersions().empty()) { return {}; }
        std::string data;
        WebUtils::Get(beatmap.GetVersions().front().GetPreviewURL(), FILE_DOWNLOAD_TIMEOUT, data);
        std::vector<uint8_t> bytes(data.begin(), data.end());
        return bytes;
    }

    std::vector<uint8_t> GetCoverImage(const BeatSaver::Beatmap& beatmap) {
        if (beatmap.GetVersions().empty()) { return {}; }
        std::string data;
        WebUtils::Get(beatmap.GetVersions().front().GetCoverURL(), FILE_DOWNLOAD_TIMEOUT, data);
        std::vector<uint8_t> bytes(data.begin(), data.end());
        return bytes;
    }

    // This isn't really ideal, but it could be kinda useful to be able to get the coverImage just by hash directly
    std::optional<std::vector<uint8_t>> GetCoverImage(std::string hash) {
        std::string data;
        std::transform(hash.begin(), hash.end(), hash.begin(), tolower);
        WebUtils::Get(fmt::format(CDN_URL "{}.jpg", hash), FILE_DOWNLOAD_TIMEOUT, data);
        if (data.empty()) return std::nullopt;
        std::vector<uint8_t> bytes(data.begin(), data.end());
        return bytes;
    }

    void GetBeatmapByKeyAsync(std::string key, std::function<void(std::optional<BeatSaver::Beatmap>)> finished) {
        SetException({});
        WebUtils::GetJSONAsync(fmt::format(API_URL "/maps/id/{}", key),
            [finished](long httpCode, bool error, rapidjson::Document& document) {
                if (error || ++document.MemberBegin() == document.MemberEnd()) {
                    finished(std::nullopt);
                }
                else {
                    try {
                        BeatSaver::Beatmap beatmap;
                        Beatmap::Deserialize(&beatmap, document);
                        finished(beatmap);
                    }
                    catch (const std::exception& e) {
                        LOG_ERROR("{}", e.what());
                        SetException(e.what());
                        finished(std::nullopt);
                        //// Convert the document into a string and log/write to file for debug purposes
                        //rapidjson::StringBuffer buffer;
                        //rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
                        //document.Accept(writer);
                        //writefile("/sdcard/ModData/GetBeatmapByKeyAsync.json", buffer.GetString());
                    }
                }
            }
        );
    }

    void GetBeatmapByHashAsync(std::string hash, std::function<void(std::optional<BeatSaver::Beatmap>)> finished) {
        SetException({});
        WebUtils::GetJSONAsync(fmt::format(API_URL "/maps/hash/{}", hash),
            [finished](long httpCode, bool error, rapidjson::Document& document) {
                if (error || ++document.MemberBegin() == document.MemberEnd()) {
                    finished(std::nullopt);
                }
                else {
                    try {
                        BeatSaver::Beatmap beatmap;
                        Beatmap::Deserialize(&beatmap, document);
                        finished(beatmap);
                    }
                    catch (const std::exception& e) {
                        LOG_ERROR("{}", e.what());
                        SetException(e.what());
                        finished(std::nullopt);
                        //// Convert the document into a string and log/write to file for debug purposes
                        //rapidjson::StringBuffer buffer;
                        //rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
                        //document.Accept(writer);
                        //writefile("/sdcard/ModData/GetBeatmapByHashAsync.json", buffer.GetString());
                    }
                }
            }
        );
    }

    void GetBeatmapByUserIdAsync(int userID, int page, std::function<void(std::optional<BeatSaver::Page>)> finished) {
        SetException({});
        std::string searchPath = fmt::format(API_URL "/maps/uploader/{}/{}", userID, page);
        LOG_DEBUG("GetBeatmapByUserIdAsync: {}", searchPath);
        WebUtils::GetJSONAsync(searchPath,
            [finished](long httpCode, bool error, rapidjson::Document& document) {
                if (error) {
                    finished(std::nullopt);
                }
                else {
                    //// Convert the document into a string and log/write to file for debug purposes
                    //rapidjson::StringBuffer buffer;
                    //rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
                    //document.Accept(writer);
                    //writefile("/sdcard/ModData/GetBeatmapByUserIdAsync.json", buffer.GetString());
                    try {
                        BeatSaver::Page page;
                        Page::Deserialize(&page, document);
                        finished(page);
                    }
                    catch (const std::exception& e) {
                        LOG_ERROR("{}", e.what());
                        SetException(e.what());
                        finished(std::nullopt);
                    }
                }
            }
        );
    }

    void GetUserByNameAsync(std::string username, std::function<void(std::optional<BeatSaver::UserDetail>)> finished) {
        SetException({});
        std::string searchPath = fmt::format(API_URL "/users/name/{}", username);
        LOG_DEBUG("GetUserByNameAsync: {}", searchPath);
        WebUtils::GetJSONAsync(searchPath,
            [finished](long httpCode, bool error, rapidjson::Document& document) {
                if (error) {
                    finished(std::nullopt);
                }
                else {
                    // Convert the document into a string and log/write to file for debug purposes
                    //rapidjson::StringBuffer buffer;
                    //rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
                    //document.Accept(writer);
                    //writefile("/sdcard/ModData/GetUserByNameAsync.json", buffer.GetString());
                    try {
                        BeatSaver::UserDetail user;
                        UserDetail::Deserialize(&user, document);
                        finished(user);
                    }
                    catch (const std::exception& e) {
                        LOG_ERROR("{}", e.what());
                        SetException(e.what());
                        finished(std::nullopt);
                    }
                }
            }
        );
    }


    void GetUserByIdAsync(int id, std::function<void(std::optional<BeatSaver::UserDetail>)> finished) {
        SetException({});
        auto searchPath = fmt::format(API_URL "/users/id/{}", id);
        LOG_DEBUG("GetUserByIdAsync: {}", searchPath);
        WebUtils::GetJSONAsync(searchPath,
            [finished](long httpCode, bool error, rapidjson::Document& document) {
                if (error) {
                    finished(std::nullopt);
                }
                else {
                    try {
                        BeatSaver::UserDetail user;
                        UserDetail::Deserialize(&user, document);
                        finished(user);
                    }
                    catch (const std::exception& e) {
                        LOG_ERROR("{}", e.what());
                        SetException(e.what());
                        finished(std::nullopt);
                        //// Convert the document into a string and log/write to file for debug purposes
                        //rapidjson::StringBuffer buffer;
                        //rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
                        //document.Accept(writer);
                        //writefile("/sdcard/ModData/GetUserByIdAsync.json", buffer.GetString());
                    }
                }
            }
        );
    }

    void SearchPagedAsync(std::string query, int pageIndex, std::function<void(std::optional<BeatSaver::Page>)> finished, std::string sortOrder, std::string automapper, std::string ranked, std::string ME, std::string NE, std::string Chroma) {
        SetException({});
        std::stringstream searchStream;
        searchStream << API_URL "/search/text/" << pageIndex;
        searchStream << "?sortOrder=" << sortOrder;
        if (!query.empty()) searchStream << "&q=" << query;
        if (!automapper.empty()) searchStream << "&automapper=" << automapper;
        if (!ranked.empty()) searchStream << "&ranked=" << ranked;
        if (!ME.empty()) searchStream << "&me=" << ME;
        if (!NE.empty()) searchStream << "&noodle=" << NE;
        if (!Chroma.empty()) searchStream << "&chroma=" << Chroma;
        auto searchPath = searchStream.str();
        LOG_DEBUG("SearchPagedAsync: {}", searchPath);
        WebUtils::GetJSONAsync(searchPath,
            [finished](long httpCode, bool error, rapidjson::Document& document) {
                if (error) {
                    finished(std::nullopt);
                }
                else {
                    //// Convert the document into a string and log/write to file for debug purposes
                    //rapidjson::StringBuffer buffer;
                    //rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
                    //document.Accept(writer);
                    //LOG_DEBUG("{}", buffer.GetString());
                    //writefile("/sdcard/ModData/SearchQuery.json", buffer.GetString());
                    try {
                        BeatSaver::Page page;
                        Page::Deserialize(&page, document);
                        finished(page);
                    }
                    catch (const std::exception& e) {
                        LOG_ERROR("{}", e.what());
                        SetException(e.what());
                        finished(std::nullopt);
                    }
                }
            }
        );
    }

    void DownloadBeatmapAsync(const BeatSaver::Beatmap& beatmap, std::function<void(bool)> finished, std::function<void(float)> progressUpdate) {
        if (beatmap.GetVersions().empty()) { if (finished) finished(true); return; }
        WebUtils::GetAsync(beatmap.GetVersions().front().GetDownloadURL(), FILE_DOWNLOAD_TIMEOUT,
            [beatmap, finished](long httpCode, std::string data) {
                if (httpCode!=200 || data.empty()) { if(finished) finished(true); return; }
                auto levelsPath = SongCore::API::Loading::GetPreferredCustomLevelPath();
                auto targetFolderName = FileUtils::FixIlegalName(fmt::format("{} ({} - {})", beatmap.GetId(), beatmap.GetMetadata().GetSongName(), beatmap.GetMetadata().GetLevelAuthorName()));
                auto targetFolder = levelsPath / targetFolderName;
                int args = 2;
                int statusCode = zip_stream_extract(data.data(), data.length(), targetFolder.c_str(), +[](const char* name, void* arg) -> int {
                    return 0;
                    }, &args);
                finished(statusCode);
            }, progressUpdate
        );
    }

    void DownloadBeatmapAsync(const BeatSaver::Beatmap& beatmap, const BeatSaver::BeatmapVersion& beatmapVer, std::function<void(bool)> finished, std::function<void(float)> progressUpdate) {
        WebUtils::GetAsync(beatmapVer.GetDownloadURL(), FILE_DOWNLOAD_TIMEOUT,
            [beatmap, finished](long httpCode, std::string data) {
                if (httpCode!=200 || data.empty()) { if(finished) finished(true); return; }
                auto levelsPath = SongCore::API::Loading::GetPreferredCustomLevelPath();
                auto targetFolderName = FileUtils::FixIlegalName(fmt::format("{} ({} - {})", beatmap.GetId(), beatmap.GetMetadata().GetSongName(), beatmap.GetMetadata().GetLevelAuthorName()));
                auto targetFolder = levelsPath / targetFolderName;

                int args = 2;
                int statusCode = zip_stream_extract(data.data(), data.length(), targetFolder.c_str(), +[](const char* name, void* arg) -> int {
                    return 0;
                    }, &args);
                finished(statusCode);
            }, progressUpdate
        );
    }

    void DownloadBeatmapAsync(const ScoreSaber::Song& song, std::function<void(bool)> finished, std::function<void(float)> progressUpdate) {
        // Probably a deprecated method of downloading maps, but at least will work if map has isn't up to date https://api.beatsaver.com/download/key/1b236
        std::string hash = song.GetId();
        std::transform(hash.begin(), hash.end(), hash.begin(), tolower);
        WebUtils::GetAsync(fmt::format(CDN_URL "{}.zip", hash), FILE_DOWNLOAD_TIMEOUT,
            [song, finished](long httpCode, std::string data) {
                if(httpCode!=200 || data.empty()){if(finished)finished(true);return;}
                auto levelsPath = SongCore::API::Loading::GetPreferredCustomLevelPath();
                auto targetFolderName = FileUtils::FixIlegalName(fmt::format("{} ({} - {})", song.GetUid(), song.GetName(), song.GetLevelAuthorName()));
                auto targetFolder = levelsPath / targetFolderName;

                int args = 2;
                int statusCode = zip_stream_extract(data.data(), data.length(), targetFolder.c_str(), +[](const char* name, void* arg) -> int {
                    return 0;
                    }, &args);
                finished(statusCode);
            }, progressUpdate
        );
    }

    void DownloadBeatmapAsync(const ScoreSaber::Leaderboard& ldb, std::function<void(bool)> finished, std::function<void(float)> progressUpdate) {
        // Probably a deprecated method of downloading maps, but at least will work if map has isn't up to date https://api.beatsaver.com/download/key/1b236
        GetBeatmapByHashAsync(ldb.GetSongHash(), [finished, progressUpdate](const std::optional<BeatSaver::Beatmap>& beatmap) {
            if (beatmap.has_value())
                DownloadBeatmapAsync(beatmap.value(), finished, progressUpdate);
            else
                if (finished) finished(true);
            }
        );

        //WebUtils::GetAsync(fmt::format(CDN_URL "{}.zip", hash), FILE_DOWNLOAD_TIMEOUT,
        //    [song, finished](long httpCode, std::string data) {
        //        auto levelsPath = SongCore::API::Loading::GetPreferredCustomLevelPath();
        //        auto targetFolderName = FileUtils::FixIlegalName(fmt::format("{} ({} - {})", song.GetUid(), song.GetName(), song.GetLevelAuthorName()));
        //        auto targetFolder = levelsPath / targetFolderName;
        //
        //        int args = 2;
        //        int statusCode = zip_stream_extract(data.data(), data.length(), targetFolder.c_str(), +[](const char* name, void* arg) -> int {
        //            return 0;
        //            }, &args);
        //        finished(statusCode);
        //    }, progressUpdate
        //);
    }

    void GetCoverImageAsync(const BeatSaver::Beatmap& beatmap, std::function<void(std::vector<uint8_t>)> finished, std::function<void(float)> progressUpdate) {
        if (beatmap.GetVersions().empty()) { if (finished) finished({}); return; }
        WebUtils::GetAsync(beatmap.GetVersions().front().GetCoverURL(), FILE_DOWNLOAD_TIMEOUT,
            [beatmap, finished](long httpCode, std::string data) {
                if (httpCode!=200 || data.empty()) { if(finished) finished({}); return; }
                std::vector<uint8_t> bytes(data.begin(), data.end());
                finished(bytes);
            }, progressUpdate
        );
    }

    void GetCoverImageAsync(const BeatSaver::BeatmapVersion& beatmap, std::function<void(std::vector<uint8_t>)> finished, std::function<void(float)> progressUpdate) {
        WebUtils::GetAsync(beatmap.GetCoverURL(), FILE_DOWNLOAD_TIMEOUT,
            [beatmap, finished](long httpCode, std::string data) {
                if (httpCode!=200 || data.empty()) { if(finished) finished({}); return; }
                std::vector<uint8_t> bytes(data.begin(), data.end());
                finished(bytes);
            }, progressUpdate
        );
    }

    void GetCoverImageByHashAsync(std::string hash, std::function<void(std::vector<uint8_t>)> finished, std::function<void(float)> progressUpdate) {
        auto coverURL = fmt::format(CDN_URL "{}.jpg", hash);
        //LOG_DEBUG("CoverURL: {}", coverURL);
        WebUtils::GetAsync(coverURL, FILE_DOWNLOAD_TIMEOUT,
            [hash, finished](long httpCode, std::string data) {
                //LOG_DEBUG("HTTP Code when loading by hash: {}", httpCode);
                if (httpCode != 200) return;
                std::vector<uint8_t> bytes(data.begin(), data.end());
                finished(bytes);
            }, progressUpdate
        );
    }

    void GetPreviewAsync(const BeatSaver::Beatmap& beatmap, std::function<void(std::vector<uint8_t>)> finished, std::function<void(float)> progressUpdate) {
        if (beatmap.GetVersions().empty()) { if (finished) finished({}); return; }
        WebUtils::GetAsync(beatmap.GetVersions().front().GetPreviewURL(), FILE_DOWNLOAD_TIMEOUT,
            [beatmap, finished](long httpCode, std::string data) {
                if (httpCode!=200 || data.empty()) { if(finished) finished({}); return; }
                std::vector<uint8_t> bytes(data.begin(), data.end());
                finished(bytes);
            }, progressUpdate
        );
    }

    void DownloadMissingSongsFromPlaylist(PlaylistCore::Playlist* playlist, std::function<void()> finished, std::function<void(int, int)> progressUpdate) {
        if (!playlist || !playlist->playlistCS) { if (finished) finished(); return; }
        auto songQueue = std::vector<std::string>{};
        auto downloads = std::make_shared<std::atomic_int>(0);
        // add all nonpresent song hashes to queue
        for(auto& song : playlist->playlistJSON.Songs) {
            bool hasSong = false;
            // search in songs in playlist instead of all songs
            // we need to treat the list as an array because it is initialized as an array elsewhere
            auto levelList = playlist->playlistCS->_beatmapLevels;
            for(auto& level : levelList) {
                std::string hash = level->levelID;
                if(hash.starts_with("custom_level")) {
                    hash = hash.substr(13);
                }
                if(song.Hash == std::move(hash)) {
                    hasSong = true;
                    break;
                }
            }
            if(!hasSong && song.Hash)
                songQueue.emplace_back(song.Hash.value());
        }
        const int quantity = static_cast<int>(songQueue.size());
        if (!quantity) { if (finished) finished(); return; }
        if (progressUpdate) progressUpdate(0, quantity);
        // recursive (because threads) callback for each time a beatmap is recieved from beatsaver
        static void(*onBeatmap)(std::vector<std::string>, std::optional<Beatmap>, std::function<void()>, std::function<void(int, int)>, int, std::shared_ptr<std::atomic_int>)
                = *[](std::vector<std::string> songQueue, std::optional<Beatmap> beatmap, std::function<void()> finished, std::function<void(int, int)> progressUpdate, int quantity, std::shared_ptr<std::atomic_int> downloads) mutable {
            // start next beatmap
            if(!songQueue.empty()) {
                auto nextHash = songQueue.back();
                GetBeatmapByHashAsync(nextHash, [songQueue = std::move(songQueue), finished, progressUpdate, quantity, downloads](std::optional<Beatmap> beatmap) mutable {
                    songQueue.pop_back();
                    onBeatmap(std::move(songQueue), std::move(beatmap), finished, progressUpdate, quantity, downloads);
                });
            }
            // download if beatmap is found, but update downloads and potentially run finish either way
            if(beatmap.has_value()) {
                DownloadBeatmapAsync(beatmap.value(), [finished, progressUpdate, quantity, downloads](bool _) {
                    const int completed = downloads->fetch_add(1) + 1;
                    bool complete = completed == quantity;
                    if(progressUpdate)
                        progressUpdate(completed, quantity);
                    if(complete && finished) {
                        finished();
                    }
                });
            } else {
                LOG_INFO("Beatmap not found on beatsaver");
                const int completed = downloads->fetch_add(1) + 1;
                    bool complete = completed == quantity;
                if(progressUpdate)
                    progressUpdate(completed, quantity);
                if(complete && finished) {
                    finished();
                }
            }
        };
        auto nextHash = songQueue.back();
                GetBeatmapByHashAsync(nextHash, [songQueue = std::move(songQueue), finished, progressUpdate, quantity, downloads](std::optional<Beatmap> beatmap) mutable {
            songQueue.pop_back();
            onBeatmap(std::move(songQueue), std::move(beatmap), finished, progressUpdate, quantity, downloads);
        });
    }

}
