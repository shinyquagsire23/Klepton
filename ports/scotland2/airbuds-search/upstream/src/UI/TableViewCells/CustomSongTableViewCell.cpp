#include "UI/TableViewCells/CustomSongTableViewCell.hpp"

#include "UnityEngine/GameObject.hpp"
#include "bsml/shared/BSML-Lite/Creation/Text.hpp"
#include "songcore/shared/SongCore.hpp"

#include "Log.hpp"
#include "Utils.hpp"
#include "bsml/shared/BSML/Components/Backgroundable.hpp"

using namespace AirbudsSearch::UI;

DEFINE_TYPE(AirbudsSearch::UI, CustomSongTableViewCell);

void CustomSongTableViewCell::ctor() {
    INVOKE_BASE_CTOR(classof(HMUI::TableCell*));
}

void CustomSongTableViewCell::OnDestroy() {
}

void CustomSongTableViewCell::SelectionDidChange(HMUI::SelectableCell::TransitionType transitionType) {
    updateBackground();
}

void CustomSongTableViewCell::HighlightDidChange(HMUI::SelectableCell::TransitionType transitionType) {
    updateBackground();
}

void CustomSongTableViewCell::WasPreparedForReuse() {
}

void CustomSongTableViewCell::updateBackground() {
    root_->set_color(UnityEngine::Color(0, 0, 0, selected || highlighted ? 0.8f : 0.45f));
}

std::string getDifficultyName(const SongDetailsCache::MapDifficulty mapDifficulty) {
    std::unordered_map<SongDetailsCache::MapDifficulty, std::string> difficultyNameMap = {
        {SongDetailsCache::MapDifficulty::Easy, "EASY"},
        {SongDetailsCache::MapDifficulty::Normal, "NORMAL"},
        {SongDetailsCache::MapDifficulty::Hard, "HARD"},
        {SongDetailsCache::MapDifficulty::Expert, "EXPERT"},
        {SongDetailsCache::MapDifficulty::ExpertPlus, "EXPERT+"},
    };
    auto iterator = difficultyNameMap.find(mapDifficulty);
    if (iterator != difficultyNameMap.end()) {
        return iterator->second;
    }
    return "Unknown";
}

void CustomSongTableViewCell::setSong(const SongDetailsCache::Song* const song) {
    song_ = song;

    // Song name
    songNameTextView_->set_text(song->songName());

    // Check if we already have this song
    if (SongCore::API::Loading::GetLevelByHash(song->hash())) {
        songNameTextView_->set_color(UnityEngine::Color(0.3f, 0.6f, 0.3f, 1.0f));
    } else {
        songNameTextView_->set_color(UnityEngine::Color::get_white());
    }

    // Map author
    uploaderNameTextView_->set_text(song->uploaderName());

    // Upload date
    const std::string uploadDateString = std::format("{:%m/%d/%y}", song->uploadTime());
    uploadTimeTextView_->set_text(uploadDateString);

    // Upvotes / Downvotes
    const std::string upvoteRatioText = std::format("<color=green>{}</color> / <color=red>{}</color>", song->upvotes, song->downvotes);
    upvoteRatioTextView_->set_text(upvoteRatioText);

    // Loading sprite
    const UnityW<UnityEngine::Sprite> placeholderSprite = Utils::getAlbumPlaceholderSprite();
    image_->set_sprite(placeholderSprite);

    // Load cover image
    const std::string songHash = song_->hash();
    Utils::getCoverImageSprite(songHash, [this, songHash](const UnityW<UnityEngine::Sprite> sprite) {
        // Check if the selected song has changed
        if (!song_ || song_->hash() != songHash) {
            AirbudsSearch::Log.warn("Cancelled sprite update");
            return;
        }

        // Update UI
        if (sprite) {
            image_->set_sprite(sprite);
        } else {
            AirbudsSearch::Log.warn("Failed loading cover image for song with hash: {}", songHash);
        }
    });

    // Difficulties
    std::vector<const SongDetailsCache::SongDifficulty*> difficulties;
    for (const SongDetailsCache::SongDifficulty& songDifficulty : *song_) {
        difficulties.push_back(&songDifficulty);
    }

    // Sort and remove duplicates
    std::stable_sort(difficulties.begin(), difficulties.end(), [](const SongDetailsCache::SongDifficulty* a, const SongDetailsCache::SongDifficulty* b) {
        return a->difficulty < b->difficulty;
    });
    difficulties.erase(std::unique(difficulties.begin(), difficulties.end(), [](const SongDetailsCache::SongDifficulty* a, const SongDetailsCache::SongDifficulty* b) {
                           return a->difficulty == b->difficulty;
                       }),
                       difficulties.end());

    // Delete existing texts
    const ArrayW<HMUI::CurvedTextMeshPro*> textChildren = diffsContainer_->GetComponentsInChildren<HMUI::CurvedTextMeshPro*>();
    for (const auto& child : textChildren) {
        DestroyImmediate(child->get_gameObject());
    }

    // Add texts
    for (const SongDetailsCache::SongDifficulty* const difficulty : difficulties) {
        HMUI::CurvedTextMeshPro* const curvedTextMeshPro = BSML::Lite::CreateText(diffsContainer_->get_transform(), getDifficultyName(difficulty->difficulty), 2);
        curvedTextMeshPro->set_alignment(TMPro::TextAlignmentOptions::Center);
        curvedTextMeshPro->set_raycastTarget(false);
    }
}
