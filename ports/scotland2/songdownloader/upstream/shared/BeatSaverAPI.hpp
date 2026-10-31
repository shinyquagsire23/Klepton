#pragma once

#include "./_config.h"

#include "Types/BeatSaver/Page.hpp"

#include "Types/ScoreSaber/Page.hpp"
#include "Types/ScoreSaber/Leaderboards.hpp"

// avoid linking to playlistcore in headers
namespace PlaylistCore {
    class Playlist;
};

#include <string>

namespace BeatSaver::API {

    SONGDOWNLOADER_EXPORT std::optional<BeatSaver::Beatmap> GetBeatmapByKey(std::string key);

    SONGDOWNLOADER_EXPORT std::optional<BeatSaver::Beatmap> GetBeatmapByHash(std::string hash);

    SONGDOWNLOADER_EXPORT std::optional<BeatSaver::UserDetail> GetUserById(int id);

    SONGDOWNLOADER_EXPORT std::optional<BeatSaver::UserDetail> GetUserByName(std::string username);

    /// <summary>
    /// Search on BeatSaver
    /// </summary>
    /// <param name="query"> What you want to search on BeatSaver</param>
    /// <param name="pageIndex"> Index of the pages search results</param>
    /// <param name="sortOrder"> Order in which the search results should be sorted default "Relevance"</param>
    /// <returns>An std::optional that either contains BeatSaver::Page or std::nullopt</returns>
    /// <param name="ME"> MappingExtensions maps should be included in search default "" for unset can be "true" or "false" otherwise</param>
    /// <param name="NE"> MappingExtensions maps should be included in search default "" for unset can be "true" or "false" otherwise</param>
    /// <param name="Chroma"> MappingExtensions maps should be included in search default "" for unset can be "true" or "false" otherwise</param>
    SONGDOWNLOADER_EXPORT std::optional<BeatSaver::Page> SearchPaged(std::string query, int pageIndex, std::string sortOrder = "Relevance", std::string ME = "", std::string NE = "", std::string Chroma = "");

    /// <summary>
    /// Get maps ordered by upload date
    /// </summary>
    /// <param name="automapper">If automapper maps should be included, default false</param>
    /// <param name="after">Specify to only show maps after specific data format
    /// "<date>T<time>" with date in "YYYY-DD-MM" and time in "hh:mm:ss.sss" plus timezone designation or "Z" for the UTC timezone
    /// for example "2020-03-08T01:46:20.649Z" or "2020-01-08T00:00:00.000+02:00" for UTC +02:00 read ISO 8601 UTC Date time
    /// defaults to an empty string</param>
    /// <returns></returns>
    SONGDOWNLOADER_EXPORT std::optional<BeatSaver::Page> LatestPaged(bool automapper = false, std::string after = "");

    SONGDOWNLOADER_EXPORT std::optional<BeatSaver::Page> PlaysPaged(int pageIndex);

    SONGDOWNLOADER_EXPORT bool DownloadBeatmap(const BeatSaver::Beatmap& beatmap);

    SONGDOWNLOADER_EXPORT std::vector<uint8_t> GetCoverImage(const BeatSaver::Beatmap& beatmap);

    // You need to check yourself if the bytes would make a valid coverImage
    SONGDOWNLOADER_EXPORT std::optional<std::vector<uint8_t>> GetCoverImage(std::string hash);

    SONGDOWNLOADER_EXPORT std::vector<uint8_t> GetPreview(const BeatSaver::Beatmap& beatmap);

    SONGDOWNLOADER_EXPORT void GetBeatmapByKeyAsync(std::string key, std::function<void(std::optional<BeatSaver::Beatmap>)> finished);

    /// <summary>
    /// Get a Beatmap based on Hash
    /// </summary>
    /// <param name="hash"> Hash of the beatmap, levelID of a custom level contains it</param>
    /// <param name="finished"> Function to run after it finishes loading</param>
    SONGDOWNLOADER_EXPORT void GetBeatmapByHashAsync(std::string hash, std::function<void(std::optional<BeatSaver::Beatmap>)> finished);

    SONGDOWNLOADER_EXPORT void GetBeatmapByUserIdAsync(int userID, int page, std::function<void(std::optional<BeatSaver::Page>)> finished);

    SONGDOWNLOADER_EXPORT void GetUserByIdAsync(int id, std::function<void(std::optional<BeatSaver::UserDetail>)> finished);

    SONGDOWNLOADER_EXPORT void GetUserByNameAsync(std::string username, std::function<void(std::optional<BeatSaver::UserDetail>)> finished);

    /// <summary>
    /// Search on BeatSaver
    /// </summary>
    /// <param name="query"> What you want to search on BeatSaver</param>
    /// <param name="pageIndex"> Index of the pages search results</param>
    /// <param name="finished"> Function to run after it finishes loading</param>
    /// <param name="sortOrder"> Order in which the search results should be sorted default "Relevance"</param>
    /// <param name="ME"> MappingExtensions maps should be included in search default "" for unset can be "true" or "false" otherwise</param>
    /// <param name="NE"> MappingExtensions maps should be included in search default "" for unset can be "true" or "false" otherwise</param>
    /// <param name="Chroma"> MappingExtensions maps should be included in search default "" for unset can be "true" or "false" otherwise</param>
    SONGDOWNLOADER_EXPORT void SearchPagedAsync(std::string query, int pageIndex, std::function<void(std::optional<BeatSaver::Page>)> finished, std::string sortOrder = "Relevance", std::string automapper = "", std::string ranked = "", std::string ME = "", std::string NE = "", std::string Chroma = "");

    SONGDOWNLOADER_EXPORT void DownloadBeatmapAsync(const BeatSaver::Beatmap& beatmap, std::function<void(bool)> finished, std::function<void(float)> progressUpdate = nullptr);

    SONGDOWNLOADER_EXPORT void DownloadBeatmapAsync(const BeatSaver::Beatmap& beatmap, const BeatSaver::BeatmapVersion& beatmapVer, std::function<void(bool)> finished, std::function<void(float)> progressUpdate = nullptr);

    /// <summary>
    /// For downloading maps gotten from the ScoreSaberAPI
    /// </summary>
    /// <param name="song">ScoreSaber Song reference</param>
    /// <param name="finished">Function to run after finishing to download</param>
    /// <param name="progressUpdate">Function to run on updating download progress</param>
    [[deprecated("Downloading with ScoreSaber::Song will not be supported in the future use ScoreSaber::Leaderboard!")]]
    SONGDOWNLOADER_EXPORT void DownloadBeatmapAsync(const ScoreSaber::Song& song, std::function<void(bool)> finished, std::function<void(float)> progressUpdate = nullptr);

    /// <summary>
    /// For downloading maps gotten from the ScoreSaberAPI
    /// </summary>
    /// <param name="ldb">ScoreSaber Leaderboard reference</param>
    /// <param name="finished">Function to run after finishing to download</param>
    /// <param name="progressUpdate">Function to run on updating download progress</param>
    SONGDOWNLOADER_EXPORT void DownloadBeatmapAsync(const ScoreSaber::Leaderboard& ldb, std::function<void(bool)> finished, std::function<void(float)> progressUpdate = nullptr);

    SONGDOWNLOADER_EXPORT void GetCoverImageAsync(const BeatSaver::Beatmap& beatmap, std::function<void(std::vector<uint8_t>)> finished, std::function<void(float)> progressUpdate = nullptr);

    SONGDOWNLOADER_EXPORT void GetCoverImageAsync(const BeatSaver::BeatmapVersion& beatmap, std::function<void(std::vector<uint8_t>)> finished, std::function<void(float)> progressUpdate = nullptr);

    SONGDOWNLOADER_EXPORT void GetCoverImageByHashAsync(std::string hash, std::function<void(std::vector<uint8_t>)> finished, std::function<void(float)> progressUpdate = nullptr);

    // Returns mp3 with previewAudio
    SONGDOWNLOADER_EXPORT void GetPreviewAsync(const BeatSaver::Beatmap& beatmap, std::function<void(std::vector<uint8_t>)> finished, std::function<void(float)> progressUpdate = nullptr);

    SONGDOWNLOADER_EXPORT void DownloadMissingSongsFromPlaylist(PlaylistCore::Playlist* playlist, std::function<void()> finished, std::function<void(int, int)> progressUpdate = nullptr);

    // Legacy ABI; internal callers use the synchronized snapshot below.
    SONGDOWNLOADER_EXPORT extern std::string exception;
    SONGDOWNLOADER_EXPORT std::string GetException();
}
