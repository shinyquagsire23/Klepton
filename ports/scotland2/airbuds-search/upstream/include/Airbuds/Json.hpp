#pragma once

#include <web-utils/shared/WebUtils.hpp> // For rapidjson

#include "Track.hpp"
#include "Playlist.hpp"
#include "User.hpp"
#include "Image.hpp"
#include "Log.hpp"

namespace airbuds::json {

Track getTrackFromJson(const rapidjson::Value& json);

PlaylistTrack getPlaylistTrackFromJson(const rapidjson::Value& json);

Playlist getPlaylistFromJson(const rapidjson::Value& json);

std::vector<Image> getImagesFromJson(const rapidjson::Value& json);

User getUserFromJson(const rapidjson::Value& json);

std::string toString(const rapidjson::Value& json);

std::string getString(const rapidjson::Value& json, const std::string& key);

template <typename T>
std::vector<T> getArray(const rapidjson::Value& json, const std::string& key, const std::function<T(const rapidjson::Value& item)>& parser) {
    // Check if the key exists
    if (!json.HasMember(key)) {
        throw std::runtime_error(std::format("Missing key: {}", key));
    }

    // Check if the key is the expected type
    const auto& jsonValue = json[key];
    if (!jsonValue.IsArray()) {
        throw std::runtime_error(std::format("Unexpected type ({}) for key: {}", static_cast<int>(jsonValue.GetType()), key));
    }

    // Parse the array
    std::vector<T> parsedItems;
    const auto items = jsonValue.GetArray();
    for (rapidjson::SizeType i = 0; i < items.Size(); ++i) {
        try {
            parsedItems.emplace_back(parser(items[i]));
        } catch (const std::exception& exception) {
            AirbudsSearch::Log.warn("Failed to parse array item: {}", exception.what());
        }
    }

    return parsedItems;
}

} // namespace airbuds::json
