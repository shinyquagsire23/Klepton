#include "UI/RefreshSongButton.hpp"
#include "bsml/shared/BSML.hpp"

DEFINE_TYPE(SongCore::UI, RefreshSongButton);

namespace SongCore::UI {
    void RefreshSongButton::ctor(SongLoader::RuntimeSongLoader* runtimeSongLoader) {
        _runtimeSongLoader = runtimeSongLoader;
        _menuButton = BSML::MenuButton::Make_new("Refresh Songs", "Refresh your loaded songs", [this](){
            if (!_runtimeSongLoader->AreSongsRefreshing) {
                _runtimeSongLoader->RefreshSongs(false);
            }
        });
    }

    void RefreshSongButton::Initialize() {
        BSML::Register::RegisterMenuButton(_menuButton);
    }

    void RefreshSongButton::Dispose() {
        BSML::Register::UnRegisterMenuButton(_menuButton);
    }
}
