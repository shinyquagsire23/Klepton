#include "UnityEngine/GameObject.hpp"

#include "Log.hpp"
#include "UI/TableViewCells/AirbudsTrackTableViewCell.hpp"
#include "Utils.hpp"

DEFINE_TYPE(AirbudsSearch::UI, AirbudsTrackTableViewCell);

using namespace AirbudsSearch::UI;

void AirbudsTrackTableViewCell::ctor() {
    INVOKE_BASE_CTOR(classof(HMUI::TableCell*));
}

void AirbudsTrackTableViewCell::OnDestroy() {
}

void AirbudsTrackTableViewCell::SelectionDidChange(HMUI::SelectableCell::TransitionType transitionType) {
    updateBackground();
}

void AirbudsTrackTableViewCell::HighlightDidChange(HMUI::SelectableCell::TransitionType transitionType) {
    updateBackground();
}

void AirbudsTrackTableViewCell::WasPreparedForReuse() {
}

void AirbudsTrackTableViewCell::updateBackground() {
    root_->set_color(UnityEngine::Color(0, 0, 0, selected || highlighted ? 0.8f : 0.45f));
}

void AirbudsTrackTableViewCell::setTrack(const airbuds::PlaylistTrack& track) {
    track_ = track;

    // Name
    trackNameTextView_->set_text(track.name);

    // Artists
    std::stringstream stringStream;
    for (auto it = track.artists.begin(); it != track.artists.end(); ++it) {
        stringStream << it->name;
        if (it != track.artists.end() - 1) {
            stringStream << ", ";
        }
    }
    trackArtistsTextView_->set_text(stringStream.str());

    // Loading sprite
    const UnityW<UnityEngine::Sprite> placeholderSprite = Utils::getAlbumPlaceholderSprite();
    image_->set_sprite(placeholderSprite);

    // Load cover image
    if (!track.album.url.empty()) {
        const std::string trackId = track.id;
        Utils::getImageAsSprite(track.album.url, [this, trackId](const UnityW<UnityEngine::Sprite> sprite) {
            // Check if the selected song has changed
            if (track_.id != trackId) {
                AirbudsSearch::Log.warn("Cancelled sprite update");
                return;
            }

            // Update UI
            if (sprite) {
                image_->set_sprite(sprite);
            } else {
                AirbudsSearch::Log.warn("Failed loading cover image for song with hash: {}", trackId);
            }
        });
    }

}
