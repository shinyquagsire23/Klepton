#include "main.hpp"

#include "_config.h"
#include "bsml/shared/BSML/SharedCoroutineStarter.hpp"
#include "bsml/shared/Helpers/delegates.hpp"
#include "bsml/shared/Helpers/getters.hpp"
#include "custom-types/shared/register.hpp"
#include "DataHolder.hpp"
#include "GlobalNamespace/BeatmapKey.hpp"
#include "GlobalNamespace/BeatmapLevelPack.hpp"
#include "GlobalNamespace/GameplaySetupViewController.hpp"
#include "GlobalNamespace/LevelFilteringNavigationController.hpp"
#include "GlobalNamespace/MainFlowCoordinator.hpp"
#include "GlobalNamespace/MenuTransitionsHelper.hpp"
#include "GlobalNamespace/MultiplayerLevelScenesTransitionSetupDataSO.hpp"
#include "GlobalNamespace/MultiplayerResultsViewController.hpp"
#include "GlobalNamespace/PlayerData.hpp"
#include "GlobalNamespace/SelectLevelCategoryViewController.hpp"
#include "GlobalNamespace/SettingsManager.hpp"
#include "GlobalNamespace/SongPackMask.hpp"
#include "HMUI/TextSegmentedControlCell.hpp"
#include "logging.hpp"
#include "PluginConfig.hpp"
#include "System/Action.hpp"
#include "UI/FlowCoordinators/BetterSongSearchFlowCoordinator.hpp"
#include "UI/Manager.hpp"
#include "UI/ViewControllers/SongList.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Resources.hpp"
#include "Util/TextUtil.hpp"

#define coro(coroutine) BSML::SharedCoroutineStarter::get_instance()->StartCoroutine(custom_types::Helpers::CoroutineHelper::New(coroutine))

using namespace BetterSongSearch::Util;
using namespace BetterSongSearch;
using namespace BetterSongSearch::UI;

// Called at the early stages of game loading
BSS_EXPORT_FUNC void setup(CModInfo& info) {
    info.id = MOD_ID;
    info.version = VERSION;
    info.version_long = GIT_COMMIT;
    modInfo.assign(info);

    getPluginConfig().Init(modInfo);

    INFO("Completed setup!");

    std::thread([] {
        // Init the data holder (sub to events)
        dataHolder.Init();
    }).detach();
}

SC2_RECORD_SEARCH_HOOK(ReturnToBSS, &HMUI::FlowCoordinator::DismissFlowCoordinator)
MAKE_HOOK_MATCH(
    ReturnToBSS,
    &HMUI::FlowCoordinator::DismissFlowCoordinator,
    void,
    HMUI::FlowCoordinator* self,
    HMUI::FlowCoordinator* flowCoordinator,
    HMUI::ViewController::AnimationDirection animationDirection,
    System::Action* finishedCallback,
    bool immediately
) {
    if (!getPluginConfig().ReturnToBSS.GetValue()) {
        ReturnToBSS(self, flowCoordinator, animationDirection, finishedCallback, immediately);
        return;
    }
    if (!(flowCoordinator->get_gameObject()->get_name() == "SoloFreePlayFlowCoordinator")) {
        ReturnToBSS(self, flowCoordinator, animationDirection, finishedCallback, immediately);
        return;
    }

    ReturnToBSS(self, flowCoordinator, animationDirection, finishedCallback, true);
    if (fromBSS) {
        auto currentFlowCoordinator = BSML::Helpers::GetMainFlowCoordinator()->YoungestChildFlowCoordinatorOrSelf();
        UnityW<BetterSongSearch::UI::FlowCoordinators::BetterSongSearchFlowCoordinator> betterSongSearchFlowCoordinator =
            UnityEngine::Resources::FindObjectsOfTypeAll<BetterSongSearch::UI::FlowCoordinators::BetterSongSearchFlowCoordinator*>()->FirstOrDefault(
            );
        if (betterSongSearchFlowCoordinator) {
            currentFlowCoordinator->PresentFlowCoordinator(
                betterSongSearchFlowCoordinator, nullptr, HMUI::ViewController::AnimationDirection::Horizontal, true, false
            );
        }
    }
};

// Soft restart in settings
SC2_RECORD_SEARCH_HOOK(MenuTransitionsHelper_RestartGame, &GlobalNamespace::MenuTransitionsHelper::RestartGame)
MAKE_HOOK_MATCH(
    MenuTransitionsHelper_RestartGame,
    &GlobalNamespace::MenuTransitionsHelper::RestartGame,
    void,
    GlobalNamespace::MenuTransitionsHelper* self,
    System::Action_1<Zenject::DiContainer*>* finishCallback
) {
    DEBUG("Destroying manager flow before restart");
    manager.DestroyFlow();
    fromBSS = false;
    MenuTransitionsHelper_RestartGame(self, finishCallback);
}

SC2_RECORD_SEARCH_HOOK(GameplaySetupViewController_RefreshContent, &GlobalNamespace::GameplaySetupViewController::RefreshContent)
MAKE_HOOK_MATCH(
    GameplaySetupViewController_RefreshContent,
    &GlobalNamespace::GameplaySetupViewController::RefreshContent,
    void,
    GlobalNamespace::GameplaySetupViewController* self
) {
    GameplaySetupViewController_RefreshContent(self);

    bool multiplayer = self->____showMultiplayer;

    // Button instance
    static SafePtrUnity<UnityEngine::GameObject> button;

    // Don't do anything if not in multiplayer to avoid messing with unity objects
    if (!multiplayer) {
        if (button) {
            button->set_active(multiplayer);
        }
        return;
    }

    if (!button) {
        DEBUG("Button not found, creating");
        UnityW<UnityEngine::Transform> x = self->get_transform()->Find("BSMLBackground/BSMLTabSelector");
        if (!x) {
            x = self->get_transform()->Find("TextSegmentedControl");
        }
        if (!x) {
            return;
        }

        button = UnityEngine::GameObject::Instantiate(x->get_transform()->GetChild(x->get_transform()->GetChildCount() - 1), x)->get_gameObject();
        if (!button) {
            ERROR("Could not create button somehow");
            return;
        }

        UnityW<HMUI::TextSegmentedControlCell> t = button->GetComponent<HMUI::TextSegmentedControlCell*>();

        t->set_text("Better Song Search");

        t.cast<HMUI::SelectableCell>()->____wasPressedSignal = nullptr;

        std::function<void(UnityW<::HMUI::SelectableCell>, HMUI::SelectableCell::TransitionType, ::System::Object*)> fun =
            [](UnityW<::HMUI::SelectableCell> cell, HMUI::SelectableCell::TransitionType transition, ::System::Object* obj) {
                if (!cell) {
                    return;
                }
                if (!cell->get_selected()) {
                    return;
                }

                cell->set_selected(false);
                manager.ShowFlow(false);
            };
        auto action = BSML::MakeSystemAction(fun);
        t->add_selectionDidChangeEvent(action);
    }

    button->set_active(multiplayer);
}

SC2_RECORD_SEARCH_HOOK(LevelFilteringNavigationController_Setup, &GlobalNamespace::LevelFilteringNavigationController::Setup)
MAKE_HOOK_MATCH(
    LevelFilteringNavigationController_Setup,
    &GlobalNamespace::LevelFilteringNavigationController::Setup,
    void,
    GlobalNamespace::LevelFilteringNavigationController* self,
    GlobalNamespace::SongPackMask songPackMask,
    GlobalNamespace::BeatmapLevelPack* levelPackToBeSelectedAfterPresent,
    GlobalNamespace::SelectLevelCategoryViewController_LevelCategory startLevelCategory,
    bool hidePacksIfOneOrNone,
    bool enableCustomLevels
) {
    LevelFilteringNavigationController_Setup(
        self, songPackMask, levelPackToBeSelectedAfterPresent, startLevelCategory, hidePacksIfOneOrNone, enableCustomLevels
    );

    // To have interoperability with pinkcore I trigger it only if pressing play in the bss
    if (openToCustom) {
        self->____selectLevelCategoryViewController->Setup(startLevelCategory.CustomSongs, self->____enabledLevelCategories);
        openToCustom = false;
    }
}

SC2_RECORD_SEARCH_HOOK(MultiplayerLevelScenesTransitionSetupDataSO_Init, &GlobalNamespace::MultiplayerLevelScenesTransitionSetupDataSO::Init)
MAKE_HOOK_MATCH(
    MultiplayerLevelScenesTransitionSetupDataSO_Init,
    &GlobalNamespace::MultiplayerLevelScenesTransitionSetupDataSO::Init,
    void,
    GlobalNamespace::MultiplayerLevelScenesTransitionSetupDataSO* self,
    StringW gameMode,
    ByRef<::GlobalNamespace::BeatmapKey> beatmapKey,
    ::GlobalNamespace::BeatmapLevel* beatmapLevel,
    ::GlobalNamespace::IBeatmapLevelData* beatmapLevelData,
    ::GlobalNamespace::ColorScheme* overrideColorScheme,
    ::GlobalNamespace::GameplayModifiers* gameplayModifiers,
    ::GlobalNamespace::PlayerSpecificSettings* playerSpecificSettings,
    ::GlobalNamespace::PracticeSettings* practiceSettings,
    ::GlobalNamespace::AudioClipAsyncLoader* audioClipAsyncLoader,
    ::GlobalNamespace::SettingsManager* settingsManager,
    ::GlobalNamespace::BeatmapDataLoader* beatmapDataLoader,
    bool useTestNoteCutSoundEffects
) {
    // Close manager first
    manager.Close(true, false);
    MultiplayerLevelScenesTransitionSetupDataSO_Init(
        self,
        gameMode,
        beatmapKey,
        beatmapLevel,
        beatmapLevelData,
        overrideColorScheme,
        gameplayModifiers,
        playerSpecificSettings,
        practiceSettings,
        audioClipAsyncLoader,
        settingsManager,
        beatmapDataLoader,
        useTestNoteCutSoundEffects
    );
}

// Debugging function
// MAKE_HOOK_MATCH(
//     MainFlowCoordinator_DidActivate,
//     &GlobalNamespace::MainFlowCoordinator::DidActivate,
//     void,
//     GlobalNamespace::MainFlowCoordinator* self,
//     bool firstActivation,
//     bool addedToHierarchy,
//     bool screenSystemEnabling
// ) {
//     MainFlowCoordinator_DidActivate(self, firstActivation, addedToHierarchy, screenSystemEnabling);
//     static bool debugstarted = false;
//     if (!debugstarted) {
//         debugstarted = true;
//         self->StartCoroutine(custom_types::Helpers::new_coro(manager.Debug()));
//     }
// }

// Called later on in the game loading - a good time to install function hooks
BSS_EXPORT_FUNC void late_load() {
    il2cpp_functions::Init();
    BSML::Init();
    custom_types::Register::AutoRegister();

    INSTALL_HOOK(Logger, ReturnToBSS);
    INSTALL_HOOK(Logger, GameplaySetupViewController_RefreshContent);
    INSTALL_HOOK(Logger, LevelFilteringNavigationController_Setup);
    INSTALL_HOOK(Logger, MultiplayerLevelScenesTransitionSetupDataSO_Init);
    INSTALL_HOOK(Logger, MenuTransitionsHelper_RestartGame);

    // Automatic testing
    // INSTALL_HOOK(Logger, MainFlowCoordinator_DidActivate);

    manager.Init();
}
