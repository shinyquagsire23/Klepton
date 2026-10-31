#include "Main.hpp"

#include "GlobalNamespace/AnnotatedBeatmapLevelCollectionCell.hpp"
#include "GlobalNamespace/AnnotatedBeatmapLevelCollectionsGridView.hpp"
#include "GlobalNamespace/AnnotatedBeatmapLevelCollectionsGridViewAnimator.hpp"
#include "GlobalNamespace/AnnotatedBeatmapLevelCollectionsViewController.hpp"
#include "GlobalNamespace/BeatmapDifficultySegmentedControlController.hpp"
#include "GlobalNamespace/BeatmapLevel.hpp"
#include "GlobalNamespace/EnvironmentInfoSO.hpp"
#include "GlobalNamespace/GridView.hpp"
#include "GlobalNamespace/IEntitlementModel.hpp"
#include "GlobalNamespace/LevelCollectionNavigationController.hpp"
#include "GlobalNamespace/LevelCollectionTableView.hpp"
#include "GlobalNamespace/LevelCollectionViewController.hpp"
#include "GlobalNamespace/LevelFilteringNavigationController.hpp"
#include "GlobalNamespace/LevelPackDetailViewController.hpp"
#include "GlobalNamespace/MenuTransitionsHelper.hpp"
#include "GlobalNamespace/PageControl.hpp"
#include "GlobalNamespace/PlayerData.hpp"
#include "GlobalNamespace/PlayerDataModel.hpp"
#include "GlobalNamespace/SongPreviewPlayer.hpp"
#include "GlobalNamespace/StandardLevelDetailViewController.hpp"
#include "HMUI/FlowCoordinator.hpp"
#include "HMUI/InputFieldView.hpp"
#include "HMUI/ScrollView.hpp"
#include "HMUI/TableView.hpp"
#include "HMUI/ViewController.hpp"
#include "PlaylistCore.hpp"
#include "ResettableStaticPtr.hpp"
#include "Settings.hpp"
#include "System/Action_1.hpp"
#include "System/Action_2.hpp"
#include "Tweening/Vector2Tween.hpp"
#include "Types/Config.hpp"
#include "Types/Scroller.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Resources.hpp"
#include "Zenject/DiContainer.hpp"
#include "Zenject/StaticMemoryPool_7.hpp"
#include "beatsaber-hook/shared/config/config-utils.hpp"
#include "beatsaber-hook/shared/utils/hooking.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"
#include "bsml/shared/BSML.hpp"
#include "songcore/shared/SongCore.hpp"

using namespace GlobalNamespace;
using namespace PlaylistCore;

modloader::ModInfo modInfo = {"PlaylistCore", VERSION, 0};
modloader::ModInfo managerModInfo = {"PlaylistManager", VERSION, 0};

bool hasManager;

std::string GetPlaylistsPath() {
    static std::string playlistsPath(getDataDir(managerModInfo) + "Playlists");
    return playlistsPath;
}

std::string GetCoversPath() {
    static std::string coversPath(getDataDir(managerModInfo) + "Covers");
    return coversPath;
}

#ifdef SC2_AOT_HOOKS
struct SC2PlaylistHookTarget { uint64_t address; uint32_t originalHook,reserved; char name[128]; };
static_assert(sizeof(SC2PlaylistHookTarget)==144);
#define SC2_PLAYLIST_HOOK(name_, mPtr, orig_) \
    static const SC2PlaylistHookTarget sc2_target_##name_ __attribute__((used,section(".sc2_hooks"))) = { \
        ::il2cpp_utils::il2cpp_type_check::MetadataGetter<mPtr>::addrs, orig_, 0, #name_ };
#else
#define SC2_PLAYLIST_HOOK(name_, mPtr, orig_)
#endif

// override header cell behavior and change no data prefab
SC2_PLAYLIST_HOOK(LevelCollectionViewController_SetData, &LevelCollectionViewController::SetData, 1)
MAKE_HOOK_MATCH(
    LevelCollectionViewController_SetData,
    &LevelCollectionViewController::SetData,
    void,
    LevelCollectionViewController* self,
    ArrayW<BeatmapLevel*> beatmapLevels,
    StringW headerText,
    UnityEngine::Sprite* headerSprite,
    bool sortLevels,
    bool sortPreviewBeatmapLevels,
    UnityEngine::GameObject* noDataInfoPrefab
) {
    // only check for null strings, not empty
    // string will be null for level collections but not level packs
    self->_showHeader = (bool) headerText;
    // copy base game method implementation
    self->_levelCollectionTableView->Init(headerText, headerSprite);
    if (self->_noDataInfoGO) {
        UnityEngine::Object::Destroy(self->_noDataInfoGO);
        self->_noDataInfoGO = nullptr;
    }
    // also override check for empty collection
    if (beatmapLevels) {
        self->_levelCollectionTableView->get_gameObject()->SetActive(true);
        self->_levelCollectionTableView->SetData(
            reinterpret_cast<System::Collections::Generic::IReadOnlyList_1<BeatmapLevel*>*>(beatmapLevels.convert()),
            self->_playerDataModel->playerData->favoritesLevelIds,
            sortLevels,
            sortPreviewBeatmapLevels
        );
        self->_levelCollectionTableView->RefreshLevelsAvailability();
    } else {
        self->_levelCollectionTableView->SetData(
            ListW<BeatmapLevel*>::New()->i___System__Collections__Generic__IReadOnlyList_1_T_(),
            self->_playerDataModel->playerData->favoritesLevelIds,
            sortLevels,
            sortPreviewBeatmapLevels
        );
        if (noDataInfoPrefab)
            self->_noDataInfoGO = self->_container->InstantiatePrefab(noDataInfoPrefab, self->_noDataInfoContainer);
        // change no custom songs text if playlists exist
        // because if they do then the only way to get here with that specific no data indicator is to have no playlists filtered
        static ConstString message("No playlists are contained in the filtering options selected.");
        if (GetLoadedPlaylists().size() > 0 && !SongCore::API::Loading::GetAllLevels().empty() &&
            noDataInfoPrefab == FindComponent<LevelFilteringNavigationController*>()->_emptyCustomSongListInfoPrefab.ptr())
            self->_noDataInfoGO->GetComponentInChildren<TMPro::TextMeshProUGUI*>()->set_text(message);
        self->_levelCollectionTableView->get_gameObject()->SetActive(false);
    }
    if (self->get_isInViewControllerHierarchy()) {
        if (self->_showHeader)
            self->_levelCollectionTableView->SelectLevelPackHeaderCell();
        else
            self->_levelCollectionTableView->ClearSelection();
        self->_songPreviewPlayer->CrossfadeToDefault();
    }
}

// fix playlists opening with exactly 7 playlists
SC2_PLAYLIST_HOOK(AnnotatedBeatmapLevelCollectionsGridView_OnPointerEnter, &AnnotatedBeatmapLevelCollectionsGridView::OnPointerEnter, 0)
MAKE_HOOK_MATCH(
    AnnotatedBeatmapLevelCollectionsGridView_OnPointerEnter,
    &AnnotatedBeatmapLevelCollectionsGridView::OnPointerEnter,
    void,
    AnnotatedBeatmapLevelCollectionsGridView* self,
    UnityEngine::EventSystems::PointerEventData* eventData
) {
    AnnotatedBeatmapLevelCollectionsGridView_OnPointerEnter(self, eventData);

    if (self->_gridView->rowCount == 1 && self->_gridView->columnCount > 6) {
        self->didOpenAnnotatedBeatmapLevelCollectionEvent->Invoke();
        self->_animator->AnimateOpen(true);
    }
}

// now fix playlists closing with exactly 7 playlists
SC2_PLAYLIST_HOOK(AnnotatedBeatmapLevelCollectionsGridView_OnPointerExit, &AnnotatedBeatmapLevelCollectionsGridView::OnPointerExit, 0)
MAKE_HOOK_MATCH(
    AnnotatedBeatmapLevelCollectionsGridView_OnPointerExit,
    &AnnotatedBeatmapLevelCollectionsGridView::OnPointerExit,
    void,
    AnnotatedBeatmapLevelCollectionsGridView* self,
    UnityEngine::EventSystems::PointerEventData* eventData
) {
    AnnotatedBeatmapLevelCollectionsGridView_OnPointerExit(self, eventData);

    if (self->_gridView->rowCount == 1 && self->_gridView->columnCount > 6)
        self->CloseLevelCollection(true);
}

// add scrolling to playlist selector
SC2_PLAYLIST_HOOK(AnnotatedBeatmapLevelCollectionsGridView_OnEnable, &AnnotatedBeatmapLevelCollectionsGridView::OnEnable, 0)
MAKE_HOOK_MATCH(
    AnnotatedBeatmapLevelCollectionsGridView_OnEnable,
    &AnnotatedBeatmapLevelCollectionsGridView::OnEnable,
    void,
    AnnotatedBeatmapLevelCollectionsGridView* self
) {
    AnnotatedBeatmapLevelCollectionsGridView_OnEnable(self);

    if (!self->GetComponent<Scroller*>())
        self->get_gameObject()->AddComponent<Scroller*>()->Init(self->_animator->_contentTransform);
}

// make the playlist opening animation work better with the playlist scroller
SC2_PLAYLIST_HOOK(AnnotatedBeatmapLevelCollectionsGridViewAnimator_AnimateOpen, &AnnotatedBeatmapLevelCollectionsGridViewAnimator::AnimateOpen, 0)
MAKE_HOOK_MATCH(
    AnnotatedBeatmapLevelCollectionsGridViewAnimator_AnimateOpen,
    &AnnotatedBeatmapLevelCollectionsGridViewAnimator::AnimateOpen,
    void,
    AnnotatedBeatmapLevelCollectionsGridViewAnimator* self,
    bool animated
) {
    // store actual values to avoid breaking things when closing
    int rowCount = self->_rowCount;
    int selectedRow = self->_selectedRow;

    // lock height to specific value
    self->_rowCount = 5;
    self->_selectedRow = 0;

    AnnotatedBeatmapLevelCollectionsGridViewAnimator_AnimateOpen(self, animated);

    // prevent modification of content transform as it overrides the scroll view
    Tweening::Vector2Tween::getStaticF_Pool()->Despawn(self->_contentPositionTween);
    self->_contentPositionTween = nullptr;

    self->_rowCount = rowCount;
    self->_selectedRow = selectedRow;
}

// prevent download icon showing up on empty custom playlists unless manager is changing the behavior
SC2_PLAYLIST_HOOK(AnnotatedBeatmapLevelCollectionCell_RefreshAvailabilityAsync, &AnnotatedBeatmapLevelCollectionCell::RefreshAvailabilityAsync, 0)
MAKE_HOOK_MATCH(
    AnnotatedBeatmapLevelCollectionCell_RefreshAvailabilityAsync,
    &AnnotatedBeatmapLevelCollectionCell::RefreshAvailabilityAsync,
    void,
    AnnotatedBeatmapLevelCollectionCell* self,
    IEntitlementModel* entitlementModel
) {
    AnnotatedBeatmapLevelCollectionCell_RefreshAvailabilityAsync(self, entitlementModel);

    if (hasManager)
        return;

    auto pack = self->_beatmapLevelPack;
    if (pack) {
        auto playlist = GetPlaylistWithPrefix(pack->packID);
        if (playlist)
            self->SetDownloadIconVisible(false);
    }
}

// throw away objects on a soft restart
SC2_PLAYLIST_HOOK(MenuTransitionsHelper_RestartGame, &MenuTransitionsHelper::RestartGame, 0)
MAKE_HOOK_MATCH(
    MenuTransitionsHelper_RestartGame,
    &MenuTransitionsHelper::RestartGame,
    void,
    MenuTransitionsHelper* self,
    System::Action_1<Zenject::DiContainer*>* finishCallback
) {
    for (auto scroller : UnityEngine::Resources::FindObjectsOfTypeAll<Scroller*>())
        UnityEngine::Object::Destroy(scroller);

    ClearLoadedImages();

    hasLoaded = false;

    MenuTransitionsHelper_RestartGame(self, finishCallback);

    ResettableStaticPtr::resetAll();
}

// override to prevent crashes due to opening with a null level pack
#define COMBINE(delegate1, selfMethodName, ...) delegate1 = (std::decay_t<decltype(delegate1)>) System::Delegate::Combine(delegate1, System::Delegate::CreateDelegate(csTypeOf(std::decay_t<decltype(delegate1)>), self, #selfMethodName));
SC2_PLAYLIST_HOOK(LevelCollectionNavigationController_DidActivate, &LevelCollectionNavigationController::DidActivate, 1)
MAKE_HOOK_MATCH(
    LevelCollectionNavigationController_DidActivate,
    &LevelCollectionNavigationController::DidActivate,
    void,
    LevelCollectionNavigationController* self,
    bool firstActivation,
    bool addedToHierarchy,
    bool screenSystemEnabling
) {
    if (addedToHierarchy) {
        COMBINE(self->_levelCollectionViewController->didSelectLevelEvent, HandleLevelCollectionViewControllerDidSelectLevel);
        COMBINE(self->_levelCollectionViewController->didSelectHeaderEvent, HandleLevelCollectionViewControllerDidSelectPack);
        COMBINE(self->_levelDetailViewController->didPressActionButtonEvent, HandleLevelDetailViewControllerDidPressActionButton);
        COMBINE(self->_levelDetailViewController->didPressPracticeButtonEvent, HandleLevelDetailViewControllerDidPressPracticeButton);
        COMBINE(self->_levelDetailViewController->didChangeDifficultyBeatmapEvent, HandleLevelDetailViewControllerDidChangeDifficultyBeatmap);
        COMBINE(self->_levelDetailViewController->didChangeContentEvent, HandleLevelDetailViewControllerDidPresentContent);
        COMBINE(self->_levelDetailViewController->didPressOpenLevelPackButtonEvent, HandleLevelDetailViewControllerDidPressOpenLevelPackButton);
        COMBINE(self->_levelDetailViewController->levelFavoriteStatusDidChangeEvent, HandleLevelDetailViewControllerLevelFavoriteStatusDidChange);
        if (self->_beatmapLevelToBeSelectedAfterPresent) {
            self->_levelCollectionViewController->SelectLevel(self->_beatmapLevelToBeSelectedAfterPresent);
            self->SetChildViewController(self->_levelCollectionViewController);
            self->_beatmapLevelToBeSelectedAfterPresent = nullptr;
        } else {
            // override here so that the pack detail controller will not be shown if no pack is selected
            if (self->_levelPack) {
                ArrayW<HMUI::ViewController*> children{2};
                children[0] = self->_levelCollectionViewController;
                children[1] = self->_levelPackDetailViewController;
                self->SetChildViewControllers(children);
            } else
                self->SetChildViewController(self->_levelCollectionViewController);
        }
    } else if (self->_loading) {
        self->ClearChildViewControllers();
    } else if (self->_hideDetailViewController) {
        self->SetChildViewController(self->_levelCollectionViewController);
        self->_hideDetailViewController = false;
    }
}

extern "C" void setup(CModInfo* info) {
    *info = modInfo.to_c();

    Paper::Logger::RegisterFileContextId(MOD_ID);

    auto playlistsPath = GetPlaylistsPath();
    if (!direxists(playlistsPath))
        mkpath(playlistsPath);

    LOG_INFO("Playlist path is {}", playlistsPath);

    auto coversPath = GetCoversPath();
    if (!direxists(coversPath))
        mkpath(coversPath);

    getConfig().Init(modInfo);
}

static void InstallPlaylistHooks() {
    INSTALL_HOOK_ORIG(logger, LevelCollectionViewController_SetData);
    INSTALL_HOOK(logger, AnnotatedBeatmapLevelCollectionsGridView_OnPointerEnter);
    INSTALL_HOOK(logger, AnnotatedBeatmapLevelCollectionsGridView_OnPointerExit);
    INSTALL_HOOK(logger, AnnotatedBeatmapLevelCollectionsGridView_OnEnable);
    INSTALL_HOOK(logger, AnnotatedBeatmapLevelCollectionsGridViewAnimator_AnimateOpen);
    INSTALL_HOOK(logger, AnnotatedBeatmapLevelCollectionCell_RefreshAvailabilityAsync);
    INSTALL_HOOK(logger, MenuTransitionsHelper_RestartGame);
    INSTALL_HOOK_ORIG(logger, LevelCollectionNavigationController_DidActivate);
}

#if defined(SC2_AOT_HOOKS) && SC2_DIAGNOSTICS
extern "C" void sc2_test_playlistcore_install_hooks() { InstallPlaylistHooks(); }
#endif

extern "C" void late_load() {
    LOG_INFO("Starting PlaylistCore installation...");
    il2cpp_functions::Init();
    custom_types::Register::AutoRegister();

    BSML::Init();
    BSML::Register::RegisterMenuButton("Reload Playlists", "Reloads all playlists!", [] { SongCore::API::Loading::RefreshLevelPacks(); });
    BSML::Register::RegisterSettingsMenu<SettingsViewController*>("Playlist Core");

    auto managerCInfo = managerModInfo.to_c();
    hasManager = modloader_require_mod(&managerCInfo, CMatchType::MatchType_IdOnly);

    InstallPlaylistHooks();

    SongCore::API::Loading::GetCustomLevelPacksWillRefreshEvent().addCallback(
        [](SongCore::SongLoader::CustomBeatmapLevelsRepository* customBeatmapLevelsRepository) { LoadPlaylists(customBeatmapLevelsRepository, true); }
    );

    LOG_INFO("Successfully installed PlaylistCore!");
}
