#include "Airbuds/Json.hpp"

#include "Airbuds/Utils.hpp"
#include "Log.hpp"

namespace {

std::chrono::milliseconds iso8601_to_epoch(const std::string& iso) {
    std::tm t{};
    // Parse manually: YYYY-MM-DDTHH:MM:SSZ
    if (sscanf(iso.c_str(), "%4d-%2d-%2dT%2d:%2d:%2dZ",
               &t.tm_year, &t.tm_mon, &t.tm_mday,
               &t.tm_hour, &t.tm_min, &t.tm_sec)
        != 6) {
        throw std::runtime_error("Invalid ISO 8601 format");
    }

    t.tm_year -= 1900; // tm_year = years since 1900
    t.tm_mon -= 1;     // tm_mon = months since January [0-11]
    t.tm_isdst = 0;    // no daylight saving

    // Convert to time_t (UTC)
    std::time_t time = timegm(&t); // POSIX

    return std::chrono::milliseconds(time);
}

}

namespace airbuds::json {

Track getTrackFromJson(const rapidjson::Value& json) {
    Track track;

    // ID
    track.id = getString(json, "id");

    // Name
    track.name = getString(json, "name");

    // Artists
    const auto& artists = json["artists"].GetArray();
    for (rapidjson::SizeType j = 0; j < artists.Size(); ++j) {
        airbuds::Artist artist;
        artist.id = artists[j]["id"].GetString();
        artist.name = artists[j]["name"].GetString();
        track.artists.push_back(artist);
    }

    // Album
    const auto& albumJson = json["album"].GetObject();
    const auto& imagesJson = albumJson["images"].GetArray();

    const std::vector<Image> images = getImagesFromJson(imagesJson);

    // Find smallest image
    if (!images.empty()) {
        const Image smallestImage = getSmallestImage(images);
        track.album.url = smallestImage.url;
    }

    return track;
}

PlaylistTrack getPlaylistTrackFromJson(const rapidjson::Value& json) {
    PlaylistTrack playlistTrack;

    // Track
    const Track track = getTrackFromJson(json["track"]);
    playlistTrack.id = track.id;
    playlistTrack.name = track.name;
    playlistTrack.artists = track.artists;
    playlistTrack.album = track.album;

    // Date added
    playlistTrack.dateAdded = getString(json, "added_at");
    playlistTrack.dateAdded_ = iso8601_to_epoch(playlistTrack.dateAdded);

    return playlistTrack;
}

User getUserFromJson(const rapidjson::Value& json) {
    User user;

    // ID
    user.id = getString(json, "id");

    // Display name
    user.displayName = getString(json, "display_name");

    // Profile image URL
    const auto& images = json["images"].GetArray();
    for (rapidjson::SizeType i = 0; i < images.Size(); ++i) {
        user.imageUrl = getString(images[0], "url");
    }

    return user;
}

std::vector<Image> getImagesFromJson(const rapidjson::Value& json) {
    std::vector<Image> images;
    for (rapidjson::SizeType i = 0; i < json.Size(); ++i) {
        const auto& imageJson = json[i];

        Image image{"", -1, -1};

        // URL
        image.url = imageJson["url"].GetString();

        // Width
        static constexpr const char* const JSON_KEY_WIDTH = "width";
        if (imageJson.HasMember(JSON_KEY_WIDTH)) {
            const auto& jsonValueWidth = imageJson[JSON_KEY_WIDTH];
            if (jsonValueWidth.IsNumber()) {
                image.width = jsonValueWidth.GetInt();
            }
        }

        // Height
        static constexpr const char* const JSON_KEY_HEIGHT = "height";
        if (imageJson.HasMember(JSON_KEY_HEIGHT)) {
            const auto& jsonValueHeight = imageJson[JSON_KEY_HEIGHT];
            if (jsonValueHeight.IsInt()) {
                image.height = jsonValueHeight.GetInt();
            }
        }

        images.push_back(image);
    }
    return images;
}

Playlist getPlaylistFromJson(const rapidjson::Value& json) {
    Playlist playlist;

    playlist.id = json["id"].GetString();
    playlist.name = json["name"].GetString();

    playlist.tracksUrl = json["tracks"]["href"].GetString();
    playlist.totalItemCount = json["tracks"]["total"].GetUint64();

    if (json.HasMember("images")) {
        if (json["images"].IsArray()) {
            const auto& imagesJson = json["images"].GetArray();
            const std::vector<Image> images = getImagesFromJson(imagesJson);

            // Find smallest image
            if (!images.empty()) {
                const Image smallestImage = getSmallestImage(images);
                playlist.imageUrl = smallestImage.url;
            }
        } else {
            AirbudsSearch::Log.warn("Playlist has images but wrong type: {}", static_cast<int>(json["images"].GetType()));
        }
    } else {
        AirbudsSearch::Log.warn("Playlist is missing images!");
    }

    return playlist;
}

std::string toString(const rapidjson::Value& json) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    json.Accept(writer);
    return buffer.GetString();
}

std::string getString(const rapidjson::Value& json, const std::string& key) {
    if (!json.HasMember(key)) {
        throw std::runtime_error(std::format("Missing key: {}", key));
    }
    const auto& jsonValue = json[key];
    if (!jsonValue.IsString()) {
        throw std::runtime_error(std::format("Unexpected type for key: {}", key));
    }
    const std::string value = jsonValue.GetString();
    if (value.empty()) {
        throw std::runtime_error(std::format("Value for key was empty: {}", key));
    }
    return value;
}

} // namespace airbuds::json
