#include "UI/Modals/Settings.hpp"

#include "assets.hpp"
#include "bsml/shared/BSML.hpp"
#include "DataHolder.hpp"
#include "PluginConfig.hpp"
#include "songcore/shared/SongCore.hpp"
#include "UI/FlowCoordinators/BetterSongSearchFlowCoordinator.hpp"
#include "Util/CurrentTimeMs.hpp"

using namespace BetterSongSearch::UI;
using namespace BetterSongSearch::Util;
#define coro(coroutine) BSML::SharedCoroutineStarter::get_instance()->StartCoroutine(custom_types::Helpers::CoroutineHelper::New(coroutine))

DEFINE_TYPE(BetterSongSearch::UI::Modals, Settings);

void Modals::Settings::OnEnable() {
}

void Modals::Settings::CloseModal() {
    this->settingsModal->Hide();
}

void Modals::Settings::ctor() {
    INVOKE_CTOR();
    this->initialized = false;
}

void Modals::Settings::OpenModal() {
    if (!initialized) {
        BSML::parse_and_construct(Assets::Settings_bsml, this->get_transform(), this);
        initialized = true;
    }
    this->settingsModal->Show();
}

bool Modals::Settings::get_returnToBssFromSolo() {
    return getPluginConfig().ReturnToBSS.GetValue();
}

void Modals::Settings::set_returnToBssFromSolo(bool value) {
    getPluginConfig().ReturnToBSS.SetValue(value);
}

bool Modals::Settings::get_loadSongPreviews() {
    return getPluginConfig().LoadSongPreviews.GetValue();
}

void Modals::Settings::set_loadSongPreviews(bool value) {
    getPluginConfig().LoadSongPreviews.SetValue(value);
}

bool Modals::Settings::get_smallerFontSize() {
    return getPluginConfig().SmallerFontSize.GetValue();
}

void Modals::Settings::set_smallerFontSize(bool value) {
    getPluginConfig().SmallerFontSize.SetValue(value);
    fcInstance->SongListController->songListTable()->ReloadData();
}

StringW Modals::Settings::get_preferredLeaderboard() {
    // Preferred Leaderboard
    std::string preferredLeaderboard = getPluginConfig().PreferredLeaderboard.GetValue();
    if (LEADERBOARD_MAP.contains(preferredLeaderboard)) {
        return preferredLeaderboard;
    } else {
        dataHolder.preferredLeaderboard = FilterTypes::PreferredLeaderBoard::ScoreSaber;
        getPluginConfig().PreferredLeaderboard.SetValue("Scoresaber");
        return "Scoresaber";
    }
}

void Modals::Settings::set_preferredLeaderboard(StringW value) {
    if (LEADERBOARD_MAP.contains(value)) {
        dataHolder.preferredLeaderboard = LEADERBOARD_MAP.at(value);
        getPluginConfig().PreferredLeaderboard.SetValue(value);
        auto controller = fcInstance->SongListController;
        dataHolder.forceReload = true;
        controller->SortAndFilterSongs(dataHolder.sort, dataHolder.search, true);
    }
}
