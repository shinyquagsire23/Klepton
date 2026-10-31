#pragma once

#include "./_config.h"
#include "Types/ScoreSaber/Page.hpp"
#include "Types/ScoreSaber/Leaderboards.hpp"

#include <string>
#include <optional>

namespace ScoreSaber::API {

    /// <summary>
    /// Enum of categories supported by ScoreSaber
    /// Search is not supported on TopPlayed and TopRanked
    /// </summary>
    enum class SONGDOWNLOADER_EXPORT ListCategory {
        Trending,
        LatestRanked,
        TopPlayed,
        TopRanked,
        Author
    };

    /// <summary>
    /// Enum of the Types supported by Search
    /// NOTE: Trending doesn't support searchQueries
    /// and will return no results if one is entered
    /// </summary>
    [[deprecated]] typedef ListCategory SearchType;

    /// <summary>
    /// Retrieves songs that are Trending as determined by ScoreSaber.
    /// </summary>
    /// <param name="ranked">If return value should contain only ranked or be mixed default: false</param>
    /// <param name="query">Searh Query</param>
    /// <param name="pageIndex">Index of the pages search results default: 0</param>
    /// <param name="amount">Amount of Songs to get default: 20</param>
    /// <returns>An optional that can contain a ScoreSaber::Page or a std::nullopt if an error was encountered or no map found</returns>
    [[deprecated("Use GetList(ListCategory list, std::optional<bool> ranked, int pageIndex) instead!")]]
    SONGDOWNLOADER_EXPORT std::optional<ScoreSaber::Page> GetTrending(bool ranked = false, int pageIndex = 0, int amount = 20);

    /// <summary>
    /// Retrieves the latest ranked songs.
    /// </summary>
    /// <param name="ranked">If return value should contain only ranked or be mixed default: false</param>
    /// <param name="query">Searh Query</param>
    /// <param name="pageIndex">Index of the pages search results default: 0</param>
    /// <param name="amount">Amount of Songs to get default: 20</param>
    /// <returns>An optional that can contain a ScoreSaber::Page or a std::nullopt if an error was encountered or no map found</returns>
    [[deprecated("Use GetList(ListCategory list, std::optional<bool> ranked, int pageIndex) instead!")]]
    SONGDOWNLOADER_EXPORT std::optional<ScoreSaber::Page> GetLatestRanked(bool ranked = false, int pageIndex = 0, int amount = 20);

    /// <summary>
    /// Retrieves the songs that have the most plays as determined by ScoreSaber.
    /// </summary>
    /// <param name="ranked">If return value should contain only ranked or be mixed default: false</param>
    /// <param name="query">Searh Query</param>
    /// <param name="pageIndex">Index of the pages search results default: 0</param>
    /// <param name="amount">Amount of Songs to get default: 20</param>
    /// <returns>An optional that can contain a ScoreSaber::Page or a std::nullopt if an error was encountered or no map found</returns>
    [[deprecated("Use GetList(ListCategory list, std::optional<bool> ranked, int pageIndex) instead!")]]
    SONGDOWNLOADER_EXPORT std::optional<ScoreSaber::Page> GetTopPlayed(bool ranked = false, int pageIndex = 0, int amount = 20);

    /// <summary>
    /// Retrieves songs ordered from highest ranked value to lowest.
    /// </summary>
    /// <param name="ranked">If return value should contain only ranked or be mixed default: false</param>
    /// <param name="query">Searh Query</param>
    /// <param name="pageIndex">Index of the pages search results default: 0</param>
    /// <param name="amount">Amount of Songs to get default: 20</param>
    /// <returns>An optional that can contain a ScoreSaber::Page or a std::nullopt if an error was encountered or no map found</returns>
    [[deprecated("Use GetList(ListCategory list, std::optional<bool> ranked, std::optional<bool> qualified, std::optional<bool> unique,  int pageIndex) instead!")]]
    SONGDOWNLOADER_EXPORT std::optional<ScoreSaber::Page> GetTopRanked(bool ranked = false, int pageIndex = 0, int amount = 20);

    /// <summary>
    /// Retrieves specified list from ScoreSaber.
    /// </summary>
    /// <param name="ranked">If return value should contain ranked, mixed or non-ranked default: std::nullopt (mixed)</param>
    /// <param name="qualified">If return value should contain qualified, mixed or non-qualified default: std::nullopt (mixed)</param>
    /// <param name="unique">Only return one leaderboard of each id</param>
    /// <param name="pageIndex">Index of the pages search results default: 0</param>
    /// <returns>An optional that can contain a ScoreSaber::Page or a std::nullopt if an error was encountered or no map found</returns>
    SONGDOWNLOADER_EXPORT std::optional<ScoreSaber::Leaderboards> GetList(ListCategory list, std::optional<bool> ranked = std::nullopt, std::optional<bool> qualified = std::nullopt, std::optional<bool> unique = std::nullopt, int pageIndex = 0);

    /// <summary>
    /// Retrieves CoverImage as uint8_t bytes
    /// </summary>
    /// <param name="song">Reference to a scoresaber song</param>
    /// <returns>The bytes for the coverImage, check if valid</returns>
    SONGDOWNLOADER_EXPORT std::vector<uint8_t> GetCoverImage(const ScoreSaber::Song& song);

    /// <summary>
    /// Retrieves CoverImage as uint8_t bytes
    /// </summary>
    /// <param name="ldb">Reference to a scoresaber leaderboard</param>
    /// <returns>The bytes for the coverImage, check if valid</returns>
    SONGDOWNLOADER_EXPORT std::vector<uint8_t> GetCoverImage(const ScoreSaber::Leaderboard& ldb);

    /// <summary>
    /// Retrieves songs that are Trending as determined by ScoreSaber.
    /// </summary>
    /// <param name="finished">Callback to run after receiving the data</param>
    /// <param name="ranked">If return value should contain only ranked or be mixed default: false</param>
    /// <param name="query">Searh Query</param>
    /// <param name="pageIndex">Index of the pages search results default: 0</param>
    /// <param name="amount">Amount of Songs to get default: 20</param>
    [[deprecated("Use GetListAsync(ListCategory list, std::function<void(std::optional<ScoreSaber::Page>)> finished, std::optional<bool> ranked, std::optional<bool> qualified, std::optional<bool> unique, int pageIndex) instead!")]]
    SONGDOWNLOADER_EXPORT void GetTrendingAsync(std::function<void(std::optional<ScoreSaber::Page>)> finished, bool ranked = false, int pageIndex = 0, int amount = 20);

    /// <summary>
    /// Retrieves the latest ranked songs.
    /// </summary>
    /// <param name="finished">Callback to run after receiving the data</param>
    /// <param name="ranked">If return value should contain only ranked or be mixed default: false</param>
    /// <param name="query">Searh Query</param>
    /// <param name="pageIndex">Index of the pages search results default: 0</param>
    /// <param name="amount">Amount of Songs to get default: 20</param>
    [[deprecated("Use GetListAsync(ListCategory list, std::function<void(std::optional<ScoreSaber::Page>)> finished, std::optional<bool> ranked, std::optional<bool> qualified, std::optional<bool> unique, int pageIndex) instead!")]]
    SONGDOWNLOADER_EXPORT void GetLatestRankedAsync(std::function<void(std::optional<ScoreSaber::Page>)> finished, bool ranked = false, int pageIndex = 0, int amount = 20);

    /// <summary>
    /// Retrieves the songs that have the most plays as determined by ScoreSaber.
    /// </summary>
    /// <param name="finished">Callback to run after receiving the data</param>
    /// <param name="ranked">If return value should contain only ranked or be mixed default: false</param>
    /// <param name="query">Searh Query</param>
    /// <param name="pageIndex">Index of the pages search results default: 0</param>
    /// <param name="amount">Amount of Songs to get default: 20</param>
    [[deprecated("Use GetListAsync(ListCategory list, std::function<void(std::optional<ScoreSaber::Page>)> finished, std::optional<bool> ranked, std::optional<bool> qualified, std::optional<bool> unique, int pageIndex) instead!")]]
    SONGDOWNLOADER_EXPORT void GetTopPlayedAsync(std::function<void(std::optional<ScoreSaber::Page>)> finished, bool ranked = false, int pageIndex = 0, int amount = 20);

    /// <summary>
    /// Retrieves the songs that have the most plays as determined by ScoreSaber.
    /// </summary>
    /// <param name="finished">Callback to run after receiving the data</param>
    /// <param name="ranked">If return value should contain only ranked or be mixed default: false</param>
    /// <param name="query">Searh Query</param>
    /// <param name="pageIndex">Index of the pages search results default: 0</param>
    /// <param name="amount">Amount of Songs to get default: 20</param>
    [[deprecated("Use GetListAsync(ListCategory list, std::function<void(std::optional<ScoreSaber::Page>)> finished, std::optional<bool> ranked, std::optional<bool> qualified, std::optional<bool> unique, int pageIndex) instead!")]]
    SONGDOWNLOADER_EXPORT void GetTopRankedAsync(std::function<void(std::optional<ScoreSaber::Page>)> finished, bool ranked = false, int pageIndex = 0, int amount = 20);

    /// <summary>
    /// Retrieves the list specified in SearchType
    /// </summary>
    /// <param name="list">Category to seach in</param>
    /// <param name="finished">Callback to run after receiving the data</param>
    /// <param name="ranked">If return value should contain ranked, mixed or non-ranked default: std::nullopt (mixed)</param>
    /// <param name="qualified">If return value should contain qualified, mixed or non-qualified default: std::nullopt (mixed)</param>
    /// <param name="unique">Only return one leaderboard of each id</param>
    /// <param name="pageIndex">Index of the pages search results default: 0</param>
    SONGDOWNLOADER_EXPORT void GetListAsync(ListCategory list, std::function<void(std::optional<ScoreSaber::Leaderboards>)> finished, std::optional<bool> ranked = std::nullopt, std::optional<bool> qualified = std::nullopt, std::optional<bool> unique = std::nullopt, int pageIndex = 0);

    /// <summary>
    /// [Deprecated] Searches on ScoreSaber
    /// </summary>
    /// <param name="query">Searh Query</param>
    /// <param name="finished"></param>
    /// <param name="ranked">If return value should contain only ranked or be mixed default: false</param>
    /// <param name="pageIndex">Index of the pages search results default: 0</param>
    /// <param name="amount">Amount of Songs to get default: 20</param>
    [[deprecated("Use SearchAsync(std::string query, ListCategory list, std::function<void(std::optional<ScoreSaber::Page>)> finished, std::optional<bool> ranked, std::optional<bool> qualified, std::optional<bool> unique, int pageIndex) instead!")]]
    SONGDOWNLOADER_EXPORT void SearchSSAsync(std::string query, SearchType list, std::function<void(std::optional<ScoreSaber::Page>)> finished, bool ranked = false, int pageIndex = 0, int amount = 20);

    /// <summary>
    /// Searches on ScoreSaber
    /// </summary>
    /// <param name="query">Searh Query</param>
    /// <param name="list">Category to seach in</param>
    /// <param name="finished"></param>
    /// <param name="ranked">If return value should contain only ranked, be mixed or exclude ranked default: std::nullopt</param>
    /// <param name="qualified">If return value should contain only qualified, be mixed or exclude qualified default: std::nullopt</param>
    /// <param name="unique">If return value should contain each leaderboard only once or not default: std::nullopt</param>
    /// <param name="pageIndex">Index of the pages search results default: 0</param>
    /// <param name="amount">Amount of Songs to get default: 20</param>
    SONGDOWNLOADER_EXPORT void SearchAsync(std::string query, ListCategory list, std::function<void(std::optional<ScoreSaber::Leaderboards>)> finished, std::optional<bool> ranked =  std::nullopt, std::optional<bool> qualified = std::nullopt, std::optional<bool> unique = std::nullopt, int pageIndex = 1);

    /// <summary>
    /// Gets CoverImage from ScoreSaber song
    /// </summary>
    /// <param name="song">Reference to a ScoreSaber Song</param>
    /// <param name="finished"></param>
    /// <param name="progressUpdate"></param>
    SONGDOWNLOADER_EXPORT void GetCoverImageAsync(const ScoreSaber::Song& song, std::function<void(std::vector<uint8_t>)> finished, std::function<void(float)> progressUpdate = nullptr);

    /// <summary>
    /// Gets CoverImage from ScoreSaber song
    /// </summary>
    /// <param name="song">Reference to a ScoreSaber Song</param>
    /// <param name="finished"></param>
    /// <param name="progressUpdate"></param>
    SONGDOWNLOADER_EXPORT void GetCoverImageAsync(const ScoreSaber::Leaderboard& ldb, std::function<void(std::vector<uint8_t>)> finished, std::function<void(float)> progressUpdate = nullptr);

    /// <summary>
    /// String containing any exceptions that were encountered during the request, empty otherwise
    /// </summary>
    // Legacy ABI; internal callers use the synchronized snapshot below.
    SONGDOWNLOADER_EXPORT extern std::string exception;
    SONGDOWNLOADER_EXPORT std::string GetException();
}
