#pragma once

#include "HMUI/TableView.hpp"
#include "UnityEngine/AudioClip.hpp"
#include "UnityEngine/Sprite.hpp"
#include "custom-types/shared/coroutine.hpp"
#include "song-details/shared/SongDetails.hpp"
#include "songcore/shared/SongLoader/CustomBeatmapLevel.hpp"

namespace AirbudsSearch::Utils {

std::string toLowerCase(const std::string& text);

void getCoverImageSprite(const std::string& songHash, std::function<void(const UnityW<UnityEngine::Sprite> sprite)> onLoadComplete);

std::string getCoverImageFilePath(const SongCore::SongLoader::CustomBeatmapLevel& beatmap);

UnityW<UnityEngine::Sprite> createSimpleSprite();

std::string toLowerCase(std::string_view text);

void getImageAsSprite(std::string url, std::function<void(const UnityW<UnityEngine::Sprite> sprite)> onLoadComplete);

custom_types::Helpers::Coroutine getAudioClipFromUrl(const std::string_view url, std::function<void(UnityW<UnityEngine::AudioClip> audioClip)> onLoadComplete);

void getAudioClipForSongHash(const std::string_view songHash, std::function<void(UnityW<UnityEngine::AudioClip> audioClip)> onLoadComplete);

UnityW<UnityEngine::Sprite> getPlaylistPlaceholderSprite();

UnityW<UnityEngine::Sprite> getAlbumPlaceholderSprite();

void goToLevelSelect(const std::string& songHash);

std::string encodeBase64(const std::string& input);

std::span<const uint8_t> toSpan(const std::string& text);

void reloadDataKeepingPosition(UnityW<HMUI::TableView> tableView);

std::string jsonDocumentToString(const rapidjson::Value& value);

SongDetailsCache::MapDifficulty getMapDifficultyFromString(const std::string& text);

void removeRaycastFromButtonIcon(UnityW<UnityEngine::UI::Button> button);

template<typename T>
T* findParentWithComponent(UnityEngine::Transform* start) {
    auto current = start;
    while (current) {
        auto comp = current->GetComponent<T*>();
        if (comp) return comp;
        current = current->get_parent();
    }
    return nullptr;
}

void setIconScale(UnityW<UnityEngine::UI::Button> button, float scale);

}// namespace AirbudsSearch::Utils

namespace AirbudsSearch::Debug {

void dumpViewHierarchy(UnityW<UnityEngine::Transform> transform, int depth = 0);

void dumpAllSprites();

}// namespace AirbudsSearch::Debug
