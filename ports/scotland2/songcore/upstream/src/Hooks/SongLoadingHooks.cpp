#include "SongLoader/RuntimeSongLoader.hpp"
#include "hooking.hpp"
#include "logging.hpp"
#include "tasks.hpp"

#include "CustomJSONData.hpp"
#include "SongCore.hpp"
#include "SongLoader/CustomBeatmapLevelsRepository.hpp"

#include "GlobalNamespace/BeatmapSaveDataHelpers.hpp"
#include "GlobalNamespace/SinglePlayerLevelSelectionFlowCoordinator.hpp"
#include "GlobalNamespace/LevelFilteringNavigationController.hpp"
#include "GlobalNamespace/LevelSearchViewController.hpp"
#include "GlobalNamespace/StandardLevelInfoSaveData.hpp"
#include "GlobalNamespace/PlatformLeaderboardViewController.hpp"
#include "GlobalNamespace/LoadingControl.hpp"
#include "GlobalNamespace/OculusPlatformAdditionalContentModel.hpp"
#include "GlobalNamespace/BeatmapLevelsModel.hpp"
#include "GlobalNamespace/BeatmapLevelsEntitlementModel.hpp"
#include "GlobalNamespace/BeatmapLevelLoader.hpp"
#include "GlobalNamespace/BeatmapLevelDataLoader.hpp"
#include "GlobalNamespace/FileHelpers.hpp"

#include "System/Threading/Tasks/Task_1.hpp"
#include "System/Collections/Generic/IReadOnlyCollection_1.hpp"
#include "System/Collections/Generic/IReadOnlyList_1.hpp"
#include "System/Version.hpp"

#include "UnityEngine/Object.hpp"
#include "UnityEngine/Networking/UnityWebRequest.hpp"
#include "UnityEngine/Sprite.hpp"

#include "BGLib/Polyglot/Localization.hpp"
#include "BGLib/DotnetExtension/Collections/LRUCache_2.hpp"

#include "utf8.h"
#include <string>
#include "Utils/SaveDataVersion.hpp"

// custom songs tab is disabled by default on quest, reenable
MAKE_AUTO_HOOK_ORIG_MATCH(SinglePlayerLevelSelectionFlowCoordinator_get_enableCustomLevels, &GlobalNamespace::SinglePlayerLevelSelectionFlowCoordinator::get_enableCustomLevels, bool, GlobalNamespace::SinglePlayerLevelSelectionFlowCoordinator* self) {
    DEBUG("SinglePlayerLevelSelectionFlowCoordinator_get_enableCustomLevels override returning true");
    return true;
}

using namespace GlobalNamespace;
using namespace System::Threading;
using namespace System::Threading::Tasks;
using namespace System::Collections::Generic;

// we return our own levels repository to which we can add packs we please
MAKE_AUTO_HOOK_ORIG_MATCH(BeatmapLevelsModel_CreateAllLoadedBeatmapLevelPacks, &BeatmapLevelsModel::LoadAllBeatmapLevelPacks, void, BeatmapLevelsModel* self) {
    BeatmapLevelsModel_CreateAllLoadedBeatmapLevelPacks(self);

    auto custom = SongCore::SongLoader::CustomBeatmapLevelsRepository::New_ctor();
    auto levelPacks = self->____allLoadedBeatmapLevelsRepository->____beatmapLevelPacks;
    auto packCount = levelPacks->get_Length();
    for (int i = 0; i < packCount; i++) {
        custom->AddLevelPack(levelPacks[i]);
    }

    custom->FixBackingDictionaries();
    
    self->_allLoadedBeatmapLevelsRepository = custom;
}

// hook to ensure a beatmaps data is actually fully unloaded
MAKE_AUTO_HOOK_MATCH(BeatmapLevelLoader_HandleItemWillBeRemovedFromCache, &BeatmapLevelLoader::HandleItemWillBeRemovedFromCache, void, BeatmapLevelLoader* self, StringW beatmapLevelId, IBeatmapLevelData* beatmapLevel) {
    DEBUG("BeatmapLevelLoader_HandleItemWillBeRemovedFromCache");
    BeatmapLevelLoader_HandleItemWillBeRemovedFromCache(self, beatmapLevelId, beatmapLevel);

    if (beatmapLevelId.starts_with(u"custom_level_"))
        self->_beatmapLevelDataLoader->TryUnload(beatmapLevelId);
}

// ifd out on quest, just check for custom_level_ prepend and say owned if so
MAKE_AUTO_HOOK_ORIG_MATCH(BeatmapLevelsEntitlementModel_GetLevelEntitlementStatusAsync, &BeatmapLevelsEntitlementModel::GetLevelEntitlementStatusAsync, Task_1<EntitlementStatus>*, BeatmapLevelsEntitlementModel* self, StringW levelID, CancellationToken token) {
    if (levelID.starts_with(u"custom_level_")) {
        return Task_1<EntitlementStatus>::FromResult(EntitlementStatus::Owned);
    }
    return BeatmapLevelsEntitlementModel_GetLevelEntitlementStatusAsync(self, levelID, token);
}

// ifd out on quest, just check for custom_levelPack_ prepend and say owned if so
MAKE_AUTO_HOOK_ORIG_MATCH(BeatmapLevelsEntitlementModel_GetPackEntitlementStatusAsync, &BeatmapLevelsEntitlementModel::GetPackEntitlementStatusAsync, Task_1<EntitlementStatus>*, BeatmapLevelsEntitlementModel* self, StringW levelPackID, CancellationToken token) {
    if (levelPackID.starts_with(u"custom_levelPack_")) {
        return Task_1<EntitlementStatus>::FromResult(EntitlementStatus::Owned);
    }
    return BeatmapLevelsEntitlementModel_GetPackEntitlementStatusAsync(self, levelPackID, token);
}

MAKE_AUTO_HOOK_ORIG_MATCH(BeatmapLevelsModel_ReloadCustomLevelPackCollectionAsync, &BeatmapLevelsModel::ReloadCustomLevelPackCollectionAsync, Task_1<GlobalNamespace::BeatmapLevelsRepository*>*, BeatmapLevelsModel* self, CancellationToken cancellationToken) {
    // if songs are loaded and not refreshing, return the repo with fromresult
    if (SongCore::API::Loading::AreSongsLoaded() && !SongCore::API::Loading::AreSongsRefreshing()) {
        return Task_1<GlobalNamespace::BeatmapLevelsRepository*>::FromResult(static_cast<GlobalNamespace::BeatmapLevelsRepository*>(SongCore::API::Loading::GetCustomBeatmapLevelsRepository()));
    }

    // levels weren't loaded or we are refreshing right now, so make the user wait
    return SongCore::StartTask<GlobalNamespace::BeatmapLevelsRepository*>([](SongCore::CancellationToken cancelToken) -> GlobalNamespace::BeatmapLevelsRepository* {
        using namespace std::chrono_literals;
        auto loader = SongCore::SongLoader::RuntimeSongLoader::get_instance();
        while (loader->AreSongsRefreshing && !cancelToken.IsCancellationRequested) std::this_thread::sleep_for(100ms);
        return loader->CustomBeatmapLevelsRepository;
    }, std::forward<SongCore::CancellationToken>(cancellationToken));
}

// gets the char16 representation of the 2 nibbles that fit 1 char
std::pair<char16_t, char16_t> getByteChars(char c) {
    static char16_t nibbleToChar[] = u"0123456789abcdef";
    auto lower = c & 0b1111;
    auto upper = (c >> 4) & 0b1111;

    return {nibbleToChar[lower], nibbleToChar[upper]};
}

// manually implement escaping the filepath url. as it turns out most of the code the game does is good except it escapes / as well which breaks filepaths, so we just escape with a limited charset
std::u16string escape(std::u16string_view url) {
    static char16_t forbidden[] = u"@&;:<>=?\"'\\!#%+$,{}|^[]`";
    static auto forbiddenEnd = forbidden + (sizeof(forbidden) / sizeof(char16_t));
    static auto isForbidden = [](char16_t c) { return std::find(forbidden, forbiddenEnd, c) != forbiddenEnd; };

    // i'd have preferred to use wstringstream but it doesn't convert to u16 as easily
    std::u16string escaped;
    escaped.reserve(url.size());

    for (auto c : url) {
        if (isForbidden(c)) {
            escaped.push_back(u'%');
            // we lose width here but all forbidden chars are only as big as 1 byte (char) anyway
            auto [lc, uc] = getByteChars(static_cast<char>(c));
            escaped.push_back(uc);
            escaped.push_back(lc);
        } else {
            escaped.push_back(c);
        }
    }

    return escaped;
}

// reading files sucks for file paths
MAKE_AUTO_HOOK_ORIG_MATCH(FileHelpers_GetEscapedURLForFilePath, &FileHelpers::GetEscapedURLForFilePath, StringW, StringW filePath) {
    std::u16string_view view(filePath);
    if (view.find(u"://") != std::string::npos) { // check if it's already a URL
        return filePath;
    }
    return fmt::format("file://{}", utf8::utf16to8(escape(filePath)));
}

// get the level data async
// TODO: rip out this level data loading from the SongLoader/LevelLoader.cpp and implement it async here to improve level loading speed and not do redundant things here
MAKE_AUTO_HOOK_ORIG_MATCH(BeatmapLevelsModel_LoadBeatmapLevelDataAsync, &BeatmapLevelsModel::LoadBeatmapLevelDataAsync, Task_1<LoadBeatmapLevelDataResult>*, BeatmapLevelsModel* self, StringW levelID, GlobalNamespace::BeatmapLevelDataVersion beatmapLevelDataVersion, CancellationToken token) {
    if (levelID.starts_with(u"custom_level_")) {
        return SongCore::StartTask<LoadBeatmapLevelDataResult>([=](SongCore::CancellationToken token){
            static auto Error = LoadBeatmapLevelDataResult(true, nullptr);
            auto level = SongCore::API::Loading::GetLevelByLevelID(static_cast<std::string>(levelID));
            if (!level || token.IsCancellationRequested) return Error;
            auto data = level->beatmapLevelData;
            if (!data) return Error;
            return LoadBeatmapLevelDataResult::Success(data);
        }, std::forward<SongCore::CancellationToken>(token));
    }

    return BeatmapLevelsModel_LoadBeatmapLevelDataAsync(self, levelID, beatmapLevelDataVersion, token);
}

// get the level data async
// TODO: rip out this level data loading from the SongLoader/LevelLoader.cpp and implement it async here to improve level loading speed and not do redundant things here
MAKE_AUTO_HOOK_ORIG_MATCH(BeatmapLevelsModel_CheckBeatmapLevelDataExistsAsync, &BeatmapLevelsModel::CheckBeatmapLevelDataExistsAsync, Task_1<bool>*, BeatmapLevelsModel* self, StringW levelID, GlobalNamespace::BeatmapLevelDataVersion beatmapLevelDataVersion, CancellationToken token) {
    if (levelID.starts_with(u"custom_level_")) {
        return SongCore::StartTask<bool>([=](SongCore::CancellationToken token){
            auto level = SongCore::API::Loading::GetLevelByLevelID(static_cast<std::string>(levelID));
            if (!level) return false;
            return level->beatmapLevelData != nullptr;
        }, std::forward<SongCore::CancellationToken>(token));
    }

    return BeatmapLevelsModel_CheckBeatmapLevelDataExistsAsync(self, levelID, beatmapLevelDataVersion, token);
}

// override getting beatmap level if original method didn't return anything
MAKE_AUTO_HOOK_MATCH(BeatmapLevelsModel_GetBeatmapLevel, &BeatmapLevelsModel::GetBeatmapLevel, BeatmapLevel*, BeatmapLevelsModel* self, StringW levelID) {
    auto result = BeatmapLevelsModel_GetBeatmapLevel(self, levelID);
    if (!result && levelID.starts_with(u"custom_level_")) {
        result = SongCore::API::Loading::GetLevelByLevelID(static_cast<std::string>(levelID));
    }

    return result;
}

// input array can have duplicates, we change the input for that reason
MAKE_AUTO_HOOK_MATCH(BeatmapLevelPack_CreateLevelPackForFiltering, &BeatmapLevelPack::CreateLevelPackForFiltering, BeatmapLevelPack*, ArrayW<BeatmapLevel*> beatmapLevels) {
    // a set considers 2 elements equivalent if !(a < b) && !(b < a) which for pointers is ideal since 2 same pointers are the same level
    std::set<BeatmapLevel*> levelSet;
    for (auto level : beatmapLevels) levelSet.insert(level);

    ArrayW<BeatmapLevel*> uniqueLevels(levelSet.size());
    for (auto idx = 0; auto level : levelSet) uniqueLevels[idx++] = level;
    INFO("Filtered levels for uniqueness, had an array of size {}, now {}", beatmapLevels.size(), uniqueLevels.size());

    return BeatmapLevelPack_CreateLevelPackForFiltering(uniqueLevels);
}

// fix error message on custom levels, as the game doesn't correctly display the error message here on quest due to an ifd out check
MAKE_AUTO_HOOK_ORIG_MATCH(
    PlatformLeaderboardViewController_Refresh,
    &PlatformLeaderboardViewController::Refresh,
    void,
    PlatformLeaderboardViewController* self,
    bool showLoadingIndicator,
    bool clear
) {
    auto levelId = self->_beatmapKey.levelId;
    if (levelId.starts_with(u"custom_level_")) {
        self->StopAllCoroutines();
        self->ClearContent();
        self->_loadingControl->ShowText(BGLib::Polyglot::Localization::Get("CUSTOM_LEVELS_LEADERBOARDS_NOT_SUPPORTED"), false);
    } else {
        PlatformLeaderboardViewController_Refresh(self, showLoadingIndicator, clear);
    }
}

// keep level search and filters when refreshing
MAKE_AUTO_HOOK_MATCH(
    LevelFilteringNavigationController_UpdateSecondChildControllerContent,
    &LevelFilteringNavigationController::UpdateSecondChildControllerContent,
    void,
    LevelFilteringNavigationController* self,
    SelectLevelCategoryViewController::LevelCategory levelCategory
) {
    std::optional<LevelFilter> searchFilter = std::nullopt;
    bool inSearch = levelCategory == SelectLevelCategoryViewController::LevelCategory::Favorites
                    || levelCategory == SelectLevelCategoryViewController::LevelCategory::All;
    // cancellationTokenSource is only non null during UpdateCustomSongs
    if (inSearch && self->_cancellationTokenSource && self->_levelSearchViewController) {
        searchFilter = self->_levelSearchViewController->_currentSearchFilter;
        if (searchFilter->searchText == nullptr) searchFilter->searchText = "";
    }
    // packs are not initialized, so filters aren't yet either
    if (inSearch && !self->_allBeatmapLevelPacks)
        self->_levelSearchViewController->ResetAllFilterSettings(levelCategory == SelectLevelCategoryViewController::LevelCategory::Favorites);

    LevelFilteringNavigationController_UpdateSecondChildControllerContent(self, levelCategory);

    if (searchFilter) self->_levelSearchViewController->Refresh(byref(*searchFilter));
}


// Fix levels using regular name instead of serialized name.
MAKE_AUTO_HOOK_MATCH(
    EnvironmentsListModel_GetEnvironmentInfoBySerializedName,
    &EnvironmentsListModel::GetEnvironmentInfoBySerializedName,
    UnityW<GlobalNamespace::EnvironmentInfoSO>,
    EnvironmentsListModel* self,
    StringW environmentSerializedName
) {
    auto result = EnvironmentsListModel_GetEnvironmentInfoBySerializedName(self, environmentSerializedName);
    //Fix here
    if (!result) {
        //This should be a rare case
        auto envList = ListW<UnityW<GlobalNamespace::EnvironmentInfoSO>>::New();
        envList->AddRange(self->environmentInfos->i___System__Collections__Generic__IEnumerable_1_T_());
        for(auto& env : envList) {
            if (env->environmentName == environmentSerializedName) {
                result = env;
            }
        }
    }

    return result;
}


// https://github.com/Kylemc1413/SongCore/pull/148/files
// This partially fixes a bug that was introduced in v1.36.0, which saves null covers when the loading operation is canceled.
// It still shows the default cover at times, but at least it reloads it now.
// TODO: Remove when fixed.
// Generic Patching ):

using LRUCacheInst = BGLib::DotnetExtension::Collections::LRUCache_2<StringW, UnityEngine::Sprite*>;

MAKE_HOOK(LRUCache_Add, nullptr, void, LRUCacheInst* self, StringW key, UnityEngine::Sprite* value, MethodInfo* methodInfo) {
    if (value != nullptr)
        LRUCache_Add(self, key, value, methodInfo);
}

struct Auto_Hook_LRUCache_Add {
    static void Auto_Hook_LRUCache_Add_Install() {
        static constexpr auto logger = Paper::ConstLoggerContext("SongCore_Install_LRUCache_Add");
        static auto addr = il2cpp_utils::FindMethodUnsafe(classof(LRUCacheInst*), "Add", 2);
        INSTALL_HOOK_DIRECT(logger, LRUCache_Add, (void*) addr->methodPointer);
    }
    Auto_Hook_LRUCache_Add() {
        SongCore::Hooking::AddInstallFunc(Auto_Hook_LRUCache_Add_Install);
    }
};
static Auto_Hook_LRUCache_Add Auto_Hook_Instance_LRUCache_Add;
