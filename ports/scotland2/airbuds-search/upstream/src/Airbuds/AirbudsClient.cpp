#include "Airbuds/AirbudsClient.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include "search-port-support.hpp"

#include <web-utils/shared/WebUtils.hpp>

#include "Log.hpp"
#include "Airbuds/Json.hpp"
#include "Airbuds/Track.hpp"
#include "Airbuds/Utils.hpp"
#include "Utils.hpp"

using namespace airbuds::json;

namespace {

constexpr std::string_view AIRBUDS_RECENTLY_PLAYED_QUERY =
    "query UserRecentlyPlayed($id: ID!, $limit: Int!, $cursor: Cursor) { userWithID(id: $id) { __typename ...UserFields recentlyPlayed(limit: $limit, cursor: $cursor, query: { maxVisibility: PRIVATE } ) { __typename items { __typename id playedAtMax playCount object { __typename ...MusicObjectFields } highlight { __typename at } userMusicStatus { __typename ...UserMusicStatusFields } feedActivity { __typename id activityUrl } visibility } pageInfo { __typename hasNextPage endCursor } } id } }  fragment UserFields on User { __typename id identifier profileURL displayName imageUrl }  fragment MusicObjectFields on MusicObject { __typename id kind openable { __typename id provider artworkURL artists { __typename id artworkURL } name artistName audioPreviewURL uri } }  fragment UserMusicStatusFields on UserMusicStatus { __typename emoji text }";
constexpr std::string_view AIRBUDS_FRIEND_LIST_QUERY =
    "query FriendList { me { __typename id friends(limit: 1000) { __typename items { __typename ...UserFriendshipFields id } } } }  fragment UserFields on User { __typename id identifier profileURL displayName imageUrl }  fragment UserFriendshipFields on UserFriendship { __typename id withUserId status createdAt acceptedAt ignoredAt hasBffedAt isBffedAt withUser { __typename ...UserFields id } }";
constexpr std::string_view AIRBUDS_ACCEPT_HEADER =
    "multipart/mixed;deferSpec=20220824, application/graphql-response+json, application/json";
constexpr std::string_view AIRBUDS_USER_AGENT = "Poplive/163 Android/16";
constexpr std::string_view AIRBUDS_GRAPHQL_ENDPOINT = "https://graph-ilsfyhvrya-uc.a.run.app/query";
constexpr std::string_view AIRBUDS_REFRESH_ENDPOINT = "https://accounts-ilsfyhvrya-uc.a.run.app/refresh";
constexpr std::string_view AIRBUDS_REFRESH_CONTENT_TYPE = "application/json; charset=utf-8";
constexpr std::string_view FRIEND_HISTORY_CACHE_DIR = "friend_recently_played";

struct AccumulatingStringResponse : public WebUtils::GenericResponse<std::string> {
    size_t totalBytes = 0;

    bool AcceptData(std::span<uint8_t const> data) override {
        if (data.size() > (8u << 20) - totalBytes) return false;
        totalBytes += data.size();
        if (!responseData) {
            responseData = std::string();
        }
        responseData->append(reinterpret_cast<const char*>(data.data()), data.size());
        return true;
    }
};

std::string trim(std::string_view value) {
    size_t start = 0;
    size_t end = value.size();
    while (start < end && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(start, end - start));
}

std::vector<airbuds::Artist> parseArtistsFromName(std::string_view artistName) {
    std::vector<airbuds::Artist> artists;
    size_t start = 0;
    while (start < artistName.size()) {
        size_t end = artistName.find(',', start);
        if (end == std::string_view::npos) {
            end = artistName.size();
        }
        const std::string name = trim(artistName.substr(start, end - start));
        if (!name.empty()) {
            airbuds::Artist artist;
            artist.id = "";
            artist.name = name;
            artists.push_back(artist);
        }
        start = end + 1;
    }
    if (artists.empty() && !artistName.empty()) {
        airbuds::Artist artist;
        artist.id = "";
        artist.name = std::string(artistName);
        artists.push_back(artist);
    }
    return artists;
}

std::chrono::milliseconds parseIso8601ToMillis(std::string_view iso) {
    if (iso.size() < 20) {
        return std::chrono::milliseconds(0);
    }

    auto parseNumber = [&iso](size_t pos, size_t count, int& value) -> bool {
        if (pos + count > iso.size()) {
            return false;
        }
        int result = 0;
        for (size_t i = 0; i < count; ++i) {
            const char c = iso[pos + i];
            if (c < '0' || c > '9') {
                return false;
            }
            result = (result * 10) + (c - '0');
        }
        value = result;
        return true;
    };

    if (iso[4] != '-' || iso[7] != '-' || iso[10] != 'T' || iso[13] != ':' || iso[16] != ':') {
        return std::chrono::milliseconds(0);
    }

    std::tm t{};
    int millis = 0;
    if (!parseNumber(0, 4, t.tm_year)
        || !parseNumber(5, 2, t.tm_mon)
        || !parseNumber(8, 2, t.tm_mday)
        || !parseNumber(11, 2, t.tm_hour)
        || !parseNumber(14, 2, t.tm_min)
        || !parseNumber(17, 2, t.tm_sec)) {
        return std::chrono::milliseconds(0);
    }

    size_t pos = 19;
    if (pos < iso.size() && iso[pos] == '.') {
        size_t start = pos + 1;
        size_t end = start;
        while (end < iso.size() && iso[end] >= '0' && iso[end] <= '9') {
            ++end;
        }
        const size_t digits = end - start;
        if (digits > 0) {
            int value = 0;
            const size_t digitsToRead = std::min<size_t>(digits, 3);
            for (size_t i = 0; i < digitsToRead; ++i) {
                value = (value * 10) + (iso[start + i] - '0');
            }
            if (digitsToRead == 1) {
                value *= 100;
            } else if (digitsToRead == 2) {
                value *= 10;
            }
            millis = value;
        }
        pos = end;
    }

    if (pos >= iso.size() || iso[pos] != 'Z') {
        return std::chrono::milliseconds(0);
    }

    t.tm_year -= 1900;
    t.tm_mon -= 1;
    t.tm_isdst = 0;

    std::time_t seconds = timegm(&t);
    const long long totalMillis = (static_cast<long long>(seconds) * 1000LL) + millis;
    return std::chrono::milliseconds(totalMillis);
}

bool getLocalDateKey(const std::chrono::milliseconds& millis, std::string& output) {
    if (millis.count() <= 0) {
        return false;
    }

    const std::time_t seconds = std::chrono::duration_cast<std::chrono::seconds>(millis).count();
    std::tm localTime{};
#if defined(_WIN32)
    if (localtime_s(&localTime, &seconds) != 0) {
        return false;
    }
#else
    if (!localtime_r(&seconds, &localTime)) {
        return false;
    }
#endif

    char buffer[16];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%04d-%02d-%02d",
        localTime.tm_year + 1900,
        localTime.tm_mon + 1,
        localTime.tm_mday);
    output = buffer;
    return true;
}

std::string getTodayKey() {
    const std::time_t now = std::time(nullptr);
    std::tm localTime{};
#if defined(_WIN32)
    localtime_s(&localTime, &now);
#else
    localtime_r(&now, &localTime);
#endif

    char buffer[16];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%04d-%02d-%02d",
        localTime.tm_year + 1900,
        localTime.tm_mon + 1,
        localTime.tm_mday);
    return buffer;
}

std::vector<airbuds::Playlist> buildPlaylistsFromTracks(const std::vector<airbuds::PlaylistTrack>& tracks) {
    std::vector<airbuds::Playlist> playlists;
    if (tracks.empty()) {
        return playlists;
    }

    const std::string todayKey = getTodayKey();
    std::unordered_map<std::string, size_t> indexByKey;
    indexByKey.reserve(tracks.size());

    for (const auto& track : tracks) {
        std::string dateKey;
        std::string label;
        if (!getLocalDateKey(track.dateAdded_, dateKey)) {
            dateKey = "unknown";
            label = "Unknown Date";
        } else {
            label = (dateKey == todayKey) ? "Today" : dateKey;
        }

        size_t index = 0;
        auto existing = indexByKey.find(dateKey);
        if (existing == indexByKey.end()) {
            airbuds::Playlist playlist;
            playlist.id = dateKey;
            playlist.name = label;
            playlist.totalItemCount = 0;
            playlist.imageUrl = track.album.url;
            playlists.push_back(playlist);
            index = playlists.size() - 1;
            indexByKey.emplace(dateKey, index);
        } else {
            index = existing->second;
        }

        playlists[index].totalItemCount += 1;
        if (playlists[index].imageUrl.empty() && !track.album.url.empty()) {
            playlists[index].imageUrl = track.album.url;
        }
    }

    return playlists;
}

std::string getOptionalString(const rapidjson::Value& json, const char* key);

struct RecentlyPlayedCache {
    std::vector<airbuds::PlaylistTrack> tracks;
    std::chrono::milliseconds oldestTimestamp{0};
};

std::filesystem::path getRecentlyPlayedCachePath() {
    return AirbudsSearch::getDataDirectory() / "recently_played_cache.json";
}

std::string sanitizeCacheKey(std::string_view value) {
    std::string output;
    output.reserve(value.size());
    for (const char c : value) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if ((uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z') || (uc >= '0' && uc <= '9') || uc == '-' || uc == '_') {
            output.push_back(static_cast<char>(uc));
        } else {
            output.push_back('_');
        }
    }
    return output;
}

std::filesystem::path getFriendRecentlyPlayedCacheDirectory() {
    const std::filesystem::path path = AirbudsSearch::getDataDirectory() / std::string(FRIEND_HISTORY_CACHE_DIR);
    if (!std::filesystem::exists(path)) {
        std::filesystem::create_directories(path);
    }
    return path;
}

std::filesystem::path getFriendRecentlyPlayedCachePath(std::string_view friendId) {
    std::string safeId = sanitizeCacheKey(friendId);
    if (safeId.empty()) {
        safeId = "unknown";
    }
    return getFriendRecentlyPlayedCacheDirectory() / std::format("recently_played_{}.json", safeId);
}

std::string makeRecentlyPlayedKey(const airbuds::PlaylistTrack& track) {
    const long long millis = track.dateAdded_.count();
    if (millis > 0) {
        return track.id + "|" + std::to_string(millis);
    }
    return track.id + "|" + track.dateAdded;
}

RecentlyPlayedCache loadRecentlyPlayedCache(const std::filesystem::path& path) {
    RecentlyPlayedCache cache;
    if (!std::filesystem::exists(path)) {
        return cache;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        AirbudsSearch::Log.warn("Failed to open recently played cache: {}", path.string());
        return cache;
    }

    std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.empty()) {
        return cache;
    }

    rapidjson::Document document;
    document.Parse(data.c_str());
    if (!document.IsObject() || !document.HasMember("tracks") || !document["tracks"].IsArray()) {
        AirbudsSearch::Log.warn("Recently played cache is invalid: {}", path.string());
        return cache;
    }

    const auto& items = document["tracks"].GetArray();
    cache.tracks.reserve(items.Size());
    for (const auto& item : items) {
        if (!item.IsObject()) {
            continue;
        }

        airbuds::PlaylistTrack track;
        track.id = getOptionalString(item, "id");
        track.name = getOptionalString(item, "name");
        track.album.url = getOptionalString(item, "albumUrl");

        if (item.HasMember("artists") && item["artists"].IsArray()) {
            for (const auto& artistJson : item["artists"].GetArray()) {
                if (artistJson.IsString()) {
                    airbuds::Artist artist;
                    artist.name = artistJson.GetString();
                    track.artists.push_back(artist);
                } else if (artistJson.IsObject()) {
                    airbuds::Artist artist;
                    artist.name = getOptionalString(artistJson, "name");
                    if (!artist.name.empty()) {
                        track.artists.push_back(artist);
                    }
                }
            }
        }

        track.dateAdded = getOptionalString(item, "playedAt");
        if (item.HasMember("playedAtMs") && item["playedAtMs"].IsInt64()) {
            track.dateAdded_ = std::chrono::milliseconds(item["playedAtMs"].GetInt64());
        } else if (!track.dateAdded.empty()) {
            track.dateAdded_ = parseIso8601ToMillis(track.dateAdded);
        } else {
            track.dateAdded_ = std::chrono::milliseconds(0);
        }

        if (track.id.empty() || track.name.empty()) {
            continue;
        }
        cache.tracks.push_back(track);
        if (track.dateAdded_.count() > 0) {
            if (cache.oldestTimestamp.count() == 0 || track.dateAdded_ < cache.oldestTimestamp) {
                cache.oldestTimestamp = track.dateAdded_;
            }
        }
    }

    if (!cache.tracks.empty()) {
        std::stable_sort(cache.tracks.begin(), cache.tracks.end(), [](const auto& left, const auto& right) {
            const auto leftMillis = left.dateAdded_.count();
            const auto rightMillis = right.dateAdded_.count();
            if (leftMillis == 0 && rightMillis == 0) {
                return false;
            }
            if (leftMillis == 0) {
                return false;
            }
            if (rightMillis == 0) {
                return true;
            }
            return leftMillis > rightMillis;
        });
    }

    return cache;
}

RecentlyPlayedCache loadRecentlyPlayedCache() {
    return loadRecentlyPlayedCache(getRecentlyPlayedCachePath());
}

void saveRecentlyPlayedCache(const std::filesystem::path& path, const std::vector<airbuds::PlaylistTrack>& tracks) {

    rapidjson::Document document;
    document.SetObject();
    auto& allocator = document.GetAllocator();
    document.AddMember("version", 1, allocator);

    rapidjson::Value items(rapidjson::kArrayType);
    for (const auto& track : tracks) {
        if (track.id.empty() || track.name.empty()) {
            continue;
        }

        rapidjson::Value item(rapidjson::kObjectType);
        item.AddMember("id", rapidjson::Value(track.id.c_str(), allocator), allocator);
        item.AddMember("name", rapidjson::Value(track.name.c_str(), allocator), allocator);
        if (!track.album.url.empty()) {
            item.AddMember("albumUrl", rapidjson::Value(track.album.url.c_str(), allocator), allocator);
        }
        if (!track.dateAdded.empty()) {
            item.AddMember("playedAt", rapidjson::Value(track.dateAdded.c_str(), allocator), allocator);
        }
        if (track.dateAdded_.count() > 0) {
            item.AddMember("playedAtMs", static_cast<int64_t>(track.dateAdded_.count()), allocator);
        }

        rapidjson::Value artists(rapidjson::kArrayType);
        for (const auto& artist : track.artists) {
            if (!artist.name.empty()) {
                artists.PushBack(rapidjson::Value(artist.name.c_str(), allocator), allocator);
            }
        }
        item.AddMember("artists", artists, allocator);

        items.PushBack(item, allocator);
    }
    document.AddMember("tracks", items, allocator);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    document.Accept(writer);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        AirbudsSearch::Log.warn("Failed to write recently played cache: {}", path.string());
        return;
    }
    file.write(buffer.GetString(), static_cast<std::streamsize>(buffer.GetSize()));
}

void saveRecentlyPlayedCache(const std::vector<airbuds::PlaylistTrack>& tracks) {
    saveRecentlyPlayedCache(getRecentlyPlayedCachePath(), tracks);
}

std::string getOptionalString(const rapidjson::Value& json, const char* key) {
    if (!json.HasMember(key)) {
        return "";
    }
    const auto& value = json[key];
    if (!value.IsString()) {
        return "";
    }
    return value.GetString();
}

std::string_view trimLeadingWhitespaceAndBom(std::string_view text) {
    if (text.size() >= 3
        && static_cast<unsigned char>(text[0]) == 0xEF
        && static_cast<unsigned char>(text[1]) == 0xBB
        && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    while (!text.empty()) {
        const unsigned char c = static_cast<unsigned char>(text.front());
        if (!std::isspace(c)) {
            break;
        }
        text.remove_prefix(1);
    }
    return text;
}

std::optional<std::string> extractFirstJsonObject(std::string_view text) {
    bool inString = false;
    bool escaped = false;
    int depth = 0;
    size_t start = std::string_view::npos;

    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (inString) {
            if (escaped) {
                escaped = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                continue;
            }
            if (c == '"') {
                inString = false;
            }
            continue;
        }

        if (c == '"') {
            inString = true;
            continue;
        }
        if (c == '{') {
            if (depth == 0) {
                start = i;
            }
            ++depth;
            continue;
        }
        if (c == '}') {
            if (depth == 0) {
                continue;
            }
            --depth;
            if (depth == 0 && start != std::string_view::npos) {
                return std::string(text.substr(start, i - start + 1));
            }
        }
    }

    return std::nullopt;
}

std::string getHeaderValue(std::string_view headers, std::string_view key) {
    const std::string loweredKey = std::string(key) + ":";
    std::string lowerHeaders(headers);
    std::transform(lowerHeaders.begin(), lowerHeaders.end(), lowerHeaders.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    const std::string loweredKeyLower = [&]() {
        std::string value(loweredKey);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }();

    size_t pos = lowerHeaders.find(loweredKeyLower);
    if (pos == std::string::npos) {
        return "";
    }
    pos += loweredKeyLower.size();
    size_t end = lowerHeaders.find('\n', pos);
    if (end == std::string::npos) {
        end = lowerHeaders.size();
    }
    std::string value = std::string(headers.substr(pos, end - pos));
    return trim(value);
}

AccumulatingStringResponse postJsonWithWebUtils(
    std::string_view url,
    const WebUtils::URLOptions::HeaderMap& headers,
    const std::string& body,
    std::string_view userAgent
) {
    WebUtils::URLOptions options(
        std::string(url),
        WebUtils::URLOptions::QueryMap{},
        headers,
        true,
        "",
        std::string(userAgent),
        10);
    return WebUtils::Post<AccumulatingStringResponse>(options, AirbudsSearch::Utils::toSpan(body));
}

template <typename ResponseT>
size_t getResponseByteCount(const ResponseT& response) {
    if constexpr (requires { response.totalBytes; }) {
        return response.totalBytes;
    }
    if (response.responseData) {
        return response.responseData->size();
    }
    return 0;
}

template <typename ResponseT>
rapidjson::Document parseJsonPayload(const ResponseT& response, std::string_view context) {
    if (!response.IsSuccessful()) {
        if (response.responseData) {
            throw std::runtime_error(std::format("API ERROR: code = {} data = {}", response.httpCode, *response.responseData));
        }
        throw std::runtime_error(std::format("API ERROR: code = {}", response.httpCode));
    }
    if (!response.responseData) {
        throw std::runtime_error(std::format("API ERROR: Request successful but no data! code = {}", response.httpCode));
    }

    std::string payload = *response.responseData;
    if (payload.empty()) {
        const std::string contentType = getHeaderValue(response.responseHeaders, "content-type");
        const std::string contentEncoding = getHeaderValue(response.responseHeaders, "content-encoding");
        const std::string contentLength = getHeaderValue(response.responseHeaders, "content-length");
        const size_t byteCount = getResponseByteCount(response);
        AirbudsSearch::Log.error(
            "Airbuds API empty body (context={}, http={}, curl={}, bytes={}, content-type={}, content-encoding={}, content-length={})",
            context,
            response.httpCode,
            response.curlStatus,
            byteCount,
            contentType.empty() ? "unknown" : contentType,
            contentEncoding.empty() ? "unknown" : contentEncoding,
            contentLength.empty() ? "unknown" : contentLength);
        throw std::runtime_error(std::format("API ERROR: Request successful but empty body! code = {}", response.httpCode));
    }

    std::string_view view = trimLeadingWhitespaceAndBom(payload);
    rapidjson::Document document;
    document.Parse(view.data(), view.size());
    if (!document.HasParseError()) {
        return document;
    }

    const std::optional<std::string> extracted = extractFirstJsonObject(view);
    if (extracted) {
        rapidjson::Document fallback;
        fallback.Parse(extracted->data(), extracted->size());
        if (!fallback.HasParseError()) {
            return fallback;
        }
    }

    const std::string contentType = getHeaderValue(response.responseHeaders, "content-type");
    const std::string contentEncoding = getHeaderValue(response.responseHeaders, "content-encoding");
    throw std::runtime_error(std::format(
        "Airbuds API response not JSON (context={}, content-type={}, content-encoding={}).",
        context,
        contentType.empty() ? "unknown" : contentType,
        contentEncoding.empty() ? "unknown" : contentEncoding));
}

std::string base64UrlDecode(std::string_view input) {
    return sc2_search::base64_url_decode(input);
}

std::string getJwtClaim(std::string_view token, std::string_view claim) {
    const size_t firstDot = token.find('.');
    if (firstDot == std::string_view::npos) {
        return "";
    }
    const size_t secondDot = token.find('.', firstDot + 1);
    if (secondDot == std::string_view::npos) {
        return "";
    }
    const std::string payload = base64UrlDecode(token.substr(firstDot + 1, secondDot - firstDot - 1));
    if (payload.empty()) {
        return "";
    }
    rapidjson::Document document;
    if (document.Parse(payload.c_str()).HasParseError()) {
        return "";
    }
    if (!document.HasMember(claim.data()) || !document[claim.data()].IsString()) {
        return "";
    }
    return document[claim.data()].GetString();
}

std::string getJwtUserId(std::string_view token) {
    std::string userId = getJwtClaim(token, "usr");
    if (!userId.empty()) {
        return userId;
    }
    return getJwtClaim(token, "sub");
}

}

namespace airbuds {

std::optional<airbuds::Client::AirbudsCredentials> airbuds::Client::getAirbudsCredentials() {
    const std::string refreshToken = AirbudsSearch::getAirbudsRefreshToken();
    if (refreshToken.empty()) {
        return std::nullopt;
    }

    if (!isAirbudsAccessTokenValid()) {
        refreshAirbudsAccessToken(refreshToken);
    }
    if (airbudsAccessToken_.empty()) {
        return std::nullopt;
    }

    AirbudsCredentials credentials;
    credentials.accessToken = airbudsAccessToken_;
    credentials.userId = airbudsUserId_;
    if (credentials.userId.empty()) {
        credentials.userId = getJwtUserId(airbudsAccessToken_);
    }
    if (credentials.userId.empty()) {
        throw std::runtime_error("Airbuds API userId is missing.");
    }

    return credentials;
}

bool airbuds::Client::isAirbudsAccessTokenValid() const {
    if (airbudsAccessToken_.empty()) {
        return false;
    }
    if (!airbudsAccessTokenExpiry_) {
        return false;
    }
    const auto now = std::chrono::system_clock::now();
    return now + std::chrono::seconds(30) < *airbudsAccessTokenExpiry_;
}

void airbuds::Client::refreshAirbudsAccessToken(const std::string& refreshToken) {
    rapidjson::Document requestJson;
    requestJson.SetObject();
    rapidjson::Document::AllocatorType& allocator = requestJson.GetAllocator();
    requestJson.AddMember("refreshToken", rapidjson::Value(refreshToken.c_str(), allocator), allocator);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    requestJson.Accept(writer);
    const std::string body = buffer.GetString();

    const WebUtils::URLOptions::HeaderMap headers = {
        {"Content-Type", std::string(AIRBUDS_REFRESH_CONTENT_TYPE)},
    };

    const AccumulatingStringResponse response =
        postJsonWithWebUtils(AIRBUDS_REFRESH_ENDPOINT, headers, body, AIRBUDS_USER_AGENT);
    const rapidjson::Document document = parseJsonPayload(response, "airbuds-refresh");
    const std::string accessToken = getString(document, "accessToken");
    if (accessToken.empty()) {
        throw std::runtime_error("Airbuds refresh response missing accessToken.");
    }

    airbudsAccessToken_ = accessToken;
    airbudsUserId_ = getJwtUserId(accessToken);

    const std::string expires = getString(document, "expires");
    if (!expires.empty()) {
        const std::chrono::milliseconds expiryMillis = parseIso8601ToMillis(expires);
        if (expiryMillis.count() > 0) {
            airbudsAccessTokenExpiry_ = std::chrono::system_clock::time_point(expiryMillis);
        } else {
            airbudsAccessTokenExpiry_.reset();
        }
    } else {
        airbudsAccessTokenExpiry_.reset();
    }
}

void airbuds::Client::resetAirbudsCredentials() {
    airbudsAccessToken_.clear();
    airbudsUserId_.clear();
    airbudsAccessTokenExpiry_.reset();
    lastRecentlyPlayedWarning_.clear();
    cachedFriendRecentlyPlayedTracks_.clear();
}

std::vector<Friend> Client::getFriends() {
    std::vector<Friend> friends;
    const auto credentials = getAirbudsCredentials();
    if (!credentials) {
        throw std::runtime_error("Airbuds refresh token is missing.");
    }

    const rapidjson::Document document = apiGetFriends(*credentials);
    if (!document.HasMember("data") || !document["data"].IsObject()) {
        throw std::runtime_error("Airbuds API response missing data.");
    }
    const auto& data = document["data"];
    if (!data.HasMember("me") || !data["me"].IsObject()) {
        throw std::runtime_error("Airbuds API response missing user.");
    }
    const auto& me = data["me"];
    if (!me.HasMember("friends") || !me["friends"].IsObject()) {
        throw std::runtime_error("Airbuds API response missing friends.");
    }
    const auto& friendsJson = me["friends"];
    if (!friendsJson.HasMember("items") || !friendsJson["items"].IsArray()) {
        throw std::runtime_error("Airbuds API response missing friends items.");
    }

    std::unordered_set<std::string> seen;
    const auto& items = friendsJson["items"].GetArray();
    for (const auto& item : items) {
        if (!item.IsObject()) {
            continue;
        }
        const std::string status = getOptionalString(item, "status");
        if (status != "ACKNOWLEDGED") {
            continue;
        }

        std::string friendId = getOptionalString(item, "withUserId");
        const rapidjson::Value* withUser = nullptr;
        if (item.HasMember("withUser") && item["withUser"].IsObject()) {
            withUser = &item["withUser"];
        }
        if (friendId.empty() && withUser) {
            friendId = getOptionalString(*withUser, "id");
        }
        if (friendId.empty()) {
            continue;
        }

        if (!seen.insert(friendId).second) {
            continue;
        }

        Friend friendUser;
        friendUser.id = friendId;
        if (withUser) {
            friendUser.identifier = getOptionalString(*withUser, "identifier");
            friendUser.displayName = getOptionalString(*withUser, "displayName");
        }
        friends.push_back(std::move(friendUser));
    }

    if (!friends.empty()) {
        std::sort(friends.begin(), friends.end(), [](const Friend& left, const Friend& right) {
            const std::string leftName = left.displayName.empty() ? left.identifier : left.displayName;
            const std::string rightName = right.displayName.empty() ? right.identifier : right.displayName;
            return AirbudsSearch::Utils::toLowerCase(leftName) < AirbudsSearch::Utils::toLowerCase(rightName);
        });
    }

    return friends;
}

std::vector<PlaylistTrack> Client::getRecentlyPlayed() {
    return getRecentlyPlayedTracks();
}

std::vector<PlaylistTrack> Client::getRecentlyPlayedCachedOnly() {
    if (!cachedRecentlyPlayedTracks_.empty()) {
        return cachedRecentlyPlayedTracks_;
    }
    RecentlyPlayedCache cache = loadRecentlyPlayedCache();
    cachedRecentlyPlayedTracks_ = cache.tracks;
    return cachedRecentlyPlayedTracks_;
}

std::vector<PlaylistTrack> Client::getRecentlyPlayedForUser(const std::string& userId) {
    if (userId.empty()) {
        return getRecentlyPlayedTracks();
    }
    const std::filesystem::path cachePath = getFriendRecentlyPlayedCachePath(userId);
    return getRecentlyPlayedTracksForUser(userId, &cachedFriendRecentlyPlayedTracks_[userId], cachePath);
}

std::vector<PlaylistTrack> Client::getRecentlyPlayedCachedOnlyForUser(const std::string& userId) {
    if (userId.empty()) {
        return getRecentlyPlayedCachedOnly();
    }
    auto existing = cachedFriendRecentlyPlayedTracks_.find(userId);
    if (existing != cachedFriendRecentlyPlayedTracks_.end() && !existing->second.empty()) {
        return existing->second;
    }
    RecentlyPlayedCache cache = loadRecentlyPlayedCache(getFriendRecentlyPlayedCachePath(userId));
    cachedFriendRecentlyPlayedTracks_[userId] = cache.tracks;
    return cachedFriendRecentlyPlayedTracks_[userId];
}

std::vector<PlaylistTrack> Client::getPlaylistTracks(const std::string_view playlistId) {
    std::vector<PlaylistTrack> tracks = getRecentlyPlayedCachedOnly();
    if (tracks.empty()) {
        return tracks;
    }

    if (playlistId.empty() || playlistId == "airbuds-recent") {
        return tracks;
    }

    std::vector<PlaylistTrack> filtered;
    filtered.reserve(tracks.size());
    for (const auto& track : tracks) {
        std::string key;
        if (!getLocalDateKey(track.dateAdded_, key)) {
            if (playlistId == "unknown") {
                filtered.push_back(track);
            }
            continue;
        }
        if (key == playlistId) {
            filtered.push_back(track);
        }
    }
    return filtered;
}

std::vector<PlaylistTrack> Client::getPlaylistTracksForUser(std::string_view userId, std::string_view playlistId) {
    if (userId.empty()) {
        return getPlaylistTracks(playlistId);
    }

    std::vector<PlaylistTrack> tracks = getRecentlyPlayedCachedOnlyForUser(std::string(userId));
    if (tracks.empty()) {
        return tracks;
    }

    if (playlistId.empty() || playlistId == "airbuds-recent") {
        return tracks;
    }

    std::vector<PlaylistTrack> filtered;
    filtered.reserve(tracks.size());
    for (const auto& track : tracks) {
        std::string key;
        if (!getLocalDateKey(track.dateAdded_, key)) {
            if (playlistId == "unknown") {
                filtered.push_back(track);
            }
            continue;
        }
        if (key == playlistId) {
            filtered.push_back(track);
        }
    }
    return filtered;
}

Playlist Client::getRecentlyPlayedPlaylist() {
    Playlist playlist;
    playlist.id = "airbuds-recent";
    playlist.name = "Airbuds History";

    const std::vector<PlaylistTrack> tracks = getRecentlyPlayedCachedOnly();
    playlist.totalItemCount = tracks.size();
    if (!tracks.empty()) {
        playlist.imageUrl = tracks.front().album.url;
    }
    return playlist;
}

std::vector<Playlist> Client::getPlaylists() {
    const std::vector<PlaylistTrack> tracks = getRecentlyPlayedTracks();
    return buildPlaylistsFromTracks(tracks);
}

std::vector<Playlist> Client::getPlaylistsCachedOnly() {
    const std::vector<PlaylistTrack> tracks = getRecentlyPlayedCachedOnly();
    return buildPlaylistsFromTracks(tracks);
}

std::vector<Playlist> Client::getPlaylistsForUser(const std::string& userId) {
    if (userId.empty()) {
        return getPlaylists();
    }
    const std::vector<PlaylistTrack> tracks = getRecentlyPlayedForUser(userId);
    return buildPlaylistsFromTracks(tracks);
}

std::vector<Playlist> Client::getPlaylistsCachedOnlyForUser(const std::string& userId) {
    if (userId.empty()) {
        return getPlaylistsCachedOnly();
    }
    const std::vector<PlaylistTrack> tracks = getRecentlyPlayedCachedOnlyForUser(userId);
    return buildPlaylistsFromTracks(tracks);
}

rapidjson::Document Client::apiGetRecentlyPlayed(
    const AirbudsCredentials& credentials,
    const std::string& userId,
    const std::optional<std::string>& cursor,
    const size_t limit) {
    rapidjson::Document requestJson;
    requestJson.SetObject();
    rapidjson::Document::AllocatorType& allocator = requestJson.GetAllocator();

    requestJson.AddMember("operationName", "UserRecentlyPlayed", allocator);

    rapidjson::Value variables(rapidjson::kObjectType);
    variables.AddMember("id", rapidjson::Value(userId.c_str(), allocator), allocator);
    variables.AddMember("limit", static_cast<int>(limit), allocator);
    if (cursor && !cursor->empty()) {
        variables.AddMember("cursor", rapidjson::Value(cursor->c_str(), allocator), allocator);
    }
    requestJson.AddMember("variables", variables, allocator);

    requestJson.AddMember("query", rapidjson::Value(AIRBUDS_RECENTLY_PLAYED_QUERY.data(), allocator), allocator);

    rapidjson::Value extensions(rapidjson::kObjectType);
    rapidjson::Value clientLibrary(rapidjson::kObjectType);
    clientLibrary.AddMember("name", "apollo-kotlin", allocator);
    clientLibrary.AddMember("version", "4.3.3", allocator);
    extensions.AddMember("clientLibrary", clientLibrary, allocator);
    requestJson.AddMember("extensions", extensions, allocator);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    requestJson.Accept(writer);

    const std::string body = buffer.GetString();

    const WebUtils::URLOptions::HeaderMap headers = {
        {"Accept", std::string(AIRBUDS_ACCEPT_HEADER)},
        {"Authorization", std::format("Bearer {}", credentials.accessToken)},
        {"Content-Type", "application/json"},
    };

    const AccumulatingStringResponse response =
        postJsonWithWebUtils(AIRBUDS_GRAPHQL_ENDPOINT, headers, body, AIRBUDS_USER_AGENT);
    const rapidjson::Document document = parseJsonPayload(response, "airbuds-graphql");
    if (document.HasMember("errors")) {
        throw std::runtime_error(std::format("Airbuds API error: {}", toString(document["errors"])));
    }

    rapidjson::Document result;
    result.CopyFrom(document, result.GetAllocator());
    return result;
}

rapidjson::Document Client::apiGetFriends(const AirbudsCredentials& credentials) {
    rapidjson::Document requestJson;
    requestJson.SetObject();
    rapidjson::Document::AllocatorType& allocator = requestJson.GetAllocator();

    requestJson.AddMember("operationName", "FriendList", allocator);

    rapidjson::Value variables(rapidjson::kObjectType);
    requestJson.AddMember("variables", variables, allocator);

    requestJson.AddMember("query", rapidjson::Value(AIRBUDS_FRIEND_LIST_QUERY.data(), allocator), allocator);

    rapidjson::Value extensions(rapidjson::kObjectType);
    rapidjson::Value clientLibrary(rapidjson::kObjectType);
    clientLibrary.AddMember("name", "apollo-kotlin", allocator);
    clientLibrary.AddMember("version", "4.3.3", allocator);
    extensions.AddMember("clientLibrary", clientLibrary, allocator);
    requestJson.AddMember("extensions", extensions, allocator);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    requestJson.Accept(writer);

    const std::string body = buffer.GetString();

    const WebUtils::URLOptions::HeaderMap headers = {
        {"Accept", std::string(AIRBUDS_ACCEPT_HEADER)},
        {"Authorization", std::format("Bearer {}", credentials.accessToken)},
        {"Content-Type", "application/json"},
    };

    const AccumulatingStringResponse response =
        postJsonWithWebUtils(AIRBUDS_GRAPHQL_ENDPOINT, headers, body, AIRBUDS_USER_AGENT);
    const rapidjson::Document document = parseJsonPayload(response, "airbuds-graphql-friends");
    if (document.HasMember("errors")) {
        throw std::runtime_error(std::format("Airbuds API error: {}", toString(document["errors"])));
    }

    rapidjson::Document result;
    result.CopyFrom(document, result.GetAllocator());
    return result;
}

std::vector<PlaylistTrack> Client::getRecentlyPlayedTracks() {
    return getRecentlyPlayedTracksForUser("", &cachedRecentlyPlayedTracks_, getRecentlyPlayedCachePath());
}

std::vector<PlaylistTrack> Client::getRecentlyPlayedTracksForUser(
    const std::string& userId,
    std::vector<PlaylistTrack>* cachedTracks,
    const std::filesystem::path& cachePath) {
    lastRecentlyPlayedWarning_.clear();
    RecentlyPlayedCache cache = loadRecentlyPlayedCache(cachePath);
    std::unordered_set<std::string> cachedKeys;
    if (!cache.tracks.empty()) {
        cachedKeys.reserve(cache.tracks.size() * 2);
        for (const auto& track : cache.tracks) {
            cachedKeys.insert(makeRecentlyPlayedKey(track));
        }
    }

    const auto credentials = getAirbudsCredentials();
    if (!credentials) {
        if (!cache.tracks.empty()) {
            lastRecentlyPlayedWarning_ = "Airbuds refresh token missing; showing cached history.";
            if (cachedTracks) {
                *cachedTracks = cache.tracks;
            }
            return cache.tracks;
        }
        throw std::runtime_error("Airbuds refresh token is missing.");
    }

    std::string targetUserId = userId;
    if (targetUserId.empty()) {
        targetUserId = credentials->userId;
    }
    if (targetUserId.empty()) {
        throw std::runtime_error("Airbuds API userId is missing.");
    }

    std::vector<PlaylistTrack> newTracks;
    std::unordered_set<std::string> newKeys;
    newKeys.reserve(cache.tracks.size() + 64);
    std::optional<std::string> cursor;
    bool hasNextPage = true;
    std::string lastCursor;
    bool reachedCachedBoundary = false;

    try {
        while (hasNextPage) {
            const rapidjson::Document document = apiGetRecentlyPlayed(*credentials, targetUserId, cursor, AIRBUDS_PAGE_LIMIT);
            if (!document.HasMember("data") || !document["data"].IsObject()) {
                throw std::runtime_error("Airbuds API response missing data.");
            }
            const auto& userJson = document["data"]["userWithID"];
            if (!userJson.IsObject() || !userJson.HasMember("recentlyPlayed")) {
                throw std::runtime_error("Airbuds API response missing recentlyPlayed.");
            }
            const auto& recentlyPlayed = userJson["recentlyPlayed"];
            if (!recentlyPlayed.IsObject() || !recentlyPlayed.HasMember("items") || !recentlyPlayed["items"].IsArray()) {
                throw std::runtime_error("Airbuds API response missing items.");
            }

            const auto& items = recentlyPlayed["items"].GetArray();
            for (const auto& item : items) {
                if (!item.IsObject() || !item.HasMember("object")) {
                    continue;
                }
                const auto& object = item["object"];
                if (!object.IsObject() || !object.HasMember("openable")) {
                    continue;
                }
                const auto& openable = object["openable"];
                if (!openable.IsObject()) {
                    continue;
                }

                PlaylistTrack track;
                track.id = getOptionalString(openable, "id");
                track.name = getOptionalString(openable, "name");
                track.album.url = getOptionalString(openable, "artworkURL");

                const std::string artistName = getOptionalString(openable, "artistName");
                track.artists = parseArtistsFromName(artistName);

                track.dateAdded = getOptionalString(item, "playedAtMax");
                if (!track.dateAdded.empty()) {
                    track.dateAdded_ = parseIso8601ToMillis(track.dateAdded);
                } else {
                    track.dateAdded_ = std::chrono::milliseconds(0);
                }

                if (track.id.empty() || track.name.empty()) {
                    continue;
                }

                if (cache.oldestTimestamp.count() > 0
                    && track.dateAdded_.count() > 0
                    && track.dateAdded_ <= cache.oldestTimestamp) {
                    reachedCachedBoundary = true;
                    break;
                }

                const std::string key = makeRecentlyPlayedKey(track);
                if (!cachedKeys.empty() && cachedKeys.contains(key)) {
                    continue;
                }
                if (!newKeys.insert(key).second) {
                    continue;
                }
                newTracks.push_back(track);
            }

            if (reachedCachedBoundary) {
                break;
            }

            hasNextPage = false;
            if (recentlyPlayed.HasMember("pageInfo") && recentlyPlayed["pageInfo"].IsObject()) {
                const auto& pageInfo = recentlyPlayed["pageInfo"];
                if (pageInfo.HasMember("hasNextPage") && pageInfo["hasNextPage"].IsBool()) {
                    hasNextPage = pageInfo["hasNextPage"].GetBool();
                }
                const std::string nextCursor = getOptionalString(pageInfo, "endCursor");
                if (nextCursor.empty() || nextCursor == lastCursor) {
                    hasNextPage = false;
                } else {
                    cursor = nextCursor;
                    lastCursor = nextCursor;
                }
            }
        }
    } catch (const std::exception& exception) {
        if (!cache.tracks.empty()) {
            AirbudsSearch::Log.warn("Recently played refresh failed: {}", exception.what());
            lastRecentlyPlayedWarning_ = "Refresh failed; showing cached history.";
            if (cachedTracks) {
                *cachedTracks = cache.tracks;
            }
            return cache.tracks;
        }
        throw;
    }

    std::vector<PlaylistTrack> merged;
    merged.reserve(newTracks.size() + cache.tracks.size());
    std::unordered_set<std::string> mergedKeys;
    mergedKeys.reserve(newTracks.size() + cache.tracks.size());
    for (const auto& track : newTracks) {
        merged.push_back(track);
        mergedKeys.insert(makeRecentlyPlayedKey(track));
    }
    for (const auto& track : cache.tracks) {
        const std::string key = makeRecentlyPlayedKey(track);
        if (mergedKeys.insert(key).second) {
            merged.push_back(track);
        }
    }

    if (!merged.empty()) {
        std::stable_sort(merged.begin(), merged.end(), [](const auto& left, const auto& right) {
            const auto leftMillis = left.dateAdded_.count();
            const auto rightMillis = right.dateAdded_.count();
            if (leftMillis == 0 && rightMillis == 0) {
                return false;
            }
            if (leftMillis == 0) {
                return false;
            }
            if (rightMillis == 0) {
                return true;
            }
            return leftMillis > rightMillis;
        });
    }

    saveRecentlyPlayedCache(cachePath, merged);
    if (cachedTracks) {
        *cachedTracks = merged;
    }
    return merged;
}

std::string Client::getLastRecentlyPlayedWarning() const {
    return lastRecentlyPlayedWarning_;
}

} // namespace airbuds
