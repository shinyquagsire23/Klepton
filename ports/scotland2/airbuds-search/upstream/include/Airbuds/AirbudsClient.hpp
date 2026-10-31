#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "web-utils/shared/WebUtils.hpp"

#include "Configuration.hpp"
#include "Log.hpp"
#include "Airbuds/Friend.hpp"
#include "Airbuds/Playlist.hpp"
#include "Airbuds/Track.hpp"
#include "Airbuds/Json.hpp"
#include "Airbuds/Utils.hpp"

namespace airbuds {

class Client {

    public:
    std::vector<PlaylistTrack> getRecentlyPlayed();
    std::vector<PlaylistTrack> getRecentlyPlayedCachedOnly();

    std::vector<Friend> getFriends();
    std::vector<PlaylistTrack> getRecentlyPlayedForUser(const std::string& userId);
    std::vector<PlaylistTrack> getRecentlyPlayedCachedOnlyForUser(const std::string& userId);
    std::vector<PlaylistTrack> getPlaylistTracksForUser(std::string_view userId, std::string_view playlistId);

    std::vector<PlaylistTrack> getPlaylistTracks(std::string_view playlistId);

    std::vector<Playlist> getPlaylists();
    std::vector<Playlist> getPlaylistsCachedOnly();
    std::vector<Playlist> getPlaylistsForUser(const std::string& userId);
    std::vector<Playlist> getPlaylistsCachedOnlyForUser(const std::string& userId);
    Playlist getRecentlyPlayedPlaylist();

    std::string getLastRecentlyPlayedWarning() const;
    void resetAirbudsCredentials();

    private:
    static constexpr size_t AIRBUDS_PAGE_LIMIT = 30;

    struct AirbudsCredentials {
        std::string accessToken;
        std::string userId;
    };

    std::string airbudsAccessToken_;
    std::string airbudsUserId_;
    std::optional<std::chrono::system_clock::time_point> airbudsAccessTokenExpiry_;

    std::string lastRecentlyPlayedWarning_;
    std::vector<PlaylistTrack> cachedRecentlyPlayedTracks_;
    std::unordered_map<std::string, std::vector<PlaylistTrack>> cachedFriendRecentlyPlayedTracks_;

    std::optional<AirbudsCredentials> getAirbudsCredentials();
    bool isAirbudsAccessTokenValid() const;
    void refreshAirbudsAccessToken(const std::string& refreshToken);

    rapidjson::Document apiGetRecentlyPlayed(
        const AirbudsCredentials& credentials,
        const std::string& userId,
        const std::optional<std::string>& cursor,
        size_t limit);
    rapidjson::Document apiGetFriends(const AirbudsCredentials& credentials);

    std::vector<PlaylistTrack> getRecentlyPlayedTracks();
    std::vector<PlaylistTrack> getRecentlyPlayedTracksForUser(
        const std::string& userId,
        std::vector<PlaylistTrack>* cachedTracks,
        const std::filesystem::path& cachePath);
};

} // namespace airbuds
