#include "UI/Modals/UploadDetails.hpp"

#include "assets.hpp"
#include "BeatSaverRegionManager.hpp"
#include "bsml/shared/BSML.hpp"
#include "bsml/shared/BSML/MainThreadScheduler.hpp"
#include "PluginConfig.hpp"
#include "songcore/shared/SongCore.hpp"
#include "UI/FlowCoordinators/BetterSongSearchFlowCoordinator.hpp"
#include "UnityEngine/GUIUtility.hpp"
#include "Util/CurrentTimeMs.hpp"

using namespace BetterSongSearch::UI;
using namespace BetterSongSearch::Util;
#define coro(coroutine) BSML::SharedCoroutineStarter::get_instance()->StartCoroutine(custom_types::Helpers::CoroutineHelper::New(coroutine))

const std::vector<std::string> CHAR_GROUPING = {
    "Custom", "Standard", "OneSaber", "NoArrows", "NinetyDegree", "ThreeSixtyDegree", "LightShow", "Lawless"
};

DEFINE_TYPE(BetterSongSearch::UI::Modals, UploadDetails);

// name and count
std::unordered_map<std::string, int> GroupCharByDiff(SongDetailsCache::Song const* song) {
    std::unordered_map<std::string, int> groupedChars;
    for (auto const& diff : *song) {
        auto key = CHAR_GROUPING[(int) diff.characteristic];
        if (!groupedChars.contains(key)) {
            groupedChars.emplace(key, 1);
        } else {
            groupedChars[key] += 1;
        }
    }
    return groupedChars;
}

void Modals::UploadDetails::OnEnable() {
}

void Modals::UploadDetails::CloseModal() {
    this->rootModal->Hide();
}

void Modals::UploadDetails::OpenBeatSaver() {
    try {
        static auto UnityEngine_Application_OpenURL = il2cpp_utils::resolve_icall<void, StringW>("UnityEngine.Application::OpenURL");
        UnityEngine_Application_OpenURL(StringW("https://beatsaver.com/maps/" + selectedSongKey->get_text()));
    } catch (...) {
        ERROR("Failed to OpenBeatSaver");
    }
}

void Modals::UploadDetails::OpenMapPreview() {
    try {
        static auto UnityEngine_Application_OpenURL = il2cpp_utils::resolve_icall<void, StringW>("UnityEngine.Application::OpenURL");
        UnityEngine_Application_OpenURL(StringW("https://allpoland.github.io/ArcViewer/?id=" + selectedSongKey->get_text()));
    } catch (...) {
        ERROR("Failed to open map preview");
    }
}

void Modals::UploadDetails::CopyBSR() {
    try {
        auto bsr = selectedSongKey->get_text();
        static auto UnityEngine_GUIUtility_set_systemCopyBuffer =
            il2cpp_utils::resolve_icall<void, StringW>("UnityEngine.GUIUtility::set_systemCopyBuffer");
        UnityEngine_GUIUtility_set_systemCopyBuffer(bsr);
    } catch (...) {
        ERROR("Failed to copy BSR");
    }
}

void Modals::UploadDetails::ctor() {
    INVOKE_CTOR();
    this->initialized = false;
}

void Modals::UploadDetails::OpenModal(SongDetailsCache::Song const* song) {
    if (!initialized) {
        BSML::parse_and_construct(Assets::UploadDetails_bsml, this->get_transform(), this);
        initialized = true;
    }

    // Fill data
    auto groupedDiffs = GroupCharByDiff(song);
    std::string characteristicsText = "";
    int loopCount = 1;
    for (auto& it : groupedDiffs) {
        if (loopCount < groupedDiffs.size()) {
            characteristicsText.append(fmt::format("{}x {}, ", it.second, it.first));
        } else {
            characteristicsText.append(fmt::format("{}x {}", it.second, it.first));
        }
        loopCount++;
    }
    selectedCharacteristics->set_text(characteristicsText);
    selectedSongKey->set_text(song->key());
    selectedRating->set_text(fmt::format("{:.1f}%", song->rating() * 100));
    selectedSongDescription->SetText("Loading...");
    selectedSongDescription->ScrollTo(0, false);

    songDetailsLoading->get_gameObject()->SetActive(true);

    BeatSaverRegionManager::GetSongDescription(std::string(song->key()), [this](std::string value) {
        BSML::MainThreadScheduler::Schedule([this, value] {
            selectedSongDescription->SetText(value);
            songDetailsLoading->get_gameObject()->SetActive(false);
        });
    });

    this->rootModal->Show();
}
