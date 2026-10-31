#include "PlayButtonInteractable.hpp"
#include "SongCore.hpp"

DEFINE_TYPE(SongCore, PlayButtonInteractable);

namespace SongCore {
    void PlayButtonInteractable::ctor() {
        INVOKE_CTOR();
    }

    void PlayButtonInteractable::Initialize() {
        SongCore::API::PlayButton::GetPlayButtonDisablingModsChangedEvent() += {&PlayButtonInteractable::InvokePlayButtonDisablingModsChanged, this};
    }

    void PlayButtonInteractable::Dispose() {
        SongCore::API::PlayButton::GetPlayButtonDisablingModsChangedEvent() -= {&PlayButtonInteractable::InvokePlayButtonDisablingModsChanged, this};
    }

    void PlayButtonInteractable::DisablePlayButton(std::string modID, std::string reason) {
        return SongCore::API::PlayButton::DisablePlayButton(modID, reason);
    }

    void PlayButtonInteractable::EnablePlayButton(std::string modID) {
        return SongCore::API::PlayButton::EnablePlayButton(modID);
    }

    std::span<PlayButtonInteractable::PlayButtonDisablingModInfo const> PlayButtonInteractable::GetPlayButtonDisablingModInfos() {
        return SongCore::API::PlayButton::GetPlayButtonDisablingModInfos();
    }

    void PlayButtonInteractable::InvokePlayButtonDisablingModsChanged(std::span<PlayButtonDisablingModInfo const> disablingModInfos) {
        PlayButtonDisablingModsChanged.invoke(disablingModInfos);
    }
}
